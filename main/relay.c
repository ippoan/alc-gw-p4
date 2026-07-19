// C212 の RTSP stream2 (H.264, 無トランスコード) を pull し、WHIP publish
// する中継本体 (alc-gw-p4#1)。ippoan/alc-gw の internal/whip/session.go と
// 同じ設計: 1 回の接続試行 (run_once) を、失敗のたびに指数バックオフ
// (1s→60s) で再試行するループで包む。
//
// esp_peer は素の API を直接叩く (esp_webrtc の高レベルラッパーは
// esp_capture 経由のカメラキャプチャ専用で、外部ソースの符号化済み
// フレームを注入する経路が無いことをソース調査で確認済み)。
// WHIP のシグナリング (POST/DELETE) は whip_client.c で自前実装する。
//
// 【実機未検証、要確認】esp_media_protocols の RTSP_CLIENT_PLAY +
// video_enable=true の組み合わせは Espressif 公式サンプルに実例が無く、
// receive_video の NAL 境界 (1 コールバック = 1 NAL か 1 アクセスユニットか)
// と Annex-B 開始コードの有無が未確認。実機でのダンプ確認が必要。
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_rtsp.h"

#include "whip_client.h"
#include "relay.h"

static const char *TAG = "relay";

#define EVT_CODEC_CONFIRMED   (1 << 0)
#define EVT_CODEC_UNSUPPORTED (1 << 1)
#define EVT_PEER_CONNECTED    (1 << 2)
#define EVT_PEER_FAILED       (1 << 3)
#define EVT_RTSP_ERROR        (1 << 4)

// docs/whip-convention.md: 再接続は 1s → 2s → … → 60s 上限
#define RECONNECT_MIN_BACKOFF_MS 1000
#define RECONNECT_MAX_BACKOFF_MS 60000
#define RTSP_DESCRIBE_TIMEOUT_MS 10000
// esp_peer_main_loop を pump する間隔。公式サンプル peer_demo.c と同じ駆動方法
#define PEER_MAIN_LOOP_INTERVAL_MS 10

typedef struct {
    EventGroupHandle_t events;
    esp_peer_handle_t peer;
    whip_client_t whip;
    esp_rtsp_handle_t rtsp;
} relay_session_t;

// ---------------------------------------------------------------------
// esp_peer コールバック
// ---------------------------------------------------------------------

static int on_peer_state(esp_peer_state_t state, void *ctx) {
    relay_session_t *s = (relay_session_t *)ctx;
    ESP_LOGI(TAG, "peer state = %d", (int)state);
    switch (state) {
    case ESP_PEER_STATE_CONNECTED:
        xEventGroupSetBits(s->events, EVT_PEER_CONNECTED);
        break;
    case ESP_PEER_STATE_CONNECT_FAILED:
    case ESP_PEER_STATE_DISCONNECTED:
    case ESP_PEER_STATE_CLOSED:
        xEventGroupSetBits(s->events, EVT_PEER_FAILED);
        break;
    default:
        break;
    }
    return 0;
}

// esp_peer が local SDP (offer) を生成すると呼ばれる。WHIP は non-trickle
// 前提 (docs/whip-convention.md) — offer 生成時に一度だけ WHIP POST を叩き、
// answer を esp_peer_send_msg で送り返す。trickle candidate 等の他メッセージ
// 種別はこの v1 スコープでは無視する。
static int on_peer_msg(esp_peer_msg_t *msg, void *ctx) {
    relay_session_t *s = (relay_session_t *)ctx;
    if (msg == NULL || msg->type != ESP_PEER_MSG_TYPE_SDP) {
        return 0;
    }

    char *answer_sdp = NULL;
    esp_err_t err = whip_client_publish(&s->whip, CONFIG_RELAY_WHIP_URL, CONFIG_RELAY_WHIP_TOKEN,
                                         (const char *)msg->data, &answer_sdp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHIP publish failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(s->events, EVT_PEER_FAILED);
        return -1;
    }

    esp_peer_msg_t answer_msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)answer_sdp,
        .size = strlen(answer_sdp),
    };
    int ret = esp_peer_send_msg(s->peer, &answer_msg);
    free(answer_sdp);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_peer_send_msg(answer) failed: %d", ret);
        xEventGroupSetBits(s->events, EVT_PEER_FAILED);
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------
// esp_rtsp コールバック
// ---------------------------------------------------------------------

