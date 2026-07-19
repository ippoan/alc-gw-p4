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
// RTSP pull は esp_media_protocols (Digest認証未対応で実機のTapo C212に
// 対して機能しなかった) から自前実装 (components/rtsp_client) に置き換え
// 済み (alc-gw-p4#1)。rtsp_client がRTSP制御プレーン (OPTIONS〜PLAY) を
// 同期的に処理した後、専用タスク (rtsp_rx_task) がソケットを読み続けて
// rtsp_demux (interleaved復号) → h264_depacket (Annex-B再構成) →
// esp_peer_send_video の順に流す。
//
// 【既知の制約、v1スコープ】RTSP keepalive (Session timeoutに対する
// 定期OPTIONS送信) は未実装。映像が流れ続けている限りタイムアウトしない
// カメラが多いため実害は小さい想定だが、長時間運用で問題が出れば追加する。
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

#include "rtsp_client.h"
#include "rtsp_demux.h"
#include "h264_depacket.h"

#include "whip_client.h"
#include "relay.h"

static const char *TAG = "relay";

#define EVT_PEER_CONNECTED (1 << 2)
#define EVT_PEER_FAILED    (1 << 3)
#define EVT_RTSP_ERROR     (1 << 4)
#define EVT_RTSP_RX_STOPPED (1 << 5)

// docs/whip-convention.md: 再接続は 1s → 2s → … → 60s 上限
#define RECONNECT_MIN_BACKOFF_MS 1000
#define RECONNECT_MAX_BACKOFF_MS 60000
#define RTSP_RESPONSE_TIMEOUT_MS 10000
// esp_peer_main_loop を pump する間隔。公式サンプル peer_demo.c と同じ駆動方法
#define PEER_MAIN_LOOP_INTERVAL_MS 10

// interleavedフレーム1つ分 (RTPは通常MTU程度に収まる)。demuxがこれを超える
// フレームは安全にスキップする (rtsp_demuxのオーバーサイズ処理)
#define DEMUX_FRAME_BUF_SIZE 1500
// 1アクセスユニット (IDRフレーム1枚) 分。360p H264ならこれで十分な想定だが、
// 実機のビットレート次第で不足する場合は増やす (PSRAM未使用のため内蔵SRAM)
#define H264_AU_BUF_SIZE (64 * 1024)

