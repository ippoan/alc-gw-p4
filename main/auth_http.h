#pragma once

#include <stdbool.h>
#include "esp_err.h"

// auth-worker (ippoan/auth-worker#406) の POST /device/hub-token /
// POST /device/introspect を叩く薄いクライアント (ippoan/alc-app-s3#83
// の GW 側実装)。alc-app-s3 の auth_link.rs と同じ役割で、都度 HTTPS
// POST するステートレスな関数のみを提供する (常設チャネルは持たない)。

// POST /device/hub-token の結果。access_token/site_id は呼び出し側が free() する。
typedef struct {
    char *access_token;
    int expires_in_s;
    char *site_id;
} auth_http_hub_token_t;

// nonce (呼び出し元が用意) を束縛した自分の hub-token を mint する。
// 成功時は *out に結果を書き込む (呼び出し側が auth_http_hub_token_free で解放)。
esp_err_t auth_http_hub_token(const char *base_url, const char *device_id, const char *device_secret,
                               const char *nonce, auth_http_hub_token_t *out);

void auth_http_hub_token_free(auth_http_hub_token_t *t);

// POST /device/introspect の結果。valid=false なら site_id/role は未設定 (NULL)。
typedef struct {
    bool valid;
    char *site_id; // 呼び出し側が free()
    char *role;    // 呼び出し側が free()
} auth_http_introspect_t;

// 相手 (CoreS3) から届いた token を検証する。
esp_err_t auth_http_introspect(const char *base_url, const char *device_id, const char *device_secret,
                                const char *token, auth_http_introspect_t *out);

void auth_http_introspect_free(auth_http_introspect_t *t);
