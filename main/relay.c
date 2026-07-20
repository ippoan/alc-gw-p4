// C212 の RTSP stream2 (H.264, 無トランスコード) を pull し、拠点カメラ用
// シグナリング Durable Object (ippoan/alc-app cf-alc-signaling の
// CameraSignalingRoom、ippoan/alc-app#129) 経由で admin (管理者ブラウザ)
// へ P2P WebRTC (STUN のみ) 中継する中継本体 (alc-gw-p4#1)。
//
// 当初は WHIP (RFC 9725) + クラウド SFU 方式だったが、同時複数視聴が
// 不要なこと・DO ベースのシグナリング資産が既にあることから、「device
// (本ファーム) が signaling room へ常時接続し、admin が現れた時だけ
// SDP を交換して P2P を開通させる」方式に転換した (docs/whip-convention.md
// は廃止予定)。ippoan/alc-gw の internal/whip/signaling.go が一次
// リファレンス、本ファームはその ESP-IDF 版。
//
// esp_peer は素の API を直接叩く (esp_webrtc の高レベルラッパーは
// esp_capture 経由のカメラキャプチャ専用で、外部ソースの符号化済み
// フレームを注入する経路が無いことをソース調査で確認済み)。
//
// RTSP pull は esp_media_protocols (Digest認証未対応で実機のTapo C212に
// 対して機能しなかった) から自前実装 (components/rtsp_client) に置き換え
// 済み (alc-gw-p4#1)。admin が接続していない間は RTSP も PeerConnection も
// 畳んでおき (viewer_t)、signaling 接続 (常時) だけを維持する。admin が
// 現れたら viewer を起こし、rtsp_client がRTSP制御プレーン (OPTIONS〜PLAY)
// を同期的に処理した後、専用タスク (rtsp_rx_task) がソケットを読み続けて
// rtsp_demux (interleaved復号) → h264_depacket (Annex-B再構成) →
// esp_peer_send_video の順に流す。
//
// RTSP keepalive: SETUP応答の Session timeout の半分の間隔で OPTIONS を
// 送り、カメラ側のセッションタイムアウトを更新する (rtsp_rx_task 内、
// alc-gw-p4#1 残課題)。
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "rtsp_client.h"
#include "rtsp_demux.h"
#include "h264_depacket.h"

#include "credential.h"
#include "auth_http.h"
#include "signaling_client.h"
#include "relay.h"

static const char *TAG = "relay";

// OTA (alc-gw-p4#15 フェーズ3) 中の一時停止フラグ。vTaskSuspend ではなく
// 協調的なフラグ方式にする理由は relay.h のコメント参照。
static volatile bool s_relay_paused;

#define EVT_PEER_FAILED     (1 << 0)
#define EVT_RTSP_ERROR      (1 << 1)
#define EVT_RTSP_RX_STOPPED (1 << 2)

// docs/whip-convention.md (廃止予定) から引き継ぎ: signaling 接続が切れた
// 場合の再接続は 1s → 60s 上限の指数バックオフ
#define RECONNECT_MIN_BACKOFF_MS 1000
#define RECONNECT_MAX_BACKOFF_MS 60000
#define RTSP_RESPONSE_TIMEOUT_MS 10000
// esp_peer_main_loop / signaling キューを pump する間隔。公式サンプル
// peer_demo.c と同じ駆動方法
#define PEER_MAIN_LOOP_INTERVAL_MS 10
// Session timeout ぎりぎりに送るとネットワーク遅延で間に合わないおそれが
// あるため、半分の間隔でOPTIONSキープアライブを送る
#define RTSP_KEEPALIVE_SAFETY_DIVISOR 2
// signaling WebSocket の keepalive 間隔 (経路上の中間ノードでの idle
// timeout 対策、best-effort)
#define SIGNAL_PING_INTERVAL_MS 30000
// mint した cam-relay-token の期限切れ前に自発的に繋ぎ直すための安全マージン
// (期限ぎりぎりで再mintを試みてネットワーク遅延で間に合わない事故を避ける、
// RTSP keepalive の RTSP_KEEPALIVE_SAFETY_DIVISOR と同じ考え方)
#define TOKEN_REFRESH_MARGIN_S 60
// 接続開始から SIGNALING_MSG_CONNECTED が届くまでの上限。これを超えたら
// 失敗として扱い、外側のバックオフ再接続に委ねる
#define SIGNAL_CONNECT_TIMEOUT_MS 15000

