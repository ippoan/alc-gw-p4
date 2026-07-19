#include "h264_depacket.h"

#include <string.h>

static const uint8_t START_CODE[4] = {0, 0, 0, 1};

void h264_depacket_init(h264_depacket_t *d, uint8_t *au_buf, size_t au_cap, h264_au_cb_t cb, void *cb_ctx) {
    memset(d, 0, sizeof(*d));
    d->au_buf = au_buf;
    d->au_cap = au_cap;
    d->cb = cb;
    d->cb_ctx = cb_ctx;
}

static bool append_bytes(h264_depacket_t *d, const uint8_t *data, size_t len) {
    if (d->au_len + len > d->au_cap) {
        return false;
    }
    memcpy(d->au_buf + d->au_len, data, len);
    d->au_len += len;
    return true;
}

static void reset_au(h264_depacket_t *d) {
    d->au_len = 0;
    d->au_has_idr = false;
    d->fu_active = false;
}

static void flush_au(h264_depacket_t *d) {
    if (d->au_len > 0 && d->cb != NULL) {
        d->cb(d->au_buf, d->au_len, d->au_has_idr, d->cb_ctx);
    }
    reset_au(d);
}

static bool nal_type_is_idr(uint8_t nal_header) {
    return (nal_header & 0x1F) == 5;
}

// 単一NAL (STAP-Aの内包分も含む) を開始コード付きで追記する
static bool append_single_nal(h264_depacket_t *d, const uint8_t *nal, size_t nal_len) {
    if (nal_len == 0) {
        return true;
    }
    if (!append_bytes(d, START_CODE, sizeof(START_CODE))) {
        return false;
    }
    if (!append_bytes(d, nal, nal_len)) {
        return false;
    }
    if (nal_type_is_idr(nal[0])) {
        d->au_has_idr = true;
    }
    return true;
}

// 破棄中に、このパケット (FU-A以外) でIDRへ復帰できるか判定する
static bool packet_contains_idr(const uint8_t *payload, size_t len, uint8_t nal_unit_type) {
    if (nal_unit_type == 24) { // STAP-A
        size_t i = 1;          // STAP-Aヘッダ1バイトをスキップ
        while (i + 2 <= len) {
            uint16_t nal_size = (uint16_t)(((uint16_t)payload[i] << 8) | payload[i + 1]);
            i += 2;
            if (i + nal_size > len) {
                break;
            }
            if (nal_size > 0 && nal_type_is_idr(payload[i])) {
                return true;
            }
            i += nal_size;
        }
        return false;
    }
    if (nal_unit_type >= 1 && nal_unit_type <= 23) {
        return nal_type_is_idr(payload[0]);
    }
    return false;
}

void h264_depacket_feed_rtp(h264_depacket_t *d, const uint8_t *rtp, size_t len) {
    if (len < 12) {
        return; // RTPヘッダ未満、壊れたパケット
    }
    uint8_t version = (uint8_t)((rtp[0] >> 6) & 0x3);
    if (version != 2) {
        return; // RTPバージョン不一致
    }
    bool marker = (rtp[1] & 0x80) != 0;
    uint8_t cc = (uint8_t)(rtp[0] & 0x0F);
    bool has_ext = (rtp[0] & 0x10) != 0;
    uint16_t seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);

    size_t header_len = 12 + (size_t)cc * 4;
    if (header_len > len) {
        return;
    }
    if (has_ext) {
        if (header_len + 4 > len) {
            return;
        }
        uint16_t ext_len_words = (uint16_t)(((uint16_t)rtp[header_len + 2] << 8) | rtp[header_len + 3]);
        header_len += 4 + (size_t)ext_len_words * 4;
        if (header_len > len) {
            return;
        }
    }

    const uint8_t *payload = rtp + header_len;
    size_t payload_len = len - header_len;

    // シーケンス番号ギャップ検出 (discarding中でもlast_seqは更新し続ける)
    bool gap = d->have_seq && (uint16_t)(seq - d->last_seq) != 1;
    d->have_seq = true;
    d->last_seq = seq;
    if (gap) {
        d->discarding = true;
        reset_au(d);
    }

    if (payload_len == 0) {
        return;
    }
    uint8_t nal_unit_type = (uint8_t)(payload[0] & 0x1F);

    if (d->discarding) {
        if (nal_unit_type == 28) { // FU-A
            if (payload_len < 2) {
                return;
            }
            bool s = (payload[1] & 0x80) != 0;
            uint8_t orig_type = (uint8_t)(payload[1] & 0x1F);
            if (s && orig_type == 5) {
                d->discarding = false; // このパケットは通常経路にそのまま流す
            } else {
                return; // startを取りこぼした継続断片は復帰判定できない、破棄継続
            }
        } else if (packet_contains_idr(payload, payload_len, nal_unit_type)) {
            d->discarding = false;
        } else {
            return;
        }
    }

    bool ok = true;
    if (nal_unit_type >= 1 && nal_unit_type <= 23) {
        ok = append_single_nal(d, payload, payload_len);
    } else if (nal_unit_type == 24) { // STAP-A
        size_t i = 1;
        while (i + 2 <= payload_len) {
            uint16_t nal_size = (uint16_t)(((uint16_t)payload[i] << 8) | payload[i + 1]);
            i += 2;
            if (i + nal_size > payload_len) {
                ok = false;
                break;
            }
            if (!append_single_nal(d, payload + i, nal_size)) {
                ok = false;
                break;
            }
            i += nal_size;
        }
    } else if (nal_unit_type == 28) { // FU-A
        if (payload_len < 2) {
            return;
        }
        uint8_t indicator = payload[0];
        uint8_t fu_header = payload[1];
        bool s = (fu_header & 0x80) != 0;
        bool e = (fu_header & 0x40) != 0;
        const uint8_t *frag = payload + 2;
        size_t frag_len = payload_len - 2;

        if (s) {
            uint8_t reconstructed = (uint8_t)((indicator & 0xE0) | (fu_header & 0x1F));
            ok = append_bytes(d, START_CODE, sizeof(START_CODE)) && append_bytes(d, &reconstructed, 1) &&
                 append_bytes(d, frag, frag_len);
            if (ok && nal_type_is_idr(reconstructed)) {
                d->au_has_idr = true;
            }
            d->fu_active = ok;
        } else if (d->fu_active) {
            ok = append_bytes(d, frag, frag_len);
            if (!ok) {
                d->fu_active = false;
            }
        } else {
            ok = false; // startを取りこぼした継続断片
        }
        if (e) {
            d->fu_active = false;
        }
    }
    // 未対応NAL type (STAP-B/MTAP/FU-B等) は無視して継続 (ok=trueのまま)

    if (!ok) {
        // バッファ超過や不整合。このAUを諦めて次のキーフレームまで破棄
        d->discarding = true;
        reset_au(d);
        return;
    }

    if (marker) {
        flush_au(d);
    }
}
