// WHIP (RFC 9725) publish のシグナリングのみを担う薄いクライアント。
//
// espressif/esp-webrtc-solution の whip_signal コンポーネントは
// https_client.h (apprtc_signal 側) への依存が未宣言でビルドが通らない
// ことをソース調査で確認したため使わない。ここでは ippoan/alc-gw の
// internal/whip/client.go と同じロジックを esp_http_client で直接書く
// (POST offer → 201+Location+answer、DELETE)。docs/whip-convention.md 参照。
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_log.h"

#include "whip_client.h"

static const char *TAG = "whip_client";

typedef struct {
    char *body;
    size_t body_len;
    char *location; // 生の Location ヘッダ値 (絶対/相対どちらもあり得る)
} whip_resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    whip_resp_ctx_t *ctx = (whip_resp_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "Location") == 0) {
            free(ctx->location);
            ctx->location = strdup(evt->header_value);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            char *grown = realloc(ctx->body, ctx->body_len + evt->data_len + 1);
            if (grown == NULL) {
                return ESP_FAIL;
            }
            ctx->body = grown;
            memcpy(ctx->body + ctx->body_len, evt->data, evt->data_len);
            ctx->body_len += evt->data_len;
            ctx->body[ctx->body_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// endpoint を base に Location (絶対 or 相対) を解決する。ESP-IDF に汎用の
// URL resolver が無いための簡易実装: 絶対URLはそのまま、それ以外は
// endpoint の scheme://host[:port] 部分と結合する (WHIP SFU は同一オリジンで
// Location を返すのが通例のため実用上十分、alc-gw の resolveLocation と同じ割り切り)。
static char *resolve_location(const char *endpoint, const char *location) {
    if (location == NULL) {
        return NULL;
    }
    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
        return strdup(location);
    }

    const char *scheme_end = strstr(endpoint, "://");
    if (scheme_end == NULL) {
        return strdup(location);
    }
    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');
    size_t origin_len = (path_start != NULL) ? (size_t)(path_start - endpoint) : strlen(endpoint);

    size_t loc_len = strlen(location);
    bool need_slash = (loc_len == 0 || location[0] != '/');
    char *out = malloc(origin_len + (need_slash ? 1 : 0) + loc_len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, endpoint, origin_len);
    size_t pos = origin_len;
    if (need_slash) {
        out[pos++] = '/';
    }
    memcpy(out + pos, location, loc_len);
    pos += loc_len;
    out[pos] = '\0';
    return out;
}

static void set_bearer_auth(esp_http_client_handle_t client, const char *token) {
    if (token == NULL || token[0] == '\0') {
        return;
    }
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth);
}

esp_err_t whip_client_publish(whip_client_t *whip, const char *endpoint, const char *token,
                               const char *offer_sdp, char **answer_sdp_out) {
    *answer_sdp_out = NULL;
    whip_resp_ctx_t ctx = {0};

    esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/sdp");
    set_bearer_auth(client, token);
    esp_http_client_set_post_field(client, offer_sdp, strlen(offer_sdp));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "publish request failed: %s", esp_err_to_name(err));
        free(ctx.body);
        free(ctx.location);
        return err;
    }
    if (status != 201) {
        ESP_LOGE(TAG, "publish failed: HTTP %d: %s", status, ctx.body != NULL ? ctx.body : "");
        free(ctx.body);
        free(ctx.location);
        return ESP_FAIL;
    }
    if (ctx.location == NULL) {
        ESP_LOGE(TAG, "201 response missing Location header");
        free(ctx.body);
        return ESP_FAIL;
    }

    free(whip->location);
    whip->location = resolve_location(endpoint, ctx.location);
    free(ctx.location);

    *answer_sdp_out = ctx.body; // 所有権を呼び出し側へ
    return ESP_OK;
}

void whip_client_close(whip_client_t *whip, const char *token) {
    if (whip->location == NULL) {
        return;
    }

    esp_http_client_config_t config = {
        .url = whip->location,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) {
        set_bearer_auth(client, token);
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete failed (best-effort): %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

    free(whip->location);
    whip->location = NULL;
}