// interleavedフレーム1つ分 (RTPは通常MTU程度に収まる)。demuxがこれを超える
// フレームは安全にスキップする (rtsp_demuxのオーバーサイズ処理)
#define DEMUX_FRAME_BUF_SIZE 1500
// 1アクセスユニット (IDRフレーム1枚) 分。360p H264ならこれで十分な想定だが、
// 実機のビットレート次第で不足する場合は増やす (PSRAM未使用のため内蔵SRAM)
#define H264_AU_BUF_SIZE (64 * 1024)

// admin 1名ぶんの RTSP pull + PeerConnection のライフサイクル。admin が
// いない間は存在しない (NULL) — signaling 接続だけが常時保たれる。
typedef struct {
    EventGroupHandle_t events; // EVT_PEER_FAILED / EVT_RTSP_ERROR / EVT_RTSP_RX_STOPPED
    esp_peer_handle_t peer;
    signaling_client_t *signal; // offerの送信に使う。所有はしない (close しない)

    rtsp_client_t *rtsp;
    rtsp_demux_t demux;
    h264_depacket_t depacket;
    uint8_t *demux_buf; // heap確保 (DEMUX_FRAME_BUF_SIZE)
    uint8_t *au_buf;    // heap確保 (H264_AU_BUF_SIZE)

    TaskHandle_t rtsp_rx_task;
    volatile bool rtsp_rx_should_stop;
} viewer_t;

// ---------------------------------------------------------------------
// esp_peer コールバック
// ---------------------------------------------------------------------

static int on_peer_state(esp_peer_state_t state, void *ctx) {
    viewer_t *v = (viewer_t *)ctx;
    ESP_LOGI(TAG, "peer state = %d", (int)state);
    switch (state) {
    case ESP_PEER_STATE_CONNECT_FAILED:
    case ESP_PEER_STATE_DISCONNECTED:
    case ESP_PEER_STATE_CLOSED:
        xEventGroupSetBits(v->events, EVT_PEER_FAILED);
        break;
    default:
        break;
    }
    return 0;
}

