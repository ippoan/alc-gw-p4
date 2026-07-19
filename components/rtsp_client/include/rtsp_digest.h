// RTSP/HTTP Digest 認証 (RFC 2617)。C212 のように qop を提示しない
// RFC 2069 互換の単純な digest (cnonce/nc 不要) も、qop=auth も扱う。
// esp_media_protocols には無かった認証計算をここに実装する (alc-gw-p4#1)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTSP_DIGEST_REALM_MAX  64
#define RTSP_DIGEST_NONCE_MAX  128
#define RTSP_DIGEST_QOP_MAX    16   // "" (qop 指定なし) または "auth" のみ v1 で扱う
#define RTSP_DIGEST_OPAQUE_MAX 64

typedef struct {
    char realm[RTSP_DIGEST_REALM_MAX];
    char nonce[RTSP_DIGEST_NONCE_MAX];
    char qop[RTSP_DIGEST_QOP_MAX];       // 空文字列 = qop 指定なし (RFC2069 互換)
    char opaque[RTSP_DIGEST_OPAQUE_MAX];
    bool has_opaque;
    bool stale;
} rtsp_digest_challenge_t;

// www_authenticate: サーバーが返した WWW-Authenticate ヘッダの値全体
// (例 "Digest realm=\"TP-Link IP-Camera\", nonce=\"...\"")。
// scheme が Digest でなければ ESP_ERR_NOT_SUPPORTED (Basic のみ提示等)。
// realm/nonce が欠落していれば ESP_ERR_INVALID_ARG。
// qop に "auth" を含まない digest-only の未対応 qop (auth-int 等) が来た場合は
// qop 指定なし (RFC2069 互換) として扱う — auth-int はボディハッシュを要求し
// このクライアントでは非対応のため、qop="auth" が無ければ無視する。
esp_err_t rtsp_digest_parse_challenge(const char *www_authenticate, rtsp_digest_challenge_t *out);

// Authorization ヘッダの値 ("Digest username=\"...\", ..." 全体) を out に書き込む。
// ch->qop が空なら RFC2069 形式 (response = MD5(HA1:nonce:HA2))、
// "auth" なら RFC2617 形式 (response = MD5(HA1:nonce:nc:cnonce:qop:HA2))。
// nc は "00000001" 固定 — このクライアントは 1 つの challenge (nonce) につき
// 1 リクエストしか送らないため、nc を跨いで使い回す必要が無い。
// cnonce は qop="auth" の場合のみ必須 (呼び出し側が esp_random 等で生成して
// 渡す。テストでは固定文字列を注入できる)。qop が空なら NULL でよい。
// out_len が不足する場合は ESP_ERR_INVALID_SIZE。
esp_err_t rtsp_digest_build_authorization(const rtsp_digest_challenge_t *ch,
                                           const char *username, const char *password,
                                           const char *method, const char *uri,
                                           const char *cnonce,
                                           char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
