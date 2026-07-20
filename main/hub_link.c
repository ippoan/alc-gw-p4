#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "cJSON.h"

#include "credential.h"
#include "auth_http.h"
#include "hub_link.h"

static const char *TAG = "hub_link";

// hub_link_stop()/再開のため、server ハンドルと beacon タスクの状態を保持する
// (元々はどちらも hub_link_start() のローカル変数だった)。
static httpd_handle_t s_server;
static TaskHandle_t s_beacon_task;
static volatile bool s_beacon_stop_requested;

#define HUB_WS_PORT 9000
#define BEACON_PORT 9001
#define BEACON_INTERVAL_MS 5000
// hello/auth/measurement/ble_status を想定した上限 (signaling_client.c の
// SIGNALING_MAX_FRAME と同じ考え方)。measurement payload を将来 Android へ
// 中継する際に増やす可能性はあるが、現状はログ出力のみなので抑えめにする
#define MAX_FRAME_LEN (8 * 1024)
// 16 バイトの乱数を hex 文字列化 (32 文字 + NUL)
#define NONCE_HEX_LEN 32

typedef enum {
    SESSION_AWAIT_HELLO,
    SESSION_AWAIT_AUTH,
    SESSION_VERIFIED,
} session_state_t;

typedef struct {
    session_state_t state;
    char nonce[NONCE_HEX_LEN + 1];
} session_ctx_t;

static void session_ctx_free(void *ctx) {
    free(ctx);
}

static void generate_nonce_hex(char *out, size_t out_cap) {
    uint8_t raw[NONCE_HEX_LEN / 2];
    for (size_t i = 0; i < sizeof(raw); i += 4) {
        uint32_t r = esp_random();
        size_t n = sizeof(r) <= sizeof(raw) - i ? sizeof(r) : sizeof(raw) - i;
        memcpy(raw + i, &r, n);
    }
    static const char hex[] = "0123456789abcdef";
    size_t written = 0;
    for (size_t i = 0; i < sizeof(raw) && written + 2 < out_cap; i++) {
        out[written++] = hex[raw[i] >> 4];
        out[written++] = hex[raw[i] & 0xF];
    }
    out[written] = '\0';
}

static esp_err_t send_text(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    return httpd_ws_send_frame(req, &pkt);
}

// auth_fail を送って切断する (ESP_FAIL を返すと httpd がコネクションを閉じる)。
static esp_err_t send_auth_fail(httpd_req_t *req, const char *reason) {
    char frame[128];
    snprintf(frame, sizeof(frame), "{\"type\":\"auth_fail\",\"reason\":\"%s\"}", reason);
    // 送信失敗しても切断はする (ESP_FAIL) — reason を伝えられないだけ
    send_text(req, frame);
    return ESP_FAIL;
}

static esp_err_t handle_hello(httpd_req_t *req, session_ctx_t *ctx, const cJSON *root) {
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    ESP_LOGI(TAG, "hello from device=%s",
             (cJSON_IsString(device) && device->valuestring != NULL) ? device->valuestring : "?");

    generate_nonce_hex(ctx->nonce, sizeof(ctx->nonce));
    char frame[NONCE_HEX_LEN + 48];
    snprintf(frame, sizeof(frame), "{\"type\":\"auth_challenge\",\"nonce\":\"%s\"}", ctx->nonce);
    ctx->state = SESSION_AWAIT_AUTH;
    return send_text(req, frame);
}

