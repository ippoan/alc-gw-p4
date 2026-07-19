// RTP (RFC 3550) + H.264 RTP payload (RFC 6184) のdepacketization。
// 単一NALユニット/STAP-A/FU-Aに対応し、Annex-B形式 (00 00 00 01 開始コード)の
// アクセスユニットへ再構成する。esp_media_protocols を置き換える自前RTSP
// クライアントの一部 (alc-gw-p4#1)。
//
// 【v1スコープ】STAP-B/MTAP16/MTAP24/FU-B (NAL type 25-27,29) は非対応、
// 検出したら無視して継続する (これらを使うカメラは稀)。
// パケットロス (シーケンス番号ギャップ) 検出時は、構築中のアクセスユニットを
// 破棄し、次にIDR (type 5) を含むパケットが来るまで以後のデータも破棄する
// (承認済み方針: 部分的に壊れたフレームより「次のキーフレームまで待つ」)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// annexb: 1つ以上のNALユニット (各々開始コード付き) を連結したアクセスユニット。
// keyframe: このアクセスユニットにIDR (type 5) NALが含まれるか
typedef void (*h264_au_cb_t)(const uint8_t *annexb, size_t len, bool keyframe, void *ctx);

typedef struct {
    uint8_t *au_buf; // 呼び出し側が確保 (1アクセスユニット分)
    size_t au_cap;
    size_t au_len;
    bool au_has_idr;

    bool fu_active; // FU-A再構成中か

    bool have_seq;
    uint16_t last_seq;
    bool discarding; // seqギャップ検出後、次のIDRまで破棄中

    h264_au_cb_t cb;
    void *cb_ctx;
} h264_depacket_t;

void h264_depacket_init(h264_depacket_t *d, uint8_t *au_buf, size_t au_cap, h264_au_cb_t cb, void *cb_ctx);

// rtp: RTPヘッダを含むRTPパケット全体 (rtsp_demuxが渡す、RTPのchannelのみ)。
// RTPマーカービットが立ったパケット処理後にアクセスユニットをコールバックへ
// 渡す (マーカーはアクセスユニット境界を示す)。
void h264_depacket_feed_rtp(h264_depacket_t *d, const uint8_t *rtp, size_t len);

#ifdef __cplusplus
}
#endif