// esp_peer が local SDP (offer) を生成すると呼ばれる。non-trickle 前提
// (ippoan/alc-app#129 の設計判断) — offer 生成時に一度だけ signaling へ
// sdp_offer を送り、answer は後続の SIGNALING_MSG_SDP_ANSWER イベント
// (run_signaling_session の主ループ) で非同期に受け取って
// esp_peer_send_msg に渡す。trickle candidate 等の他メッセージ種別は
// この v1 スコープでは無視する。
static int on_peer_msg(esp_peer_msg_t *msg, void *ctx) {
    viewer_t *v = (viewer_t *)ctx;
    if (msg == NULL || msg->type != ESP_PEER_MSG_TYPE_SDP) {
        return 0;
    }

    esp_err_t err = signaling_client_send_offer(v->signal, (const char *)msg->data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "signaling_client_send_offer failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(v->events, EVT_PEER_FAILED);
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
    viewer_t *v = (viewer_t *)ctx;
    (void)keyframe;
    if (v->peer == NULL || len == 0) {
        return;
    }
    esp_peer_video_frame_t frame = {
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
        .data = (uint8_t *)annexb,
        .size = len,
    };
    int err = esp_peer_send_video(v->peer, &frame);
    if (err != 0) {
        ESP_LOGW(TAG, "esp_peer_send_video failed: %d", err);
    }
}

// SETUPで "interleaved=0-1" を要求している (channel0=RTP video, channel1=RTCP)。
// RTCPはv1では読み捨てる。
static void on_demux_frame(uint8_t channel, const uint8_t *payload, size_t len, void *ctx) {
    viewer_t *v = (viewer_t *)ctx;
    if (channel == 0) {
        h264_depacket_feed_rtp(&v->depacket, payload, len);
    }
}

static void rtsp_rx_task(void *arg) {
    viewer_t *v = (viewer_t *)arg;
    const rtsp_io_ops_t *io = NULL;
    void *io_ctx = NULL;
    rtsp_client_get_io(v->rtsp, &io, &io_ctx);

    uint8_t buf[DEMUX_FRAME_BUF_SIZE];

    // PLAY応答読み取り時にソケットへ先読みされてしまった分 (RTPの先頭を
    // 含むかもしれない) をまず処理する
    size_t buffered = rtsp_client_take_buffered(v->rtsp, buf, sizeof(buf));
    if (buffered > 0) {
        rtsp_demux_feed(&v->demux, buf, buffered);
    }

    uint32_t timeout_sec = rtsp_client_get_session_timeout_sec(v->rtsp);
    int64_t keepalive_interval_us =
        (int64_t)(timeout_sec > 0 ? timeout_sec : 60) * 1000000LL / RTSP_KEEPALIVE_SAFETY_DIVISOR;
    int64_t last_keepalive_us = esp_timer_get_time();

    while (!v->rtsp_rx_should_stop) {
        int n = io->recv(io_ctx, buf, sizeof(buf), 1000);
        if (n < 0) {
            ESP_LOGW(TAG, "rtsp: connection lost while streaming");
            xEventGroupSetBits(v->events, EVT_RTSP_ERROR);
            break;
        }
        if (n > 0) {
            rtsp_demux_feed(&v->demux, buf, (size_t)n);
        }

        // io->recv のタイムアウト(1000ms)が下限の分解能になるため、データが
        // 流れていない間もこのループは定期的に回ってくる
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_keepalive_us >= keepalive_interval_us) {
            rtsp_client_send_keepalive(v->rtsp);
            last_keepalive_us = now_us;
        }
    }
    xEventGroupSetBits(v->events, EVT_RTSP_RX_STOPPED);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------
// viewer ライフサイクル (adminの出入りに応じて開閉)
// ---------------------------------------------------------------------

static esp_err_t start_rtsp(viewer_t *v) {
    rtsp_client_config_t cfg = {
        .url = CONFIG_RELAY_RTSP_URL,
        .username = CONFIG_RELAY_RTSP_USERNAME[0] != '\0' ? CONFIG_RELAY_RTSP_USERNAME : NULL,
        .password = CONFIG_RELAY_RTSP_PASSWORD[0] != '\0' ? CONFIG_RELAY_RTSP_PASSWORD : NULL,
        .response_timeout_ms = RTSP_RESPONSE_TIMEOUT_MS,
    };
    esp_err_t err = rtsp_client_open(&cfg, &v->rtsp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtsp_client_open failed: %s", esp_err_to_name(err));
        return err;
    }

    rtsp_sdp_video_t video;
    err = rtsp_client_play(v->rtsp, &video);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtsp_client_play failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "rtsp: PLAY ok (payload_type=%d, sps_len=%d, pps_len=%d)", video.payload_type,
             (int)video.sps_len, (int)video.pps_len);

    v->demux_buf = malloc(DEMUX_FRAME_BUF_SIZE);
    v->au_buf = malloc(H264_AU_BUF_SIZE);
    if (v->demux_buf == NULL || v->au_buf == NULL) {
        ESP_LOGE(TAG, "malloc failed for demux/au buffers");
        return ESP_ERR_NO_MEM;
    }
    h264_depacket_init(&v->depacket, v->au_buf, H264_AU_BUF_SIZE, on_h264_au, v);
    rtsp_demux_init(&v->demux, v->demux_buf, DEMUX_FRAME_BUF_SIZE, on_demux_frame, v);

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
            on_h264_au(sps_pps, off, true, v);
        }
    }

    return ESP_OK;
}