// CoreS3 の auth (hub-token) を検証し、OK なら自分の hub-token を mint して
// auth_ok を返す (相互認証、ippoan/alc-app-s3#83)。
static esp_err_t handle_auth(httpd_req_t *req, session_ctx_t *ctx, const cJSON *root) {
    if (ctx->state != SESSION_AWAIT_AUTH) {
        ESP_LOGW(TAG, "auth を認証待ちでない状態で受信、無視");
        return ESP_OK;
    }
    const cJSON *token_j = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (!cJSON_IsString(token_j) || token_j->valuestring == NULL || token_j->valuestring[0] == '\0') {
        return send_auth_fail(req, "missing_token");
    }

    char device_id[CREDENTIAL_ID_MAX_LEN];
    char device_secret[CREDENTIAL_SECRET_MAX_LEN];
    if (!credential_load(device_id, sizeof(device_id), device_secret, sizeof(device_secret))) {
        ESP_LOGE(TAG, "credential 未設定 ('cred set <id> <secret>' で注入してください)");
        return send_auth_fail(req, "no_credential");
    }

    auth_http_introspect_t cores3_result;
    esp_err_t err = auth_http_introspect(CONFIG_RELAY_AUTH_WORKER_URL, device_id, device_secret,
                                          token_j->valuestring, &cores3_result);
    if (err != ESP_OK) {
        return send_auth_fail(req, "introspect_failed");
    }
    if (!cores3_result.valid || cores3_result.role == NULL || strcmp(cores3_result.role, "device-hub") != 0) {
        ESP_LOGW(TAG, "CoreS3 の token が無効 (valid=%d, role=%s)", cores3_result.valid,
                 cores3_result.role != NULL ? cores3_result.role : "(none)");
        auth_http_introspect_free(&cores3_result);
        return send_auth_fail(req, "invalid_token");
    }

    char nonce2[NONCE_HEX_LEN + 1];
    generate_nonce_hex(nonce2, sizeof(nonce2));
    auth_http_hub_token_t own;
    err = auth_http_hub_token(CONFIG_RELAY_AUTH_WORKER_URL, device_id, device_secret, nonce2, &own);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "自分の hub-token mint 失敗");
        auth_http_introspect_free(&cores3_result);
        return send_auth_fail(req, "mint_failed");
    }

    // site_id 一致で 1:1 を強制する (CoreS3 の site_id は自分の device_id が
    // 既定、GW の site_id は provisioning 時に対象 hub の device_id を明示
    // 指定して割り当て済み、ippoan/auth-worker#406)。
    if (strcmp(cores3_result.site_id, own.site_id) != 0) {
        ESP_LOGW(TAG, "site_id 不一致 (cores3=%s, self=%s)", cores3_result.site_id, own.site_id);
        auth_http_introspect_free(&cores3_result);
        auth_http_hub_token_free(&own);
        return send_auth_fail(req, "site_mismatch");
    }
    auth_http_introspect_free(&cores3_result);

    char frame[1200];
    int n = snprintf(frame, sizeof(frame), "{\"type\":\"auth_ok\",\"token\":\"%s\"}", own.access_token);
    auth_http_hub_token_free(&own);
    if (n < 0 || (size_t)n >= sizeof(frame)) {
        ESP_LOGE(TAG, "auth_ok frame too large");
        return ESP_FAIL;
    }

    ctx->state = SESSION_VERIFIED;
    esp_err_t send_err = send_text(req, frame);
    ESP_LOGI(TAG, "EVT HUB_AUTH_OK");
    return send_err;
}

