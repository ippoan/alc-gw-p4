#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include "credential.h"
#include "auth_http.h"
#include "ws_client.h"
#include "recorder_link.h"

static const char *TAG = "recorder_link";

// docs/whip-convention.md (廃止予定) から引き継いだ relay.c と同じ 1s→60s
// 上限の指数バックオフ方針をそのまま踏襲する。
#define RECONNECT_MIN_BACKOFF_MS 1000
#define RECONNECT_MAX_BACKOFF_MS 60000
// device JWT の TTL は 3600s。relay.c の cam-relay-token 用マージン (60s) は
// 短命トークン前提のため流用せず、ネットワーク/mint遅延に十分な余裕を
// 持たせて 300s (5分) とする。
#define TOKEN_REFRESH_MARGIN_S 300
#define CONNECT_TIMEOUT_MS 15000
#define PING_INTERVAL_MS 30000
#define POLL_INTERVAL_MS 1000

typedef enum {
    REC_MSG_CONNECTED,
    REC_MSG_DISCONNECTED,
    REC_MSG_FRAME, // json は呼び出し側が free する
} rec_msg_type_t;

typedef struct {
    rec_msg_type_t type;
    char *json;
} rec_msg_t;

// out_queue が満杯なら json を free してドロップする (signaling_client と
// 同じフェイルセーフ、通常運用では発生しない想定)。
static void enqueue(QueueHandle_t q, rec_msg_type_t type, char *json) {
    rec_msg_t m = {.type = type, .json = json};
    if (xQueueSend(q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping msg type=%d", (int)type);
        free(json);
    }
}

static void rec_on_connected(void *ctx) {
    enqueue((QueueHandle_t)ctx, REC_MSG_CONNECTED, NULL);
}

static void rec_on_disconnected(void *ctx) {
    enqueue((QueueHandle_t)ctx, REC_MSG_DISCONNECTED, NULL);
}

static void rec_on_text(const char *json, void *ctx) {
    enqueue((QueueHandle_t)ctx, REC_MSG_FRAME, strdup(json));
}

// {"type":"command_result","id":<cmd_id>,"payload":<payload>} を組み立てて
// 送信する。payload の所有権を受け取る (成功・失敗どちらの経路でも消費する)。
static void send_command_result(ws_client_t *ws, const char *cmd_id, cJSON *payload) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(payload);
        return;
    }
    cJSON_AddStringToObject(root, "type", "command_result");
    cJSON_AddStringToObject(root, "id", cmd_id);
    cJSON_AddItemToObject(root, "payload", payload);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return;
    }
    ws_client_send_text(ws, json, strlen(json), 5000);
    free(json);
}

// バージョン照会: 現在の firmware version (esp_app_desc の git-describe 値) と
// 実行中パーティションラベルを返す (CoreS3 の ws_uplink.rs "version" 応答と
// 同形、alc-app-s3#21)。
static void handle_version_command(ws_client_t *ws, const char *cmd_id) {
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return;
    }
    cJSON_AddStringToObject(payload, "version", desc != NULL ? desc->version : "unknown");
    cJSON_AddStringToObject(payload, "slot", part != NULL ? part->label : "?");
    send_command_result(ws, cmd_id, payload);
}

// OTA本体はフェーズ3で実装する。それまではサーバー側を待機させないよう
// 即座にエラーを返す (CoreS3 ota.rs と同じ "phase"/"message" 形式)。
static void handle_ota_command_stub(ws_client_t *ws, const char *cmd_id) {
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return;
    }
    cJSON_AddStringToObject(payload, "phase", "error");
    cJSON_AddStringToObject(payload, "message", "ota not yet supported");
    send_command_result(ws, cmd_id, payload);
}

// 下りフレームを解釈する。id はトップレベル、action は payload オブジェクトの
// 中 (alc-app-s3 crates/hub-core/src/uplink.rs の実装で確認済みの配線)。
static void handle_frame(ws_client_t *ws, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid JSON from recorder");
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "command") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (cJSON_IsString(id) && id->valuestring != NULL) {
            const cJSON *action = cJSON_IsObject(payload) ? cJSON_GetObjectItemCaseSensitive(payload, "action")
                                                            : NULL;
            if (cJSON_IsString(action) && action->valuestring != NULL) {
                if (strcmp(action->valuestring, "version") == 0) {
                    handle_version_command(ws, id->valuestring);
                } else if (strcmp(action->valuestring, "ota") == 0) {
                    handle_ota_command_stub(ws, id->valuestring);
                }
                // measure/print/gw_url 等 (CoreS3専用) は P4 と無関係なので無視する。
            }
        }
    }
    // "pong" は無視 (keepalive応答、状態遷移なし)

    cJSON_Delete(root);
}