static int on_rtsp_stream_codec(esp_rtsp_aud_info_t *aud_info, esp_rtsp_video_info_t *vid_info, void *ctx) {
    relay_session_t *s = (relay_session_t *)ctx;
    if (vid_info == NULL || vid_info->vcodec != RTSP_VCODEC_H264) {
        ESP_LOGE(TAG, "camera stream is not H264 (v1 は H264 のみ対応、docs/whip-convention.md 参照)");
        xEventGroupSetBits(s->events, EVT_CODEC_UNSUPPORTED);
        return -1;
    }
    ESP_LOGI(TAG, "rtsp video: H264 %dx%d @%dfps", vid_info->width, vid_info->height, vid_info->fps);
    xEventGroupSetBits(s->events, EVT_CODEC_CONFIRMED);
    return 0;
}

// カメラの H.264 アクセスユニットを無トランスコードのまま esp_peer_send_video
// へ転送する。RTP 由来の pts は esp_rtsp から得られないため、esp_timer から
// 自前の単調増加 pts を合成する (esp_peer 公式サンプルの
// "av_stream pts out of sync, use system pts instead" と同じ手法)。
static int on_rtsp_receive_video(unsigned char *data, int len, void *ctx) {
    relay_session_t *s = (relay_session_t *)ctx;
    if (s->peer == NULL || len <= 0) {
        return 0;
    }
    esp_peer_video_frame_t frame = {
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
        .data = data,
        .size = len,
    };
    int err = esp_peer_send_video(s->peer, &frame);
    if (err != 0) {
        ESP_LOGW(TAG, "esp_peer_send_video failed: %d", err);
    }
    return 0;
}

// ---------------------------------------------------------------------
// セッションライフサイクル
// ---------------------------------------------------------------------