typedef struct {
    EventGroupHandle_t events;
    esp_peer_handle_t peer;
    whip_client_t whip;

    rtsp_client_t *rtsp;
    rtsp_demux_t demux;
    h264_depacket_t depacket;
    uint8_t *demux_buf; // heap確保 (DEMUX_FRAME_BUF_SIZE)
    uint8_t *au_buf;    // heap確保 (H264_AU_BUF_SIZE)

    TaskHandle_t rtsp_rx_task;
    volatile bool rtsp_rx_should_stop;
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
// RTSP受信 (interleaved復号 → H264 depacketization → esp_peer)
// ---------------------------------------------------------------------

// H.264アクセスユニット (Annex-B) を無トランスコードのまま esp_peer_send_video
// へ転送する。RTPタイムスタンプはPTPドリフトの補正が別途要るため使わず、
// esp_timerから自前の単調増加ptsを合成する (esp_peer公式サンプルの
// "av_stream pts out of sync, use system pts instead" と同じ手法)。
static void on_h264_au(const uint8_t *annexb, size_t len, bool keyframe, void *ctx) {
    relay_session_t *s = (relay_session_t *)ctx;
    (void)keyframe;
    if (s->peer == NULL || len == 0) {
        return;
    }
    esp_peer_video_frame_t frame = {
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
        .data = (uint8_t *)annexb,
        .size = len,
    };
    int err = esp_peer_send_video(s->peer, &frame);
    if (err != 0) {
        ESP_LOGW(TAG, "esp_peer_send_video failed: %d", err);
    }
}

// SETUPで "interleaved=0-1" を要求している (channel0=RTP video, channel1=RTCP)。
// RTCPはv1では読み捨てる。
static void on_demux_frame(uint8_t channel, const uint8_t *payload, size_t len, void *ctx) {
    relay_session_t *s = (relay_session_t *)ctx;
    if (channel == 0) {
        h264_depacket_feed_rtp(&s->depacket, payload, len);
    }
}

static void rtsp_rx_task(void *arg) {
    relay_session_t *s = (relay_session_t *)arg;
    const rtsp_io_ops_t *io = NULL;
    void *io_ctx = NULL;
    rtsp_client_get_io(s->rtsp, &io, &io_ctx);

    uint8_t buf[DEMUX_FRAME_BUF_SIZE];

    // PLAY応答読み取り時にソケットへ先読みされてしまった分 (RTPの先頭を
    // 含むかもしれない) をまず処理する
    size_t buffered = rtsp_client_take_buffered(s->rtsp, buf, sizeof(buf));
    if (buffered > 0) {
        rtsp_demux_feed(&s->demux, buf, buffered);
    }

    while (!s->rtsp_rx_should_stop) {
        int n = io->recv(io_ctx, buf, sizeof(buf), 1000);
        if (n < 0) {
            ESP_LOGW(TAG, "rtsp: connection lost while streaming");
            xEventGroupSetBits(s->events, EVT_RTSP_ERROR);
            break;
        }
        if (n > 0) {
            rtsp_demux_feed(&s->demux, buf, (size_t)n);
        }
    }
    xEventGroupSetBits(s->events, EVT_RTSP_RX_STOPPED);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// セッションライフサイクル
// ---------------------------------------------------------------------

static esp_err_t start_rtsp(relay_session_t *s) {
    rtsp_client_config_t cfg = {
        .url = CONFIG_RELAY_RTSP_URL,
        .username = CONFIG_RELAY_RTSP_USERNAME[0] != '\0' ? CONFIG_RELAY_RTSP_USERNAME : NULL,
        .password = CONFIG_RELAY_RTSP_PASSWORD[0] != '\0' ? CONFIG_RELAY_RTSP_PASSWORD : NULL,
        .response_timeout_ms = RTSP_RESPONSE_TIMEOUT_MS,
    };
    esp_err_t err = rtsp_client_open(&cfg, &s->rtsp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtsp_client_open failed: %s", esp_err_to_name(err));
        return err;
    }

    rtsp_sdp_video_t video;
    err = rtsp_client_play(s->rtsp, &video);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtsp_client_play failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "rtsp: PLAY ok (payload_type=%d, sps_len=%d, pps_len=%d)", video.payload_type,
             (int)video.sps_len, (int)video.pps_len);

    s->demux_buf = malloc(DEMUX_FRAME_BUF_SIZE);
    s->au_buf = malloc(H264_AU_BUF_SIZE);
    if (s->demux_buf == NULL || s->au_buf == NULL) {
        ESP_LOGE(TAG, "malloc failed for demux/au buffers");
        return ESP_ERR_NO_MEM;
    }
    h264_depacket_init(&s->depacket, s->au_buf, H264_AU_BUF_SIZE, on_h264_au, s);
    rtsp_demux_init(&s->demux, s->demux_buf, DEMUX_FRAME_BUF_SIZE, on_demux_frame, s);

    if (video.has_sprop && (video.sps_len > 0 || video.pps_len > 0)) {
        // SDPのsprop-parameter-setsを最初のフレームとして先出しする。
        // カメラによってはIDR毎にSTAP-Aで再送してくるが、esp_peer側は
        // SPS/PPSの重複を許容する想定で素通しにする (実装計画PR4のリスク節)
        static const uint8_t start_code[4] = {0, 0, 0, 1};
        uint8_t sps_pps[256];
        size_t off = 0;
        if (video.sps_len > 0 && off + 4 + video.sps_len <= sizeof(sps_pps)) {
            memcpy(sps_pps + off, start_code, 4);
            off += 4;
            memcpy(sps_pps + off, video.sps, video.sps_len);
            off += video.sps_len;
        }
        if (video.pps_len > 0 && off + 4 + video.pps_len <= sizeof(sps_pps)) {
            memcpy(sps_pps + off, start_code, 4);
            off += 4;
            memcpy(sps_pps + off, video.pps, video.pps_len);
            off += video.pps_len;
        }
        if (off > 0) {
            on_h264_au(sps_pps, off, true, s);
        }
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
    if (s->rtsp_rx_task != NULL) {
        s->rtsp_rx_should_stop = true;
        // rtsp_rx_task が自分でソケットの読み取りをやめるのを待ってから
        // 閉じる (io->close と recv の競合を避ける)。io->recv のタイムアウト
        // (1000ms) 分は待つ必要があるため、余裕を見て2000ms
        xEventGroupWaitBits(s->events, EVT_RTSP_RX_STOPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        s->rtsp_rx_task = NULL;
    }

    whip_client_close(&s->whip, CONFIG_RELAY_WHIP_TOKEN);
    if (s->peer != NULL) {
        esp_peer_close(s->peer);
        s->peer = NULL;
    }
    if (s->rtsp != NULL) {
        rtsp_client_teardown(s->rtsp);
        rtsp_client_close(s->rtsp);
        s->rtsp = NULL;
    }
    free(s->demux_buf);
    free(s->au_buf);
    s->demux_buf = NULL;
    s->au_buf = NULL;
    if (s->events != NULL) {
        vEventGroupDelete(s->events);
        s->events = NULL;
    }
}

// 1 回の接続試行: RTSP PLAY → WHIP publish → 接続維持
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

    BaseType_t rx_ok = xTaskCreate(rtsp_rx_task, "rtsp_rx", 8192, &session, 5, &session.rtsp_rx_task);
    if (rx_ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(rtsp_rx) failed");
        session.rtsp_rx_task = NULL;
        teardown_session(&session);
        return ESP_ERR_NO_MEM;
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