static esp_err_t start_peer(viewer_t *v) {
    // 拠点外の admin ブラウザとの直接 P2P 前提なので ice_use_lite_mode は
    // 使わない (docs/whip-convention.md (廃止予定) の ICE 節と同じ判断)。
    static esp_peer_default_cfg_t default_cfg;
    memset(&default_cfg, 0, sizeof(default_cfg));

    esp_peer_cfg_t cfg = {
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .video_info = {.codec = ESP_PEER_VIDEO_CODEC_H264},
        .audio_dir = ESP_PEER_MEDIA_DIR_NONE, // v1 は映像のみ
        .role = ESP_PEER_ROLE_CONTROLLING,    // publish は常に offerer 側
        .on_state = on_peer_state,
        .on_msg = on_peer_msg,
        .ctx = v,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg),
    };
    int err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &v->peer);
    if (err != 0) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", err);
        return ESP_FAIL;
    }
    err = esp_peer_new_connection(v->peer);
    if (err != 0) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void viewer_close(viewer_t *v) {
    if (v == NULL) {
        return;
    }
    if (v->rtsp_rx_task != NULL) {
        v->rtsp_rx_should_stop = true;
        // rtsp_rx_task が自分でソケットの読み取りをやめるのを待ってから
        // 閉じる (io->close と recv の競合を避ける)。io->recv のタイムアウト
        // (1000ms) 分は待つ必要があるため、余裕を見て2000ms
        xEventGroupWaitBits(v->events, EVT_RTSP_RX_STOPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        v->rtsp_rx_task = NULL;
    }

    if (v->peer != NULL) {
        esp_peer_close(v->peer);
        v->peer = NULL;
    }
    if (v->rtsp != NULL) {
        rtsp_client_teardown(v->rtsp);
        rtsp_client_close(v->rtsp);
        v->rtsp = NULL;
    }
    free(v->demux_buf);
    free(v->au_buf);
    if (v->events != NULL) {
        vEventGroupDelete(v->events);
    }
    free(v);
}

// admin が接続してきたら呼ぶ: RTSP PLAY → PeerConnection 起動 → offer 送信
// (answer は非同期)。失敗したら NULL を返し、呼び出し側は viewer 無しの
// まま次の admin 接続を待てばよい (viewer 単体の失敗では signaling 接続
// 自体は切らない)。
static viewer_t *viewer_start(signaling_client_t *sig) {
    viewer_t *v = calloc(1, sizeof(viewer_t));
    if (v == NULL) {
        return NULL;
    }
    v->signal = sig;
    v->events = xEventGroupCreate();
    if (v->events == NULL) {
        free(v);
        return NULL;
    }

    esp_err_t err = start_rtsp(v);
    if (err != ESP_OK) {
        viewer_close(v);
        return NULL;
    }

    err = start_peer(v);
    if (err != ESP_OK) {
        viewer_close(v);
        return NULL;
    }

    BaseType_t rx_ok = xTaskCreate(rtsp_rx_task, "rtsp_rx", 8192, v, 5, &v->rtsp_rx_task);
    if (rx_ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(rtsp_rx) failed");
        v->rtsp_rx_task = NULL;
        viewer_close(v);
        return NULL;
    }

    return v;
}

static void viewer_handle_answer(viewer_t *v, const char *answer_sdp) {
    esp_peer_msg_t answer_msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)answer_sdp,
        .size = strlen(answer_sdp),
    };
    int ret = esp_peer_send_msg(v->peer, &answer_msg);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_peer_send_msg(answer) failed: %d", ret);
        xEventGroupSetBits(v->events, EVT_PEER_FAILED);
    }
}

// ---------------------------------------------------------------------
// signaling セッションライフサイクル
// ---------------------------------------------------------------------

// device credential (role=device-gateway) があれば cam-relay-token を都度
// mint し、無い/mint失敗なら静的トークン (CONFIG_RELAY_SIGNALING_TOKEN) に
// フォールバックする (移行期共存、alc-gw-p4#2 テスト計画)。mint できたら
// *out_minted_token / *out_site_id に所有権を渡す (呼び出し側が free する。
// static token にフォールバックした場合は両方 NULL のまま — site_id は
// RELAY_SIGNALING_ROOM_OVERRIDE で別途補う、relay.c 呼び出し側参照)。
// *out_expiry_us は再mintすべき時刻 (static token の場合は運用側が失効
// させる前提で INT64_MAX)。
static bool mint_or_fallback_token(char **out_minted_token, char **out_site_id, int64_t *out_expiry_us) {
    *out_minted_token = NULL;
    *out_site_id = NULL;
    *out_expiry_us = INT64_MAX;

    char device_id[CREDENTIAL_ID_MAX_LEN];
    char device_secret[CREDENTIAL_SECRET_MAX_LEN];
    if (!credential_load(device_id, sizeof(device_id), device_secret, sizeof(device_secret))) {
        return false;
    }

    auth_http_cam_relay_token_t minted;
    esp_err_t err = auth_http_cam_relay_token(CONFIG_RELAY_AUTH_WORKER_URL, device_id, device_secret, &minted);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cam-relay-token mint失敗、静的トークンにフォールバック");
        return false;
    }

    *out_minted_token = minted.access_token; // 所有権を呼び出し側へ移す
    *out_site_id = minted.site_id;           // 同上
    *out_expiry_us =
        esp_timer_get_time() + ((int64_t)minted.expires_in_s - TOKEN_REFRESH_MARGIN_S) * 1000000;
    return true;
}