static esp_err_t handle_frame(httpd_req_t *req, session_ctx_t *ctx, const char *text) {
    cJSON *root = cJSON_Parse(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "不正な JSON frame を無視");
        cJSON_Delete(root);
        return ESP_OK;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type_str = (cJSON_IsString(type) && type->valuestring != NULL) ? type->valuestring : "";

    esp_err_t result = ESP_OK;
    if (strcmp(type_str, "hello") == 0) {
        result = handle_hello(req, ctx, root);
    } else if (strcmp(type_str, "auth") == 0) {
        result = handle_auth(req, ctx, root);
    } else if (strcmp(type_str, "measurement") == 0 || strcmp(type_str, "ble_status") == 0) {
        // Android への中継は別途 (今回はログのみ、ippoan/alc-app-s3#83 のスコープ外)
        if (ctx->state == SESSION_VERIFIED) {
            ESP_LOGI(TAG, "%s (未中継、ログのみ): %s", type_str, text);
        } else {
            ESP_LOGW(TAG, "認証未完了のため %s を無視", type_str);
        }
    } else {
        ESP_LOGI(TAG, "未対応の frame type=%s", type_str);
    }
    cJSON_Delete(root);
    return result;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // WS ハンドシェイク完了。以後のフレームはこの同じ handler が
        // 再度呼ばれる (ESP-IDF esp_http_server の作法)。接続ごとの
        // 状態は sess_ctx に持たせる
        session_ctx_t *ctx = calloc(1, sizeof(session_ctx_t));
        if (ctx == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ctx->state = SESSION_AWAIT_HELLO;
        req->sess_ctx = ctx;
        req->free_ctx = session_ctx_free;
        ESP_LOGI(TAG, "new connection (WS handshake done)");
        return ESP_OK;
    }

    session_ctx_t *ctx = (session_ctx_t *)req->sess_ctx;
    if (ctx == NULL) {
        return ESP_FAIL;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws recv (length) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        return ESP_OK;
    }
    if (ws_pkt.len == 0) {
        return ESP_OK;
    }
    if (ws_pkt.len > MAX_FRAME_LEN) {
        ESP_LOGW(TAG, "frame too large (%d bytes), disconnecting", (int)ws_pkt.len);
        return ESP_FAIL;
    }

    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws recv (payload) failed: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    esp_err_t handle_ret = ESP_OK;
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        handle_ret = handle_frame(req, ctx, (const char *)buf);
    }
    free(buf);
    return handle_ret;
}

static void beacon_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "beacon: socket() failed, ビーコン送信を諦めます (自動発見無効)");
        s_beacon_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(BEACON_PORT),
    };
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    while (!s_beacon_stop_requested) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            char payload[160];
            int n = snprintf(payload, sizeof(payload),
                              "{\"src\":\"alc-gw\",\"type\":\"beacon\",\"ws\":\"ws://%s:%d\",\"fw\":\"p4-hub-link\"}",
                              ip_str, HUB_WS_PORT);
            if (n > 0 && (size_t)n < sizeof(payload)) {
                sendto(sock, payload, (size_t)n, 0, (struct sockaddr *)&dest, sizeof(dest));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BEACON_INTERVAL_MS));
    }
    close(sock);
    s_beacon_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t hub_link_start(void) {
    if (s_server != NULL) {
        // hub_link_stop() 後の再開で呼び直しても安全 (OTA失敗時の resume 等)。
        return ESP_OK;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HUB_WS_PORT;
    // introspect/hub-token の TLS ハンドシェイクを ws_handler (httpd worker
    // タスク) 内から同期呼び出しするため、既定 (4096) より大きく確保する
    // (alc-app-s3 の auth_mint スレッド 20KB を参考に安全側へ)
    config.stack_size = 16 * 1024;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    err = httpd_register_uri_handler(server, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler failed: %s", esp_err_to_name(err));
        httpd_stop(server);
        return err;
    }
    s_server = server;

    s_beacon_stop_requested = false;
    if (xTaskCreate(beacon_task, "hub_beacon", 4096, NULL, 5, &s_beacon_task) != pdPASS) {
        ESP_LOGE(TAG, "beacon task の起動に失敗 (自動発見無効、NVS の GW URL 手動設定相当の手段は今回未実装)");
        s_beacon_task = NULL;
    }

    ESP_LOGI(TAG, "hub_link: WS server listening on :%d, beacon on udp :%d", HUB_WS_PORT, BEACON_PORT);
    return ESP_OK;
}

esp_err_t hub_link_stop(void) {
    if (s_server == NULL) {
        return ESP_OK;
    }

    if (s_beacon_task != NULL) {
        s_beacon_stop_requested = true;
        // beacon_task は次の BEACON_INTERVAL_MS 以内に自分で vTaskDelete する
        // (ソケットを保持したまま外部から vTaskDelete するとfdがリークするため)。
    }

    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_stop failed: %s", esp_err_to_name(err));
    }
    s_server = NULL;
    return err;
}
