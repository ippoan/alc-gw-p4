#include "rtsp_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "rtsp_digest.h"
#include "rtsp_sdp.h"

#if !CONFIG_IDF_TARGET_LINUX
#include "rtsp_io_socket.h"
#endif

static const char *TAG = "rtsp_client";

#define RTSP_MSG_BUF_SIZE       1536
#define RTSP_BODY_BUF_SIZE      2048
#define RTSP_HOST_MAX           128
#define RTSP_PATH_MAX           192
#define RTSP_URI_MAX            256
#define RTSP_SESSION_ID_MAX     64
#define RTSP_CNONCE_MAX         20
#define RTSP_DEFAULT_TIMEOUT_MS 10000

typedef struct {
    int status_code;
    bool has_cseq;
    int cseq;
    bool has_www_authenticate;
    char www_authenticate[256];
    bool has_session;
    char session_id[RTSP_SESSION_ID_MAX];
    uint32_t session_timeout_sec;
    bool has_content_length;
    size_t content_length;
    const uint8_t *body; // content_length分の本文 (c->body_buf を指す、次のread_responseまで有効)
} rtsp_response_t;

struct rtsp_client {
    const rtsp_io_ops_t *io;
    void *io_ctx;
    int owned_fd; // io==NULL(既定ソケット)時に使う。io_ctx はこれを指す

    char host[RTSP_HOST_MAX];
    uint16_t port;
    char path[RTSP_PATH_MAX];
    char base_uri[RTSP_URI_MAX]; // "rtsp://host:port/path" (OPTIONS/DESCRIBE/PLAYに使う)

    char username[64];
    char password[64];

    uint32_t timeout_ms;
    int cseq;

    bool have_challenge;
    rtsp_digest_challenge_t challenge;
    char cnonce[RTSP_CNONCE_MAX];
    bool have_cnonce;

    bool have_session;
    char session_id[RTSP_SESSION_ID_MAX];
    uint32_t session_timeout_sec;

    uint8_t buf[RTSP_MSG_BUF_SIZE]; // 生の受信バッファ (ヘッダ探索 + PLAY後の先読み分の持ち越し)
    size_t buf_fill;
    size_t buf_pos;

    uint8_t body_buf[RTSP_BODY_BUF_SIZE];
};

// dest_cap 未満に切り詰めてコピーする (常にNUL終端)。rtsp_digest.c の
// copy_bounded と同じ理由 (-Wformat-truncation 回避) で、この .c 内にも
// 同等のものを置く。
static void copy_bounded(char *dest, size_t dest_cap, const char *src) {
    size_t n = strlen(src);
    if (n >= dest_cap) {
        n = dest_cap - 1;
    }
    memcpy(dest, src, n);
    dest[n] = '\0';
}

// buf (容量buf_cap, 現在の書き込み位置*off) の末尾に src を追記する。
// 収まらなければ false (*off は変更しない)。常にNUL終端を保つ。
// snprintf(dest, cap, "%s", 固定長配列) の組み合わせは、GCCの
// -Wformat-truncation (両者のサイズが宣言から分かる場合に発火、-Werror)に
// 引っかかりうるため、URI組み立てはこちらを使う (rtsp_digest.c の
// copy_bounded と同じ理由)。
static bool append_str(char *buf, size_t buf_cap, size_t *off, const char *src) {
    size_t src_len = strlen(src);
    if (*off + src_len >= buf_cap) {
        return false;
    }
    memcpy(buf + *off, src, src_len);
    *off += src_len;
    buf[*off] = '\0';
    return true;
}

