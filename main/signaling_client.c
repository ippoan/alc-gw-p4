#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "signaling_client.h"

static const char *TAG = "signaling_client";

// テキストフレーム1通ぶんの再構成バッファ上限。SDPは数百B〜数KB程度の
// 想定 (esp_websocket_client のイベントは同一フレームでも payload_offset/
// payload_len に応じて複数回に分かれて届きうる)。
#define SIGNALING_MAX_FRAME (8 * 1024)

struct signaling_client {
    esp_websocket_client_handle_t ws;
    QueueHandle_t out_queue;

    char *uri;
    char *headers;

    char *frame_buf;
    size_t frame_len;
    size_t frame_cap;
};

static void reset_frame_buf(struct signaling_client *c) {
    free(c->frame_buf);
    c->frame_buf = NULL;
    c->frame_len = 0;
    c->frame_cap = 0;
}

static bool append_frame(struct signaling_client *c, const char *data, int len) {
    if (c->frame_len + (size_t)len > SIGNALING_MAX_FRAME) {
        ESP_LOGW(TAG, "frame too large (>%d bytes), dropping", SIGNALING_MAX_FRAME);
        reset_frame_buf(c);
        return false;
    }
    size_t need = c->frame_len + (size_t)len + 1;
    if (need > c->frame_cap) {
        size_t new_cap = need < 256 ? 256 : need;
        char *grown = realloc(c->frame_buf, new_cap);
        if (grown == NULL) {
            reset_frame_buf(c);
            return false;
        }
        c->frame_buf = grown;
        c->frame_cap = new_cap;
    }
    memcpy(c->frame_buf + c->frame_len, data, (size_t)len);
    c->frame_len += (size_t)len;
    c->frame_buf[c->frame_len] = '\0';
    return true;
}

// out_queue が満杯なら sdp を free してドロップする (呼び出し側のキュー
// サイズは通常運用で十分な余裕を持たせてある想定、ここでの詰まりは
// 異常系のフェイルセーフ)。
static void enqueue(struct signaling_client *c, signaling_msg_type_t type, char *sdp) {
    signaling_msg_t m = {.type = type, .sdp = sdp};
    if (xQueueSend(c->out_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "out_queue full, dropping message type=%d", (int)type);
        free(sdp);
    }
}

static void handle_json_message(struct signaling_client *c, const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid JSON from signaling server");
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "peer_joined") == 0 || strcmp(type->valuestring, "peer_left") == 0) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (cJSON_IsString(role) && role->valuestring != NULL && strcmp(role->valuestring, "admin") == 0) {
            enqueue(c,
                    strcmp(type->valuestring, "peer_joined") == 0 ? SIGNALING_MSG_PEER_JOINED_ADMIN
                                                                   : SIGNALING_MSG_PEER_LEFT_ADMIN,
                    NULL);
        }
    } else if (strcmp(type->valuestring, "sdp_answer") == 0) {
        const cJSON *sdp = cJSON_GetObjectItemCaseSensitive(root, "sdp");
        if (cJSON_IsString(sdp) && sdp->valuestring != NULL) {
            enqueue(c, SIGNALING_MSG_SDP_ANSWER, strdup(sdp->valuestring));
        }
    } else if (strcmp(type->valuestring, "ice_candidate") == 0) {
        // 受信は非対応 (non-trickle client、signaling_client.h 参照)。
        ESP_LOGW(TAG, "ignoring ice_candidate from admin (non-trickle client)");
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGW(TAG, "signaling server error: %s",
                 (cJSON_IsString(msg) && msg->valuestring != NULL) ? msg->valuestring : "(none)");
        enqueue(c, SIGNALING_MSG_SERVER_ERROR, NULL);
    }
    // "pong" は無視 (keepalive応答、状態遷移なし)

    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    struct signaling_client *c = (struct signaling_client *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        enqueue(c, SIGNALING_MSG_CONNECTED, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        enqueue(c, SIGNALING_MSG_DISCONNECTED, NULL);
        break;
    case WEBSOCKET_EVENT_DATA:
        // op_code 0x1 = text frame。他 (binary/ping/pong/close 等) はこの
        // プロトコルでは使わないため無視する。
        if (data->op_code == 0x1 && data->data_len > 0) {
            if (!append_frame(c, data->data_ptr, data->data_len)) {
                return;
            }
            if (data->payload_offset + data->data_len >= data->payload_len) {
                handle_json_message(c, c->frame_buf);
                reset_frame_buf(c);
            }
        }
        break;
    default:
        break;
    }
}

esp_err_t signaling_client_connect(const char *endpoint, const char *token, QueueHandle_t out_queue,
                                    signaling_client_t **out) {
    if (endpoint == NULL || out_queue == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct signaling_client *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    c->out_queue = out_queue;

    // role=device クエリを付与する (endpoint に既に '?' があれば '&' で連結)。
    size_t uri_cap = strlen(endpoint) + sizeof("&role=device");
    c->uri = malloc(uri_cap);
    if (c->uri == NULL) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(c->uri, uri_cap, "%s%srole=device", endpoint, strchr(endpoint, '?') != NULL ? "&" : "?");
    if (n < 0 || (size_t)n >= uri_cap) {
        free(c->uri);
        free(c);
        return ESP_ERR_INVALID_SIZE;
    }

    if (token != NULL && token[0] != '\0') {
        size_t hdr_cap = strlen(token) + sizeof("Authorization: Bearer \r\n");
        c->headers = malloc(hdr_cap);
        if (c->headers == NULL) {
            free(c->uri);
            free(c);
            return ESP_ERR_NO_MEM;
        }
        snprintf(c->headers, hdr_cap, "Authorization: Bearer %s\r\n", token);
    }

    esp_websocket_client_config_t config = {
        .uri = c->uri,
        .headers = c->headers,
        // backoff は relay.c 側 (1s→60s 指数バックオフ) が握るため、
        // クライアント自身の自動再接続は無効化する。
        .disable_auto_reconnect = true,
        // wss:// のサーバー証明書検証に ESP-IDF 同梱の CA バンドルを使う
        // (未指定だと TLS ハンドシェイクが検証不能で失敗する)。
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    c->ws = esp_websocket_client_init(&config);
    if (c->ws == NULL) {
        free(c->headers);
        free(c->uri);
        free(c);
        return ESP_FAIL;
    }
    esp_websocket_register_events(c->ws, WEBSOCKET_EVENT_ANY, ws_event_handler, c);

    esp_err_t err = esp_websocket_client_start(c->ws);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(c->ws);
        free(c->headers);
        free(c->uri);
        free(c);
        return err;
    }

    *out = c;
    return ESP_OK;
}

esp_err_t signaling_client_send_offer(signaling_client_t *c, const char *offer_sdp) {
    if (c == NULL || offer_sdp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "sdp_offer");
    cJSON_AddStringToObject(root, "sdp", offer_sdp);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(c->ws, json, (int)strlen(json), pdMS_TO_TICKS(5000));
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t signaling_client_ping(signaling_client_t *c) {
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char ping[] = "{\"type\":\"ping\"}";
    int sent = esp_websocket_client_send_text(c->ws, ping, (int)(sizeof(ping) - 1), pdMS_TO_TICKS(2000));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

void signaling_client_close(signaling_client_t *c) {
    if (c == NULL) {
        return;
    }
    if (c->ws != NULL) {
        esp_websocket_client_close(c->ws, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(c->ws);
    }
    free(c->headers);
    free(c->uri);
    free(c->frame_buf);
    free(c);
}
