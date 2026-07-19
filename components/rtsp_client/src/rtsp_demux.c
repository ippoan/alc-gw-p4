#include "rtsp_demux.h"

#include <string.h>

void rtsp_demux_init(rtsp_demux_t *d, uint8_t *frame_buf, size_t frame_buf_cap, rtsp_demux_frame_cb_t cb,
                      void *cb_ctx) {
    memset(d, 0, sizeof(*d));
    d->state = RTSP_DEMUX_WAIT_DOLLAR;
    d->frame_buf = frame_buf;
    d->frame_buf_cap = frame_buf_cap;
    d->cb = cb;
    d->cb_ctx = cb_ctx;
}

void rtsp_demux_feed(rtsp_demux_t *d, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (d->state) {
        case RTSP_DEMUX_WAIT_DOLLAR:
            if (b == '$') {
                d->state = RTSP_DEMUX_READ_CHANNEL;
            }
            // '$' 以外 (RTSPテキスト応答の残り等) は読み飛ばす
            break;

        case RTSP_DEMUX_READ_CHANNEL:
            d->channel = b;
            d->state = RTSP_DEMUX_READ_LEN_HI;
            break;

        case RTSP_DEMUX_READ_LEN_HI:
            d->frame_len = (uint16_t)((uint16_t)b << 8);
            d->state = RTSP_DEMUX_READ_LEN_LO;
            break;

        case RTSP_DEMUX_READ_LEN_LO:
            d->frame_len = (uint16_t)(d->frame_len | b);
            d->frame_pos = 0;
            if (d->frame_len == 0) {
                if (d->cb != NULL) {
                    d->cb(d->channel, d->frame_buf, 0, d->cb_ctx);
                }
                d->state = RTSP_DEMUX_WAIT_DOLLAR;
            } else if (d->frame_len > d->frame_buf_cap) {
                // バッファに収まらない。破棄してスキップ (次の$待ちには戻らず
                // このフレーム分をバイト数どおり読み飛ばしてから復帰する)
                d->state = RTSP_DEMUX_SKIP_OVERSIZED;
            } else {
                d->state = RTSP_DEMUX_READ_PAYLOAD;
            }
            break;

        case RTSP_DEMUX_READ_PAYLOAD:
            d->frame_buf[d->frame_pos++] = b;
            if (d->frame_pos == d->frame_len) {
                if (d->cb != NULL) {
                    d->cb(d->channel, d->frame_buf, d->frame_len, d->cb_ctx);
                }
                d->state = RTSP_DEMUX_WAIT_DOLLAR;
            }
            break;

        case RTSP_DEMUX_SKIP_OVERSIZED:
            d->frame_pos++;
            if (d->frame_pos == d->frame_len) {
                d->state = RTSP_DEMUX_WAIT_DOLLAR;
            }
            break;
        }
    }
}
