// RTSP over TCP の interleaved フレーミング ($<channel><len16><data>) を
// バイト単位のステートマシンで解いて、チャネル毎のペイロードをコールバックへ
// 渡す。RTSPテキスト応答とバイナリRTP/RTCPが同一ソケットに混在するため、
// 行バッファでは処理できず、recv()の任意の分割位置をまたいでも継続できる
// 必要がある (alc-gw-p4#1 の最大リスク項目)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rtsp_demux_frame_cb_t)(uint8_t channel, const uint8_t *payload, size_t len, void *ctx);

typedef enum {
    RTSP_DEMUX_WAIT_DOLLAR = 0,
    RTSP_DEMUX_READ_CHANNEL,
    RTSP_DEMUX_READ_LEN_HI,
    RTSP_DEMUX_READ_LEN_LO,
    RTSP_DEMUX_READ_PAYLOAD,
    RTSP_DEMUX_SKIP_OVERSIZED,
} rtsp_demux_state_t;

typedef struct {
    rtsp_demux_state_t state;
    uint8_t channel;
    uint16_t frame_len;
    uint16_t frame_pos;

    uint8_t *frame_buf; // 呼び出し側が確保 (1フレーム分、最大65535バイト)
    size_t frame_buf_cap;

    rtsp_demux_frame_cb_t cb;
    void *cb_ctx;
} rtsp_demux_t;

void rtsp_demux_init(rtsp_demux_t *d, uint8_t *frame_buf, size_t frame_buf_cap, rtsp_demux_frame_cb_t cb,
                      void *cb_ctx);

// data[0..len) を1バイトずつ処理する。呼び出しをまたいで状態を保持するため、
// recv() が返す任意のサイズ・任意の境界で呼んでよい。
// '$' 以外の待機中バイト (RTSPテキスト応答等が紛れ込んだ場合) は読み飛ばす。
// フレーム長が frame_buf_cap を超える場合はそのフレームを安全にスキップする
// (バッファオーバーランはしない、コールバックも呼ばれない)。
void rtsp_demux_feed(rtsp_demux_t *d, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