// 1 回の recorder WebSocket 接続のライフサイクル。戻り値: 一度でも接続できて
// いれば true (呼び出し側のバックオフリセット判定に使う、relay.c と同じ形)。
static bool run_recorder_session(void) {
    char device_id[CREDENTIAL_ID_MAX_LEN];
    char device_secret[CREDENTIAL_SECRET_MAX_LEN];
    if (!credential_load(device_id, sizeof(device_id), device_secret, sizeof(device_secret))) {
        ESP_LOGW(TAG, "device credential 未設定のため recorder_link 接続をスキップ");
        return false;
    }

    auth_http_device_token_t tok;
    esp_err_t err = auth_http_device_token(CONFIG_RELAY_AUTH_WORKER_URL, device_id, device_secret, &tok);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device token mint failed: %s", esp_err_to_name(err));
        return false;
    }
    int64_t token_expiry_us =
        esp_timer_get_time() + ((int64_t)tok.expires_in_s - TOKEN_REFRESH_MARGIN_S) * 1000000;

    QueueHandle_t q = xQueueCreate(16, sizeof(rec_msg_t));
    if (q == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        auth_http_device_token_free(&tok);
        return false;
    }

    ws_client_callbacks_t cbs = {
        .on_connected = rec_on_connected,
        .on_disconnected = rec_on_disconnected,
        .on_text = rec_on_text,
    };
    ws_client_t *ws = NULL;
    err = ws_client_connect(CONFIG_RECORDER_LINK_URL, tok.access_token, &cbs, q, &ws);
    auth_http_device_token_free(&tok);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws_client_connect failed: %s", esp_err_to_name(err));
        vQueueDelete(q);
        return false;
    }

    bool connected_once = false;
    bool up = false;
    int64_t connect_deadline_us = esp_timer_get_time() + (int64_t)CONNECT_TIMEOUT_MS * 1000;
    int64_t last_ping_us = esp_timer_get_time();

    while (1) {
        rec_msg_t msg;
        if (xQueueReceive(q, &msg, pdMS_TO_TICKS(POLL_INTERVAL_MS)) == pdTRUE) {
            switch (msg.type) {
            case REC_MSG_CONNECTED:
                ESP_LOGI(TAG, "recorder_link: connected");
                up = true;
                connected_once = true;
                break;

            case REC_MSG_DISCONNECTED:
                ESP_LOGW(TAG, "recorder_link: disconnected");
                ws_client_close(ws);
                vQueueDelete(q);
                return connected_once;

            case REC_MSG_FRAME:
                handle_frame(ws, msg.json);
                free(msg.json);
                break;
            }
        }

        if (!up && esp_timer_get_time() > connect_deadline_us) {
            ESP_LOGW(TAG, "recorder_link: connect timed out");
            ws_client_close(ws);
            vQueueDelete(q);
            return connected_once;
        }

        int64_t now_us = esp_timer_get_time();
        if (up && now_us - last_ping_us >= (int64_t)PING_INTERVAL_MS * 1000) {
            static const char ping[] = "{\"type\":\"ping\"}";
            ws_client_send_text(ws, ping, sizeof(ping) - 1, 2000);
            last_ping_us = now_us;
        }

        // token 期限が近づいたら自発的に繋ぎ直す (viewer 相当の概念が無いため
        // relay.c と異なり無条件に、次のバックオフ即時リトライで再mintする)。
        if (up && now_us >= token_expiry_us) {
            ESP_LOGI(TAG, "device token 期限間近のため再接続します");
            ws_client_close(ws);
            vQueueDelete(q);
            return true;
        }
    }
}

static void recorder_task(void *arg) {
    (void)arg;
    uint32_t backoff_ms = RECONNECT_MIN_BACKOFF_MS;
    while (1) {
        bool connected_once = run_recorder_session();
        if (connected_once) {
            backoff_ms = RECONNECT_MIN_BACKOFF_MS;
        }
        ESP_LOGW(TAG, "recorder_link session ended, retry in %lums", (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > RECONNECT_MAX_BACKOFF_MS) {
            backoff_ms = RECONNECT_MAX_BACKOFF_MS;
        }
    }
}

void recorder_link_start(void) {
    if (CONFIG_RECORDER_LINK_URL[0] == '\0') {
        ESP_LOGI(TAG, "RECORDER_LINK_URL 未設定のため recorder_link を起動しません");
        return;
    }
    xTaskCreate(recorder_task, "recorder_link", 8192, NULL, 5, NULL);
}
