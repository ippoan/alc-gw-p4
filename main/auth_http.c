// auth-worker (ippoan/auth-worker#406) への都度 HTTPS POST。
// パターンは旧 whip_client.c (git 履歴 b310fe4) の esp_http_client 使用法を
// 踏襲するが、あちらに無かった crt_bundle_attach を明示する (signaling_client.c
// と同じ理由 — 未指定だと TLS 証明書検証ができずハンドシェイクが失敗する)。
#include <string.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

#include "auth_http.h"

static const char *TAG = "auth_http";

#define HTTP_TIMEOUT_MS 15000
#define MAX_BODY (8 * 1024)

typedef struct {
    char *body;
    size_t body_len;
} resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || esp_http_client_is_chunked_response(evt->client)) {
        return ESP_OK;
    }
    if (ctx->body_len + (size_t)evt->data_len > MAX_BODY) {
        ESP_LOGW(TAG, "response too large, truncating");
        return ESP_OK;
    }
    char *grown = realloc(ctx->body, ctx->body_len + (size_t)evt->data_len + 1);
    if (grown == NULL) {
        return ESP_FAIL;
    }
    ctx->body = grown;
    memcpy(ctx->body + ctx->body_len, evt->data, (size_t)evt->data_len);
    ctx->body_len += (size_t)evt->data_len;
    ctx->body[ctx->body_len] = '\0';
    return ESP_OK;
}

// url へ JSON body を POST し、応答本文を *out_body (呼び出し側が free) に返す。
// auth-worker はエラー時も {"error":...} を 4xx/5xx で返すため、HTTP status
// に関わらず本文はそのまま返す (JSON 側の解釈は呼び出し元が行う)。
static esp_err_t post_json(const char *url, const char *body, char **out_body) {
    *out_body = NULL;
    resp_ctx_t ctx = {0};

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        free(ctx.body);
        return err;
    }
    if (ctx.body == NULL) {
        ctx.body = strdup("{}");
    }
    *out_body = ctx.body;
    return ESP_OK;
}

// 応答の {"error":...} を検出してログする。エラーなら true。
static bool json_has_error(const cJSON *root) {
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(err) && err->valuestring != NULL) {
        ESP_LOGW(TAG, "auth-worker error: %s", err->valuestring);
        return true;
    }
    return false;
}

static char *dup_json_string(const cJSON *root, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || v->valuestring == NULL || v->valuestring[0] == '\0') {
        return NULL;
    }
    return strdup(v->valuestring);
}

esp_err_t auth_http_hub_token(const char *base_url, const char *device_id, const char *device_secret,
                               const char *nonce, auth_http_hub_token_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "device_id", device_id);
    cJSON_AddStringToObject(req, "device_secret", device_secret);
    cJSON_AddStringToObject(req, "nonce", nonce);
    char *req_body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (req_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/device/hub-token", base_url);

    char *resp_body = NULL;
    esp_err_t err = post_json(url, req_body, &resp_body);
    free(req_body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);
    if (root == NULL) {
        return ESP_FAIL;
    }
    if (json_has_error(root) || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    out->access_token = dup_json_string(root, "access_token");
    out->site_id = dup_json_string(root, "site_id");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    out->expires_in_s = cJSON_IsNumber(expires) ? expires->valueint : 0;
    cJSON_Delete(root);

    if (out->access_token == NULL || out->site_id == NULL) {
        ESP_LOGW(TAG, "hub-token response missing access_token/site_id");
        auth_http_hub_token_free(out);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void auth_http_hub_token_free(auth_http_hub_token_t *t) {
    if (t == NULL) {
        return;
    }
    free(t->access_token);
    free(t->site_id);
    memset(t, 0, sizeof(*t));
}

esp_err_t auth_http_cam_relay_token(const char *base_url, const char *device_id, const char *device_secret,
                                     auth_http_cam_relay_token_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "device_id", device_id);
    cJSON_AddStringToObject(req, "device_secret", device_secret);
    char *req_body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (req_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/device/cam-relay-token", base_url);

    char *resp_body = NULL;
    esp_err_t err = post_json(url, req_body, &resp_body);
    free(req_body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);
    if (root == NULL) {
        return ESP_FAIL;
    }
    if (json_has_error(root) || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    out->access_token = dup_json_string(root, "access_token");
    out->site_id = dup_json_string(root, "site_id");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    out->expires_in_s = cJSON_IsNumber(expires) ? expires->valueint : 0;
    cJSON_Delete(root);

    if (out->access_token == NULL || out->site_id == NULL) {
        ESP_LOGW(TAG, "cam-relay-token response missing access_token/site_id");
        auth_http_cam_relay_token_free(out);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void auth_http_cam_relay_token_free(auth_http_cam_relay_token_t *t) {
    if (t == NULL) {
        return;
    }
    free(t->access_token);
    free(t->site_id);
    memset(t, 0, sizeof(*t));
}

esp_err_t auth_http_introspect(const char *base_url, const char *device_id, const char *device_secret,
                                const char *token, auth_http_introspect_t *out) {
    memset(out, 0, sizeof(*out));

    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "device_id", device_id);
    cJSON_AddStringToObject(req, "device_secret", device_secret);
    cJSON_AddStringToObject(req, "token", token);
    char *req_body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (req_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/device/introspect", base_url);

    char *resp_body = NULL;
    esp_err_t err = post_json(url, req_body, &resp_body);
    free(req_body);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);
    if (root == NULL) {
        return ESP_FAIL;
    }
    // 呼び出し元 (P4) の credential 自体が invalid/unauthorized な場合は
    // {"error":...} (401) — これはサーバエラーとして ESP_FAIL にする。
    // {"valid":false} は「相手のtokenが無効」という正常な判定結果なので
    // ESP_OK で返す (out->valid=false のまま)。
    if (json_has_error(root) || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *valid = cJSON_GetObjectItemCaseSensitive(root, "valid");
    out->valid = cJSON_IsBool(valid) && cJSON_IsTrue(valid);
    if (out->valid) {
        out->site_id = dup_json_string(root, "site_id");
        out->role = dup_json_string(root, "role");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

void auth_http_introspect_free(auth_http_introspect_t *t) {
    if (t == NULL) {
        return;
    }
    free(t->site_id);
    free(t->role);
    memset(t, 0, sizeof(*t));
}
