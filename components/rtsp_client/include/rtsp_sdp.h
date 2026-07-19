// RTSP DESCRIBE 応答本文 (SDP, RFC 4566) から H.264 の映像トラック情報を
// 抽出する。esp_media_protocols を置き換える自前 RTSP クライアントの一部
// (alc-gw-p4#1)。m=audio 等、映像以外のメディアは読み飛ばす。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTSP_SDP_CONTROL_MAX 128
// H.264 の SPS/PPS はごく小さい (実務上は十数〜数十バイト) が、余裕を見て
// 128/64 バイトを上限にする。超える場合は sprop 由来の SPS/PPS を諦め
// (has_sprop=false) インバンド NAL 前提にフォールバックする。
#define RTSP_SDP_SPS_MAX 128
#define RTSP_SDP_PPS_MAX 64

typedef struct {
    int payload_type;                     // m=video で確定した動的 payload type (H264)
    char control[RTSP_SDP_CONTROL_MAX];    // 映像 m-line の a=control 値 (相対/絶対/"*" のまま保持、
                                           // 絶対URL解決は rtsp_client 側の責務)
    bool has_control;                     // 映像m-line直下にa=controlが無ければ false
                                           // (呼び出し側は Content-Base やリクエストURIへの
                                           // フォールバックを検討すること)
    uint8_t sps[RTSP_SDP_SPS_MAX];
    size_t sps_len;
    uint8_t pps[RTSP_SDP_PPS_MAX];
    size_t pps_len;
    bool has_sprop;                       // false ならインバンド SPS/PPS 前提 (SDPに無いか、
                                           // デコードに失敗しバッファ上限を超えた場合)
} rtsp_sdp_video_t;

// sdp: DESCRIBE 応答本文 (Content-Type: application/sdp)。NUL終端は不要、len で範囲指定。
// H.264 の m=video セクション (a=rtpmap で "H264" と確定したもの) が
// 1つも無ければ ESP_ERR_NOT_SUPPORTED。複数ある場合は最初の1つを採用する。
esp_err_t rtsp_sdp_parse(const char *sdp, size_t len, rtsp_sdp_video_t *out);

#ifdef __cplusplus
}
#endif
