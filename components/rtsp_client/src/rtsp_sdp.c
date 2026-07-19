#include "rtsp_sdp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "mbedtls/base64.h"

// [line, line+len) の1行を切り出し、次の行の開始位置を返す (行末の \r\n / \n
// は line_len に含めない)。呼び出し側は cursor < end の間だけ呼ぶ前提
// (この前提が壊れると NULL を返すので、その場合は呼び出し側のバグ)。
static const char *next_line(const char *p, const char *end, const char **line, size_t *line_len) {
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

static bool line_is(const char *line, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    return len >= plen && strncmp(line, prefix, plen) == 0;
}

// [p, end) の先頭から10進数を読み取り、*out に格納して p を読み終えた位置まで
// 進める。非数字なら false (p は変更しない)。line が NUL 終端でない前提なので
// strtol は使わず、範囲 (end) を超えて読まない手書きスキャンにする。
static bool scan_uint(const char **pp, const char *end, long *out) {
    const char *p = *pp;
    if (p >= end || !isdigit((unsigned char)*p)) {
        return false;
    }
    long v = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    *pp = p;
    return true;
}

// "m=<media> <port> <proto> <fmt> ..." が指定した media (例 "video") かどうか
// を判定し、先頭の fmt (payload type) を *out_pt に取り出す。
static bool line_starts_with_media(const char *line, size_t len, const char *media, int *out_pt) {
    char prefix[16];
    int plen = snprintf(prefix, sizeof(prefix), "m=%s ", media);
    if (plen < 0 || len < (size_t)plen || strncmp(line, prefix, (size_t)plen) != 0) {
        return false;
    }
    const char *p = line + plen;
    const char *end = line + len;
    // port を読み飛ばす
    while (p < end && *p != ' ') p++;
    while (p < end && *p == ' ') p++;
    // proto (例 "RTP/AVP") を読み飛ばす
    while (p < end && *p != ' ') p++;
    while (p < end && *p == ' ') p++;
    long pt;
    if (!scan_uint(&p, end, &pt)) {
        return false;
    }
    *out_pt = (int)pt;
    return true;
}

// "a=rtpmap:<pt> <codec>/<clockrate>..." から pt と codec 名を取り出す
static bool parse_rtpmap(const char *line, size_t len, int *pt, char *codec, size_t codec_cap) {
    const char *prefix = "a=rtpmap:";
    size_t plen = strlen(prefix);
    if (len < plen) {
        return false;
    }
    const char *p = line + plen;
    const char *end = line + len;
    long v;
    if (!scan_uint(&p, end, &v)) {
        return false;
    }
    *pt = (int)v;
    while (p < end && *p == ' ') p++;
    size_t ci = 0;
    while (p < end && *p != '/') {
        if (ci + 1 < codec_cap) {
            codec[ci++] = *p;
        }
        p++;
    }
    codec[ci] = '\0';
    return ci > 0;
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

// "...key=value;..." の value 部分 (次の ';' または行末まで、末尾の空白は除去)
// を取り出す
static bool extract_fmtp_param(const char *line, size_t len, const char *key,
                                const char **val_start, size_t *val_len) {
    const char *end = line + len;
    const char *key_pos;
    if (!find_substr(line, len, key, &key_pos)) {
        return false;
    }
    const char *v = key_pos + strlen(key);
    const char *v_end = v;
    while (v_end < end && *v_end != ';') {
        v_end++;
    }
    while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) {
        v_end--;
    }
    *val_start = v;
    *val_len = (size_t)(v_end - v);
    return true;
}

// "sprop-parameter-sets=<SPS>,<PPS>[,...]" の値部分 (カンマ区切り、base64)
// を SPS/PPS それぞれデコードして out に書き込む。SPS のデコードに成功した
// 場合のみ has_sprop=true にする (PPS 単体の欠落は許容)。
static void decode_sprop(const char *val, size_t val_len, rtsp_sdp_video_t *out) {
    const char *val_end = val + val_len;
    const char *comma = memchr(val, ',', val_len);
    const char *sps_str = val;
    size_t sps_len = comma ? (size_t)(comma - val) : val_len;

    size_t olen = 0;
    if (mbedtls_base64_decode(out->sps, sizeof(out->sps), &olen,
                               (const unsigned char *)sps_str, sps_len) == 0) {
        out->sps_len = olen;
        out->has_sprop = true;
    }

    if (comma != NULL) {
        const char *pps_str = comma + 1;
        const char *next_comma = memchr(pps_str, ',', (size_t)(val_end - pps_str));
        size_t pps_len = next_comma ? (size_t)(next_comma - pps_str) : (size_t)(val_end - pps_str);
        if (mbedtls_base64_decode(out->pps, sizeof(out->pps), &olen,
                                   (const unsigned char *)pps_str, pps_len) == 0) {
            out->pps_len = olen;
        }
    }
}

// 現在解析中のセクション状態 (m=行を跨いで保持し、セクション終端で out に確定する)
typedef struct {
    bool have_section;
    bool is_video;
    int pt;
    bool rtpmap_h264;
    char control[RTSP_SDP_CONTROL_MAX];
    bool has_control;
    const char *sprop;
    size_t sprop_len;
} sdp_section_t;

static void finalize_section(const sdp_section_t *s, rtsp_sdp_video_t *out, bool *video_found) {
    if (*video_found || !s->have_section || !s->is_video || !s->rtpmap_h264) {
        return;
    }
    out->payload_type = s->pt;
    if (s->has_control) {
        snprintf(out->control, sizeof(out->control), "%s", s->control);
        out->has_control = true;
    }
    if (s->sprop != NULL) {
        decode_sprop(s->sprop, s->sprop_len, out);
    }
    *video_found = true;
}

esp_err_t rtsp_sdp_parse(const char *sdp, size_t len, rtsp_sdp_video_t *out) {
    if (sdp == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const char *end = sdp + len;
    const char *cursor = sdp;
    sdp_section_t section = {0};
    bool video_found = false;

    while (cursor < end) {
        const char *line;
        size_t line_len;
        cursor = next_line(cursor, end, &line, &line_len);

        if (line_len >= 2 && line[0] == 'm' && line[1] == '=') {
            finalize_section(&section, out, &video_found);

            section.have_section = true;
            int pt;
            section.is_video = line_starts_with_media(line, line_len, "video", &pt);
            section.pt = section.is_video ? pt : -1;
            section.rtpmap_h264 = false;
            section.has_control = false;
            section.sprop = NULL;
            section.sprop_len = 0;
            continue;
        }

        if (!section.have_section || !section.is_video || video_found) {
            continue; // セッションレベル行、非映像セクション、または確定済み
        }

        if (line_is(line, line_len, "a=rtpmap:")) {
            int pt;
            char codec[16];
            if (parse_rtpmap(line, line_len, &pt, codec, sizeof(codec)) &&
                pt == section.pt && strcasecmp(codec, "H264") == 0) {
                section.rtpmap_h264 = true;
            }
        } else if (line_is(line, line_len, "a=fmtp:")) {
            const char *p = line + strlen("a=fmtp:");
            const char *lend = line + line_len;
            long pt;
            if (scan_uint(&p, lend, &pt) && (int)pt == section.pt) {
                const char *v;
                size_t vlen;
                if (extract_fmtp_param(line, line_len, "sprop-parameter-sets=", &v, &vlen)) {
                    section.sprop = v;
                    section.sprop_len = vlen;
                }
            }
        } else if (line_is(line, line_len, "a=control:")) {
            const char *v = line + strlen("a=control:");
            size_t vlen = line_len - strlen("a=control:");
            size_t n = vlen < sizeof(section.control) - 1 ? vlen : sizeof(section.control) - 1;
            memcpy(section.control, v, n);
            section.control[n] = '\0';
            section.has_control = true;
        }
    }

    finalize_section(&section, out, &video_found);

    return video_found ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