// 1 回の signaling WebSocket 接続のライフサイクル: 接続 → admin の出入りに
// 応じて viewer を開閉するループ → 接続が切れる/タイムアウトしたら戻る。
// 戻り値: signaling に一度でも接続できていれば true (呼び出し側の
// バックオフリセット判定に使う)。
static bool run_signaling_session(void) {
    QueueHandle_t q = xQueueCreate(16, sizeof(signaling_msg_t));
    if (q == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return false;
    }

    char *minted_token = NULL;
    char *minted_site_id = NULL;
    int64_t token_expiry_us = INT64_MAX;
    mint_or_fallback_token(&minted_token, &minted_site_id, &token_expiry_us);
    const char *token = minted_token != NULL ? minted_token : CONFIG_RELAY_SIGNALING_TOKEN;
    // site_id は cam-relay-token mint 応答を常に優先する (拠点ごとに違う値を
    // 全デバイス共通の OTA バイナリに焼けないため、実行時に決まる)。mint が
    // 使えない場合のみ開発用の静的値にフォールバックする。
    const char *site_id = minted_site_id != NULL ? minted_site_id : CONFIG_RELAY_SIGNALING_ROOM_OVERRIDE;

    if (site_id[0] == '\0') {
        ESP_LOGW(TAG, "site_id 不明 (device credential 未設定 or mint失敗、"
                       "RELAY_SIGNALING_ROOM_OVERRIDE も未設定) — 今回の接続試行をスキップ");
        free(minted_token);
        free(minted_site_id);
        vQueueDelete(q);
        return false;
    }

    char endpoint[192];
    int n = snprintf(endpoint, sizeof(endpoint), "wss://%s/cam-room/%s", CONFIG_RELAY_SIGNALING_HOST, site_id);
    if (n < 0 || (size_t)n >= sizeof(endpoint)) {
        ESP_LOGE(TAG, "signaling endpoint がバッファに収まらない (host/site_id が長すぎる)");
        free(minted_token);
        free(minted_site_id);
        vQueueDelete(q);
        return false;
    }

    signaling_client_t *sig = NULL;
    esp_err_t err = signaling_client_connect(endpoint, token, q, &sig);
    free(minted_token);
    free(minted_site_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "signaling_client_connect failed: %s", esp_err_to_name(err));
        vQueueDelete(q);
        return false;
    }

    bool connected_once = false;
    bool signaling_up = false;
    int64_t connect_deadline_us = esp_timer_get_time() + (int64_t)SIGNAL_CONNECT_TIMEOUT_MS * 1000;
    int64_t last_ping_us = esp_timer_get_time();
    viewer_t *v = NULL;

    while (1) {
        if (s_relay_paused) {
            ESP_LOGI(TAG, "relay: pause要求により signaling セッションを終了します");
            viewer_close(v);
            signaling_client_close(sig);
            vQueueDelete(q);
            return true;
        }

        signaling_msg_t msg;
        if (xQueueReceive(q, &msg, pdMS_TO_TICKS(PEER_MAIN_LOOP_INTERVAL_MS)) == pdTRUE) {
            switch (msg.type) {
            case SIGNALING_MSG_CONNECTED:
                ESP_LOGI(TAG, "signaling: connected, waiting for admin");
                signaling_up = true;
                connected_once = true;
                break;

            case SIGNALING_MSG_DISCONNECTED:
                ESP_LOGW(TAG, "signaling: disconnected");
                viewer_close(v);
                signaling_client_close(sig);
                vQueueDelete(q);
                return connected_once;

            case SIGNALING_MSG_PEER_JOINED_ADMIN:
                if (v == NULL) {
                    ESP_LOGI(TAG, "admin joined, starting viewer");
                    v = viewer_start(sig);
                    if (v == NULL) {
                        ESP_LOGW(TAG, "viewer_start failed, waiting for next admin");
                    }
                }
                break;

            case SIGNALING_MSG_PEER_LEFT_ADMIN:
                if (v != NULL) {
                    ESP_LOGI(TAG, "admin left, closing viewer");
                    viewer_close(v);
                    v = NULL;
                }
                break;

            case SIGNALING_MSG_SDP_ANSWER:
                if (v != NULL) {
                    viewer_handle_answer(v, msg.sdp);
                }
                free(msg.sdp);
                break;

            case SIGNALING_MSG_SERVER_ERROR:
                ESP_LOGW(TAG, "signaling: server reported an error");
                break;
            }
        }

        if (!signaling_up && esp_timer_get_time() > connect_deadline_us) {
            ESP_LOGW(TAG, "signaling: connect timed out");
            viewer_close(v);
            signaling_client_close(sig);
            vQueueDelete(q);
            return connected_once;
        }

        if (v != NULL) {
            esp_peer_main_loop(v->peer);
            EventBits_t bits = xEventGroupGetBits(v->events);
            if (bits & (EVT_PEER_FAILED | EVT_RTSP_ERROR)) {
                ESP_LOGW(TAG, "viewer session ending (peer_failed=%d rtsp_error=%d)",
                         (bits & EVT_PEER_FAILED) != 0, (bits & EVT_RTSP_ERROR) != 0);
                viewer_close(v);
                v = NULL;
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (signaling_up && now_us - last_ping_us >= (int64_t)SIGNAL_PING_INTERVAL_MS * 1000) {
            signaling_client_ping(sig);
            last_ping_us = now_us;
        }

        // mint した token の期限が近づいたら、admin 非接続中 (viewer が無い)
        // タイミングを見計らって自発的に繋ぎ直し、新しい token で再mintする。
        // viewer 接続中は打ち切らず、次に空いたタイミングまで持ち越す
        // (無停止でpublish継続、alc-gw-p4#2 テスト計画)。
        if (signaling_up && v == NULL && now_us >= token_expiry_us) {
            ESP_LOGI(TAG, "cam-relay-token 期限間近のため再接続します");
            signaling_client_close(sig);
            vQueueDelete(q);
            return true;
        }
    }
}

static void relay_task(void *arg) {
    (void)arg;
    uint32_t backoff_ms = RECONNECT_MIN_BACKOFF_MS;
    while (1) {
        if (s_relay_paused) {
            // pause中は signaling セッションを開こうとせず idle で待つ
            // (OTAダウンロード中、relay_pause()/relay_resume() 参照)。
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        bool connected_once = run_signaling_session();
        if (connected_once) {
            // 一度でも signaling に繋がっていたなら次回はすぐ再試行
            // (ippoan/alc-gw の internal/whip/session.go と同じバックオフ
            // リセット方針)
            backoff_ms = RECONNECT_MIN_BACKOFF_MS;
        }
        ESP_LOGW(TAG, "signaling session ended, retry in %lums", (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > RECONNECT_MAX_BACKOFF_MS) {
            backoff_ms = RECONNECT_MAX_BACKOFF_MS;
        }
    }
}

void relay_start(void) {
    // signaling host 未設定 (既定は空文字、RTSP_USERNAME/PASSWORD と同じ規約)
    // なら relay_task 自体を起動しない。以前はダミーの example.workers.dev
    // を既定にしていたため、未設定のまま起動すると DNS 解決失敗の再接続
    // ループが priority 5 で回り続け、priority 2 の console (esp_console)
    // タスクが実質的に応答不能になる事故があった。
    if (CONFIG_RELAY_SIGNALING_HOST[0] == '\0') {
        ESP_LOGI(TAG, "RELAY_SIGNALING_HOST 未設定のため camera relay を起動しません");
        return;
    }
    xTaskCreate(relay_task, "relay", 8192, NULL, 5, NULL);
}

void relay_pause(void) {
    s_relay_paused = true;
}

void relay_resume(void) {
    s_relay_paused = false;
}