static esp_err_t start_rtsp(relay_session_t *s) {
    // esp_rtsp が内部で保持し続けるポインタなので static にして関数外まで
    // 生存させる (このプロセス内で複数セッションが並行することは無い前提)
    static esp_rtsp_data_cb_t data_cb;
    memset(&data_cb, 0, sizeof(data_cb));
    data_cb.receive_video = on_rtsp_receive_video;
    data_cb.stream_codec = on_rtsp_stream_codec;

    esp_rtsp_config_t cfg = {
        .ctx = s,
        .uri = CONFIG_RELAY_RTSP_URL,
        .video_enable = true,
        .audio_enable = false, // v1 スコープ: 映像のみ (docs/whip-convention.md)
        .mode = RTSP_CLIENT_PLAY,
        .trans = RTSP_TRANSPORT_TCP, // interleaved。UDP側ポート通知が不要になる分シンプル
        .data_cb = &data_cb,
        .stack_size = 8192,
        .task_prio = 5,
    };
    s->rtsp = esp_rtsp_client_start(&cfg);
    if (s->rtsp == NULL) {
        ESP_LOGE(TAG, "esp_rtsp_client_start failed");
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(s->events, EVT_CODEC_CONFIRMED | EVT_CODEC_UNSUPPORTED,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(RTSP_DESCRIBE_TIMEOUT_MS));
    if (bits & EVT_CODEC_UNSUPPORTED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!(bits & EVT_CODEC_CONFIRMED)) {
        ESP_LOGE(TAG, "rtsp: timed out waiting for stream codec info");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t start_peer(relay_session_t *s) {
    // LAN 内 SFU 限定運用なら ice_use_lite_mode=true にできる
    // (docs/whip-convention.md の ICE 節、whip_demo の WEBRTC_USE_ICE_LITE 相当)。
    // v1 は拠点外 SFU 到達を前提に false のまま
    static esp_peer_default_cfg_t default_cfg;
    memset(&default_cfg, 0, sizeof(default_cfg));

    esp_peer_cfg_t cfg = {
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .video_info = { .codec = ESP_PEER_VIDEO_CODEC_H264 },
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE, // v1 は映像のみ (docs/whip-convention.md)
        .role = ESP_PEER_ROLE_CONTROLLING,    // WHIP publish は常に offerer 側
        .on_state = on_peer_state,
        .on_msg = on_peer_msg,
        .ctx = s,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg),
    };
    int err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s->peer);
    if (err != 0) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", err);
        return ESP_FAIL;
    }
    err = esp_peer_new_connection(s->peer);
    if (err != 0) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void teardown_session(relay_session_t *s) {
    whip_client_close(&s->whip, CONFIG_RELAY_WHIP_TOKEN);
    if (s->peer != NULL) {
        esp_peer_close(s->peer);
        s->peer = NULL;
    }
    if (s->rtsp != NULL) {
        esp_rtsp_client_stop(s->rtsp);
        s->rtsp = NULL;
    }
    if (s->events != NULL) {
        vEventGroupDelete(s->events);
        s->events = NULL;
    }
}

// 1 回の接続試行: RTSP describe (コーデック確認) → WHIP publish → 接続維持
// (esp_peer_main_loop を pump し続ける) → 切断/エラーで戻る。
// *out_connected_once は接続成功(CONNECTED状態)を一度でも観測したら true
// にする — 呼び出し側のバックオフリセット判定に使う。
static esp_err_t run_once(bool *out_connected_once) {
    relay_session_t session = {0};
    session.events = xEventGroupCreate();
    if (session.events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = start_rtsp(&session);
    if (err != ESP_OK) {
        teardown_session(&session);
        return err;
    }

    err = start_peer(&session);
    if (err != ESP_OK) {
        teardown_session(&session);
        return err;
    }

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            session.events, EVT_PEER_FAILED | EVT_RTSP_ERROR | EVT_PEER_CONNECTED,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(PEER_MAIN_LOOP_INTERVAL_MS));

        if (bits & EVT_PEER_CONNECTED) {
            if (out_connected_once != NULL) {
                *out_connected_once = true;
            }
            xEventGroupClearBits(session.events, EVT_PEER_CONNECTED);
        }
        if (bits & (EVT_PEER_FAILED | EVT_RTSP_ERROR)) {
            ESP_LOGW(TAG, "session ending (peer_failed=%d rtsp_error=%d)",
                     (bits & EVT_PEER_FAILED) != 0, (bits & EVT_RTSP_ERROR) != 0);
            teardown_session(&session);
            return ESP_FAIL;
        }
        esp_peer_main_loop(session.peer);
    }
}

static void relay_task(void *arg) {
    uint32_t backoff_ms = RECONNECT_MIN_BACKOFF_MS;
    while (1) {
        bool connected_once = false;
        esp_err_t err = run_once(&connected_once);
        if (connected_once) {
            // 一度でも繋がっていたなら次回はすぐ再試行 (alc-gw の
            // internal/whip/session.go と同じバックオフリセット方針)
            backoff_ms = RECONNECT_MIN_BACKOFF_MS;
        }
        ESP_LOGW(TAG, "relay session ended (%s), retry in %lums",
                 esp_err_to_name(err), (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > RECONNECT_MAX_BACKOFF_MS) {
            backoff_ms = RECONNECT_MAX_BACKOFF_MS;
        }
    }
}

void relay_start(void) {
    xTaskCreate(relay_task, "relay", 8192, NULL, 5, NULL);
}
