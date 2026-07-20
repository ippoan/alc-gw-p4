#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"

#include "ws_client.h"

static const char *TAG = "ws_client";

// テキストフレーム1通ぶんの再構成バッファ上限 (元 signaling_client.c の
// SIGNALING_MAX_FRAME と同じ値・同じ理由 — esp_websocket_client のイベントは
// 同一フレームでも payload_offset/payload_len に応じて複数回に分かれて届きうる)。
#define WS_CLIENT_MAX_FRAME (8 * 1024)

struct ws_client {
    esp_websocket_client_handle_t ws;
    ws_client_callbacks_t cbs;
    void *user_ctx;

    char *uri;
    char *headers;

    char *frame_buf;
    size_t frame_len;
    size_t frame_cap;
};

static void reset_frame_buf(struct ws_client *c) {
    free(c->frame_buf);
    c->frame_buf = NULL;
    c->frame_len = 0;
    c->frame_cap = 0;
}

static bool append_frame(struct ws_client *c, const char *data, int len) {
    if (c->frame_len + (size_t)len > WS_CLIENT_MAX_FRAME) {
        ESP_LOGW(TAG, "frame too large (>%d bytes), dropping", WS_CLIENT_MAX_FRAME);
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

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    struct ws_client *c = (struct ws_client *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        if (c->cbs.on_connected != NULL) {
            c->cbs.on_connected(c->user_ctx);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        if (c->cbs.on_disconnected != NULL) {
            c->cbs.on_disconnected(c->user_ctx);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        // op_code 0x1 = text frame。他 (binary/ping/pong/close 等) はこの
        // プロトコルでは使わないため無視する。
        if (data->op_code == 0x1 && data->data_len > 0) {
            if (!append_frame(c, data->data_ptr, data->data_len)) {
                return;
            }
            if (data->payload_offset + data->data_len >= data->payload_len) {
                if (c->cbs.on_text != NULL) {
                    c->cbs.on_text(c->frame_buf, c->user_ctx);
                }
                reset_frame_buf(c);
            }
        }
        break;
    default:
        break;
    }
}

esp_err_t ws_client_connect(const char *uri, const char *token,
                             const ws_client_callbacks_t *cbs, void *user_ctx,
                             ws_client_t **out) {
    if (uri == NULL || cbs == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct ws_client *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    c->cbs = *cbs;
    c->user_ctx = user_ctx;

    c->uri = strdup(uri);
    if (c->uri == NULL) {
        free(c);
        return ESP_ERR_NO_MEM;
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
        // backoff は呼び出し側 (relay.c / recorder_link.c) が握るため、
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

esp_err_t ws_client_send_text(ws_client_t *c, const char *text, size_t len, int timeout_ms) {
    if (c == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int sent = esp_websocket_client_send_text(c->ws, text, (int)len, pdMS_TO_TICKS(timeout_ms));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

void ws_client_close(ws_client_t *c) {
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
