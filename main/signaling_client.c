#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "cJSON.h"

#include "ws_client.h"
#include "signaling_client.h"

static const char *TAG = "signaling_client";

struct signaling_client {
    ws_client_t *ws;
    QueueHandle_t out_queue;
};

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

static void sig_on_connected(void *ctx) {
    enqueue((struct signaling_client *)ctx, SIGNALING_MSG_CONNECTED, NULL);
}

static void sig_on_disconnected(void *ctx) {
    enqueue((struct signaling_client *)ctx, SIGNALING_MSG_DISCONNECTED, NULL);
}

static void sig_on_text(const char *json, void *ctx) {
    handle_json_message((struct signaling_client *)ctx, json);
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
    char *uri = malloc(uri_cap);
    if (uri == NULL) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(uri, uri_cap, "%s%srole=device", endpoint, strchr(endpoint, '?') != NULL ? "&" : "?");
    if (n < 0 || (size_t)n >= uri_cap) {
        free(uri);
        free(c);
        return ESP_ERR_INVALID_SIZE;
    }

    ws_client_callbacks_t cbs = {
        .on_connected = sig_on_connected,
        .on_disconnected = sig_on_disconnected,
        .on_text = sig_on_text,
    };
    esp_err_t err = ws_client_connect(uri, token, &cbs, c, &c->ws);
    free(uri);
    if (err != ESP_OK) {
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

    esp_err_t err = ws_client_send_text(c->ws, json, strlen(json), 5000);
    free(json);
    return err;
}

esp_err_t signaling_client_ping(signaling_client_t *c) {
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char ping[] = "{\"type\":\"ping\"}";
    return ws_client_send_text(c->ws, ping, sizeof(ping) - 1, 2000);
}

void signaling_client_close(signaling_client_t *c) {
    if (c == NULL) {
        return;
    }
    ws_client_close(c->ws);
    free(c);
}
