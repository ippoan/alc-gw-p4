#include "rtsp_digest.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "mbedtls/md5.h"

static void md5_hex(const unsigned char *data, size_t len, char out_hex[33]) {
    unsigned char digest[16];
    mbedtls_md5(data, len, digest);
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out_hex[i * 2]     = hex[(digest[i] >> 4) & 0xf];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out_hex[32] = '\0';
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

// "key=value" または "key=\"value\"" を1つ読み取り、後続トークンの先頭位置を返す
// (末尾に達したら NULL)。区切りのカンマはこの関数側で読み飛ばす。
static const char *next_param(const char *p, char *key, size_t key_cap, char *val, size_t val_cap) {
    p = skip_ws(p);
    if (*p == ',') {
        p++;
        p = skip_ws(p);
    }
    if (*p == '\0') {
        return NULL;
    }

    size_t ki = 0;
    while (*p && *p != '=' && *p != ',' && !isspace((unsigned char)*p)) {
        if (ki + 1 < key_cap) {
            key[ki++] = *p;
        }
        p++;
    }
    key[ki] = '\0';
    if (ki == 0) {
        return NULL;
    }

    p = skip_ws(p);
    if (*p != '=') {
        // 値なしトークン。digest challenge には出現しない想定だが安全側で空値扱い
        val[0] = '\0';
        return p;
    }
    p++;
    p = skip_ws(p);

    size_t vi = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (vi + 1 < val_cap) {
                val[vi++] = *p;
            }
            p++;
        }
        if (*p == '"') {
            p++;
        }
    } else {
        while (*p && *p != ',' && !isspace((unsigned char)*p)) {
            if (vi + 1 < val_cap) {
                val[vi++] = *p;
            }
            p++;
        }
    }
    val[vi] = '\0';
    return p;
}

// qop の値 (例 "auth,auth-int") がトークンとして "auth" を含むか判定する。
// strstr だけだと "auth-int" にも "auth" が部分一致してしまうため、
// カンマ/空白区切りでトークン単位に見る。
static bool qop_value_offers_auth(const char *val) {
    const char *p = val;
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char *tok_start = p;
        while (*p && *p != ' ' && *p != ',') {
            p++;
        }
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len == 4 && strncasecmp(tok_start, "auth", 4) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t rtsp_digest_parse_challenge(const char *www_authenticate, rtsp_digest_challenge_t *out) {
    if (www_authenticate == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const char *p = skip_ws(www_authenticate);
    if (strncasecmp(p, "Digest", 6) != 0) {
        return ESP_ERR_NOT_SUPPORTED; // Basic 等
    }
    p += 6;

    bool has_realm = false, has_nonce = false;
    char key[32], val[RTSP_DIGEST_NONCE_MAX];
    while ((p = next_param(p, key, sizeof(key), val, sizeof(val))) != NULL) {
        if (strcasecmp(key, "realm") == 0) {
            snprintf(out->realm, sizeof(out->realm), "%s", val);
            has_realm = true;
        } else if (strcasecmp(key, "nonce") == 0) {
            snprintf(out->nonce, sizeof(out->nonce), "%s", val);
            has_nonce = true;
        } else if (strcasecmp(key, "opaque") == 0) {
            snprintf(out->opaque, sizeof(out->opaque), "%s", val);
            out->has_opaque = true;
        } else if (strcasecmp(key, "stale") == 0) {
            out->stale = (strcasecmp(val, "true") == 0);
        } else if (strcasecmp(key, "qop") == 0) {
            if (qop_value_offers_auth(val)) {
                snprintf(out->qop, sizeof(out->qop), "auth");
            }
            // "auth" を含まない (auth-int のみ等) 場合は qop 指定なし扱いのまま
        }
        // algorithm 等の未知パラメータは無視 (MD5 前提、v1 スコープ外)
    }

    if (!has_realm || !has_nonce) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t rtsp_digest_build_authorization(const rtsp_digest_challenge_t *ch,
                                           const char *username, const char *password,
                                           const char *method, const char *uri,
                                           const char *cnonce,
                                           char *out, size_t out_len) {
    if (ch == NULL || username == NULL || password == NULL || method == NULL || uri == NULL ||
        out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool use_qop = (ch->qop[0] != '\0');
    if (use_qop && (cnonce == NULL || cnonce[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[512];
    char ha1[33], ha2[33], response[33];
    int n;

    n = snprintf(buf, sizeof(buf), "%s:%s:%s", username, ch->realm, password);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    md5_hex((const unsigned char *)buf, (size_t)n, ha1);

    n = snprintf(buf, sizeof(buf), "%s:%s", method, uri);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    md5_hex((const unsigned char *)buf, (size_t)n, ha2);

    static const char *NC = "00000001";
    if (use_qop) {
        n = snprintf(buf, sizeof(buf), "%s:%s:%s:%s:%s:%s", ha1, ch->nonce, NC, cnonce, ch->qop, ha2);
    } else {
        n = snprintf(buf, sizeof(buf), "%s:%s:%s", ha1, ch->nonce, ha2);
    }
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    md5_hex((const unsigned char *)buf, (size_t)n, response);

    int written;
    if (use_qop) {
        written = snprintf(out, out_len,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "response=\"%s\", qop=%s, nc=%s, cnonce=\"%s\"",
            username, ch->realm, ch->nonce, uri, response, ch->qop, NC, cnonce);
    } else {
        written = snprintf(out, out_len,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
            username, ch->realm, ch->nonce, uri, response);
    }
    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (ch->has_opaque) {
        size_t used = (size_t)written;
        int extra = snprintf(out + used, out_len - used, ", opaque=\"%s\"", ch->opaque);
        if (extra < 0 || (size_t)extra >= out_len - used) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}