static bool find_substr(const char *hay, size_t hay_len, const char *needle, const char **out) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || hay_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (strncmp(hay + i, needle, needle_len) == 0) {
            *out = hay + i;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// URL解析
// ---------------------------------------------------------------------

// "rtsp://host[:port]/path" を解析する。path省略時は "/" を書く。
static esp_err_t parse_rtsp_url(const char *url, char *host, size_t host_cap,
                                 uint16_t *port, char *path, size_t path_cap) {
    const char *prefix = "rtsp://";
    size_t plen = strlen(prefix);
    if (strncmp(url, prefix, plen) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = url + plen;
    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') {
        p++;
    }
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= host_cap) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    uint16_t port_val = 554; // RTSP既定ポート
    if (*p == ':') {
        p++;
        const char *num_start = p;
        long v = 0;
        while (*p && isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            p++;
        }
        if (p == num_start) {
            return ESP_ERR_INVALID_ARG;
        }
        port_val = (uint16_t)v;
    }
    *port = port_val;

    int n = (*p == '\0') ? snprintf(path, path_cap, "/") : snprintf(path, path_cap, "%s", p);
    if (n < 0 || (size_t)n >= path_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

// sdp.control (絶対URL/"*"/相対のいずれか) から SETUP/PLAY に使う絶対URIを
// 組み立てる。相対の場合は base_uri (DESCRIBEのリクエストURI) に連結する。
// 【既知の簡略化】Content-Base ヘッダは見ておらず、常に base_uri を基準にする。
// 単純なカメラでは Content-Base がリクエストURIと同じことが多いため v1 では
// 割り切る。
static esp_err_t resolve_control_uri(const char *base_uri, const rtsp_sdp_video_t *sdp,
                                      char *out, size_t out_cap) {
    const char *control = sdp->has_control ? sdp->control : "*";
    size_t off = 0;
    if (out_cap == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    out[0] = '\0';

    bool ok;
    if (strcmp(control, "*") == 0) {
        ok = append_str(out, out_cap, &off, base_uri);
    } else if (strncmp(control, "rtsp://", 7) == 0) {
        ok = append_str(out, out_cap, &off, control);
    } else {
        size_t base_len = strlen(base_uri);
        bool need_slash = (base_len == 0 || base_uri[base_len - 1] != '/');
        ok = append_str(out, out_cap, &off, base_uri);
        if (ok && need_slash) {
            ok = append_str(out, out_cap, &off, "/");
        }
        if (ok) {
            ok = append_str(out, out_cap, &off, control);
        }
    }
    return ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

// ---------------------------------------------------------------------
// 受信バッファ管理 / 応答パース
// ---------------------------------------------------------------------

static esp_err_t conn_fill_more(rtsp_client_t *c) {
    if (c->buf_pos > 0) {
        size_t remain = c->buf_fill - c->buf_pos;
        memmove(c->buf, c->buf + c->buf_pos, remain);
        c->buf_fill = remain;
        c->buf_pos = 0;
    }
    if (c->buf_fill >= sizeof(c->buf)) {
        return ESP_ERR_INVALID_SIZE; // ヘッダが異常に長い
    }
    int n = c->io->recv(c->io_ctx, c->buf + c->buf_fill, sizeof(c->buf) - c->buf_fill, c->timeout_ms);
    if (n < 0) {
        return ESP_FAIL; // 切断
    }
    if (n == 0) {
        return ESP_ERR_TIMEOUT;
    }
    c->buf_fill += (size_t)n;
    return ESP_OK;
}

// "\r\n\r\n" (ヘッダ終端) までを読み込む。見つかったブロック (終端の空行を
// 含む) を指すポインタ/長さを返し、buf_pos をその直後まで進める。
static esp_err_t read_header_block(rtsp_client_t *c, const char **block_start, size_t *block_len) {
    while (1) {
        if (c->buf_fill - c->buf_pos >= 4) {
            for (size_t i = c->buf_pos; i + 4 <= c->buf_fill; i++) {
                if (memcmp(c->buf + i, "\r\n\r\n", 4) == 0) {
                    *block_start = (const char *)(c->buf + c->buf_pos);
                    *block_len = i + 4 - c->buf_pos;
                    c->buf_pos = i + 4;
                    return ESP_OK;
                }
            }
        }
        esp_err_t err = conn_fill_more(c);
        if (err != ESP_OK) {
            return err;
        }
    }
}

static const char *next_header_line(const char *p, const char *end, const char **line, size_t *line_len) {
    if (p >= end) {
        return NULL;
    }
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *line_end = nl ? nl : end;
    size_t len = (size_t)(line_end - p);
    if (len > 0 && p[len - 1] == '\r') {
        len--;
    }
    *line = p;
    *line_len = len;
    return nl ? nl + 1 : end;
}

static bool parse_status_line(const char *line, size_t len, int *status_code) {
    const char *p = line;
    const char *end = line + len;
    while (p < end && *p != ' ') {
        p++;
    }
    while (p < end && *p == ' ') {
        p++;
    }
    const char *num_start = p;
    long v = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }
    if (p == num_start) {
        return false;
    }
    *status_code = (int)v;
    return true;
}

static bool split_header(const char *line, size_t len, const char **key, size_t *key_len,
                          const char **val, size_t *val_len) {
    const char *colon = memchr(line, ':', len);
    if (colon == NULL) {
        return false;
    }
    *key = line;
    *key_len = (size_t)(colon - line);
    const char *v = colon + 1;
    const char *end = line + len;
    while (v < end && *v == ' ') {
        v++;
    }
    const char *v_end = end;
    while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) {
        v_end--;
    }
    *val = v;
    *val_len = (size_t)(v_end - v);
    return true;
}

static bool key_is(const char *key, size_t key_len, const char *name) {
    size_t nlen = strlen(name);
    return key_len == nlen && strncasecmp(key, name, nlen) == 0;
}

static long parse_uint_field(const char *val, size_t val_len) {
    long v = 0;
    for (size_t i = 0; i < val_len && isdigit((unsigned char)val[i]); i++) {
        v = v * 10 + (val[i] - '0');
    }
    return v;
}

static void parse_response_headers(const char *block, size_t block_len, rtsp_response_t *resp) {
    memset(resp, 0, sizeof(*resp));
    const char *p = block;
    const char *end = block + block_len;
    const char *line;
    size_t line_len;
    bool first = true;
    while ((p = next_header_line(p, end, &line, &line_len)) != NULL) {
        if (line_len == 0) {
            continue;
        }
        if (first) {
            parse_status_line(line, line_len, &resp->status_code);
            first = false;
            continue;
        }
        const char *key, *val;
        size_t key_len, val_len;
        if (!split_header(line, line_len, &key, &key_len, &val, &val_len)) {
            continue;
        }
        if (key_is(key, key_len, "CSeq")) {
            resp->cseq = (int)parse_uint_field(val, val_len);
            resp->has_cseq = true;
        } else if (key_is(key, key_len, "WWW-Authenticate")) {
            size_t n = val_len < sizeof(resp->www_authenticate) - 1 ? val_len : sizeof(resp->www_authenticate) - 1;
            memcpy(resp->www_authenticate, val, n);
            resp->www_authenticate[n] = '\0';
            resp->has_www_authenticate = true;
        } else if (key_is(key, key_len, "Session")) {
            const char *semi = memchr(val, ';', val_len);
            size_t id_len = semi ? (size_t)(semi - val) : val_len;
            size_t n = id_len < sizeof(resp->session_id) - 1 ? id_len : sizeof(resp->session_id) - 1;
            memcpy(resp->session_id, val, n);
            resp->session_id[n] = '\0';
            resp->has_session = true;
            if (semi != NULL) {
                const char *tpos;
                size_t after_semi_len = (size_t)((val + val_len) - semi);
                if (find_substr(semi, after_semi_len, "timeout=", &tpos)) {
                    const char *tv = tpos + strlen("timeout=");
                    const char *tend = val + val_len;
                    long t = 0;
                    while (tv < tend && isdigit((unsigned char)*tv)) {
                        t = t * 10 + (*tv - '0');
                        tv++;
                    }
                    resp->session_timeout_sec = (uint32_t)t;
                }
            }
        } else if (key_is(key, key_len, "Content-Length")) {
            resp->content_length = (size_t)parse_uint_field(val, val_len);
            resp->has_content_length = true;
        }
    }
}

static esp_err_t read_response(rtsp_client_t *c, int expected_cseq, rtsp_response_t *resp) {
    const char *block;
    size_t block_len;
    esp_err_t err = read_header_block(c, &block, &block_len);
    if (err != ESP_OK) {
        return err;
    }
    parse_response_headers(block, block_len, resp);
    if (!resp->has_cseq || resp->cseq != expected_cseq) {
        // CSeq不一致はプロトコル同期が壊れている兆候 (interleavedバイナリが
        // ヘッダ境界を壊した等)。ここで打ち切る (リトライしない)。
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp->has_content_length && resp->content_length > 0) {
        if (resp->content_length > sizeof(c->body_buf)) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t have = c->buf_fill - c->buf_pos;
        size_t need = resp->content_length;
        size_t from_buf = have < need ? have : need;
        memcpy(c->body_buf, c->buf + c->buf_pos, from_buf);
        c->buf_pos += from_buf;
        size_t got = from_buf;
        while (got < need) {
            int n = c->io->recv(c->io_ctx, c->body_buf + got, need - got, c->timeout_ms);
            if (n < 0) {
                return ESP_FAIL;
            }
            if (n == 0) {
                return ESP_ERR_TIMEOUT;
            }
            got += (size_t)n;
        }
        resp->body = c->body_buf;
    } else {
        resp->body = NULL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------
// リクエスト送信 / Digest再送
// ---------------------------------------------------------------------

static esp_err_t send_request(rtsp_client_t *c, const char *method, const char *uri,
                               const char *auth_line, const char *extra_lines, int cseq) {
    char session_line[96] = "";
    if (c->have_session) {
        int sn = snprintf(session_line, sizeof(session_line), "Session: %s\r\n", c->session_id);
        if (sn < 0 || (size_t)sn >= sizeof(session_line)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    char req[900];
    int n = snprintf(req, sizeof(req),
                      "%s %s RTSP/1.0\r\n"
                      "CSeq: %d\r\n"
                      "%s%s%s"
                      "\r\n",
                      method, uri, cseq, session_line, auth_line ? auth_line : "",
                      extra_lines ? extra_lines : "");
    if (n < 0 || (size_t)n >= sizeof(req)) {
        return ESP_ERR_INVALID_SIZE;
    }
    int sent = c->io->send(c->io_ctx, (const uint8_t *)req, (size_t)n);
    if (sent < 0 || (size_t)sent != n) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t build_auth_header_value(rtsp_client_t *c, const char *method, const char *uri,
                                          char *out, size_t out_len) {
    const char *cnonce = NULL;
    if (c->challenge.qop[0] != '\0') {
        if (!c->have_cnonce) {
            // 【既知の制約】qop=auth の場合 nc は "00000001" 固定のまま、同一
            // nonce を複数リクエスト (DESCRIBE→SETUP→PLAY) で使い回す。RFC上は
            // nc をリクエスト毎に増分すべきだが、対象カメラ (C212) は実機ログで
            // qop 無し (RFC2069互換) と確認済みでこの経路は通らないため、
            // v1 スコープでは対応を見送る (rtsp_digest.h 参照)。
            snprintf(c->cnonce, sizeof(c->cnonce), "%08x%08x", (unsigned)rand(), (unsigned)rand());
            c->have_cnonce = true;
        }
        cnonce = c->cnonce;
    }
    return rtsp_digest_build_authorization(&c->challenge, c->username, c->password, method, uri, cnonce, out,
                                            out_len);
}

// 401なら challenge を取得して1回だけ Authorization 付きで再送する。
// 既に challenge を持った状態 (2回目以降) で401が返れば、資格情報が誤って
// いると判断しそのまま失敗を返す (無限に再送しない)。
static esp_err_t request_with_auth(rtsp_client_t *c, const char *method, const char *uri,
                                    const char *extra_lines, rtsp_response_t *resp) {
    for (int attempt = 0; attempt < 2; attempt++) {
        char auth_line[512] = "";
        if (c->have_challenge) {
            char auth_value[400];
            esp_err_t derr = build_auth_header_value(c, method, uri, auth_value, sizeof(auth_value));
            if (derr != ESP_OK) {
                return derr;
            }
            int n = snprintf(auth_line, sizeof(auth_line), "Authorization: %s\r\n", auth_value);
            if (n < 0 || (size_t)n >= sizeof(auth_line)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        int cseq = c->cseq++;
        esp_err_t err = send_request(c, method, uri, auth_line, extra_lines, cseq);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s %s: send_request failed: %s", method, uri, esp_err_to_name(err));
            return err;
        }
        err = read_response(c, cseq, resp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s %s: read_response failed: %s", method, uri, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "%s %s -> %d", method, uri, resp->status_code);

        if (resp->status_code == 401) {
            if (!resp->has_www_authenticate) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            rtsp_digest_challenge_t new_challenge;
            esp_err_t perr = rtsp_digest_parse_challenge(resp->www_authenticate, &new_challenge);
            if (perr != ESP_OK) {
                return perr;
            }
            if (c->have_challenge && !new_challenge.stale) {
                return ESP_ERR_INVALID_STATE; // 資格情報誤り
            }
            c->challenge = new_challenge;
            c->have_challenge = true;
            c->have_cnonce = false; // 新challengeでは再生成する
            continue;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

// ---------------------------------------------------------------------
// 公開API
// ---------------------------------------------------------------------

esp_err_t rtsp_client_open(const rtsp_client_config_t *cfg, rtsp_client_t **out) {
    if (cfg == NULL || cfg->url == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rtsp_client_t *c = calloc(1, sizeof(rtsp_client_t));
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = parse_rtsp_url(cfg->url, c->host, sizeof(c->host), &c->port, c->path, sizeof(c->path));
    if (err != ESP_OK) {
        free(c);
        return err;
    }
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)c->port);
    size_t off = 0;
    c->base_uri[0] = '\0';
    if (!append_str(c->base_uri, sizeof(c->base_uri), &off, "rtsp://") ||
        !append_str(c->base_uri, sizeof(c->base_uri), &off, c->host) ||
        !append_str(c->base_uri, sizeof(c->base_uri), &off, ":") ||
        !append_str(c->base_uri, sizeof(c->base_uri), &off, port_str) ||
        !append_str(c->base_uri, sizeof(c->base_uri), &off, c->path)) {
        free(c);
        return ESP_ERR_INVALID_SIZE;
    }

    if (cfg->username != NULL) {
        copy_bounded(c->username, sizeof(c->username), cfg->username);
    }
    if (cfg->password != NULL) {
        copy_bounded(c->password, sizeof(c->password), cfg->password);
    }
    c->timeout_ms = cfg->response_timeout_ms != 0 ? cfg->response_timeout_ms : RTSP_DEFAULT_TIMEOUT_MS;
    c->cseq = 1;
    c->owned_fd = -1;

    if (cfg->io != NULL) {
        c->io = cfg->io;
        c->io_ctx = cfg->io_ctx;
    } else {
#if CONFIG_IDF_TARGET_LINUX
        // linux target (host test) には既定io実装が無い。テストは必ずモックを注入すること
        free(c);
        return ESP_ERR_NOT_SUPPORTED;
#else
        c->io = &rtsp_io_socket_ops;
        c->io_ctx = &c->owned_fd;
#endif
    }

    *out = c;
    return ESP_OK;
}

esp_err_t rtsp_client_play(rtsp_client_t *c, rtsp_sdp_video_t *video_out) {
    if (c == NULL || video_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "connecting to %s:%u ...", c->host, (unsigned)c->port);
    if (c->io->connect(c->io_ctx, c->host, c->port, c->timeout_ms) != 0) {
        ESP_LOGW(TAG, "connect to %s:%u failed", c->host, (unsigned)c->port);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connected");

    rtsp_response_t resp;

    // OPTIONS (多くのIPカメラは認証不要。401が来てもrequest_with_authが対応する)
    esp_err_t err = request_with_auth(c, "OPTIONS", c->base_uri, "", &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status_code != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // DESCRIBE
    err = request_with_auth(c, "DESCRIBE", c->base_uri, "Accept: application/sdp\r\n", &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status_code != 200 || resp.body == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    rtsp_sdp_video_t sdp;
    err = rtsp_sdp_parse((const char *)resp.body, resp.content_length, &sdp);
    if (err != ESP_OK) {
        return err; // H264が無ければ ESP_ERR_NOT_SUPPORTED がそのまま伝播
    }

    char control_uri[RTSP_URI_MAX];
    err = resolve_control_uri(c->base_uri, &sdp, control_uri, sizeof(control_uri));
    if (err != ESP_OK) {
        return err;
    }

    // SETUP: RTP=channel0, RTCP=channel1 でTCP多重化 (interleaved)
    err = request_with_auth(c, "SETUP", control_uri, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status_code != 200 || !resp.has_session) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_bounded(c->session_id, sizeof(c->session_id), resp.session_id);
    c->have_session = true;
    c->session_timeout_sec = resp.session_timeout_sec != 0 ? resp.session_timeout_sec : 60;

    // PLAY
    err = request_with_auth(c, "PLAY", c->base_uri, "Range: npt=0.000-\r\n", &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status_code != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *video_out = sdp;
    return ESP_OK;
}

esp_err_t rtsp_client_teardown(rtsp_client_t *c) {
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!c->have_session) {
        return ESP_OK;
    }
    // best-effort・fire-and-forget: PLAY成功後はソケットにRTP-over-TCPの
    // バイナリデータが届き続けており、ここで応答を読もうとすると未読の
    // RTPフレームをRTSPテキスト応答と誤認しかねない (interleavedストリーム
    // の制約、実機検証で確認)。応答は読まずリクエスト送信のみ行う。
    char auth_line[512] = "";
    if (c->have_challenge) {
        char auth_value[400];
        if (build_auth_header_value(c, "TEARDOWN", c->base_uri, auth_value, sizeof(auth_value)) == ESP_OK) {
            snprintf(auth_line, sizeof(auth_line), "Authorization: %s\r\n", auth_value);
        }
    }
    int cseq = c->cseq++;
    esp_err_t err = send_request(c, "TEARDOWN", c->base_uri, auth_line, "", cseq);
    c->have_session = false; // best-effort。応答を待たずにセッション状態はクリアする
    return err;
}

uint32_t rtsp_client_get_session_timeout_sec(const rtsp_client_t *c) {
    if (c == NULL || !c->have_session) {
        return 0;
    }
    return c->session_timeout_sec;
}

esp_err_t rtsp_client_send_keepalive(rtsp_client_t *c) {
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!c->have_session) {
        return ESP_OK; // セッション確立前 (呼び出し側の想定外だが害はない)
    }
    char auth_line[512] = "";
    if (c->have_challenge) {
        char auth_value[400];
        if (build_auth_header_value(c, "OPTIONS", c->base_uri, auth_value, sizeof(auth_value)) == ESP_OK) {
            snprintf(auth_line, sizeof(auth_line), "Authorization: %s\r\n", auth_value);
        }
    }
    int cseq = c->cseq++;
    esp_err_t err = send_request(c, "OPTIONS", c->base_uri, auth_line, "", cseq);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "keepalive OPTIONS send failed: %s", esp_err_to_name(err));
    }
    return err;
}

void rtsp_client_close(rtsp_client_t *c) {
    if (c == NULL) {
        return;
    }
    if (c->io != NULL && c->io->close != NULL) {
        c->io->close(c->io_ctx);
    }
    free(c);
}

size_t rtsp_client_take_buffered(rtsp_client_t *c, uint8_t *out, size_t out_cap) {
    if (c == NULL || out == NULL) {
        return 0;
    }
    size_t avail = c->buf_fill - c->buf_pos;
    size_t n = avail < out_cap ? avail : out_cap;
    memcpy(out, c->buf + c->buf_pos, n);
    c->buf_pos += n;
    return n;
}

void rtsp_client_get_io(rtsp_client_t *c, const rtsp_io_ops_t **io, void **io_ctx) {
    if (c == NULL || io == NULL || io_ctx == NULL) {
        return;
    }
    *io = c->io;
    *io_ctx = c->io_ctx;
}
