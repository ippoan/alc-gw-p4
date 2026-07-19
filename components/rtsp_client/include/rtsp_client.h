// esp_media_protocols を置き換える自前 RTSP クライアント (alc-gw-p4#1)。
// OPTIONS→DESCRIBE(→401なら1回だけDigest再送)→SETUP→PLAY の状態遷移のみを
// 持つ。RTP-over-TCP interleavedデータの受信・復号は rtsp_demux (PR4) の責務。
// タスクは持たない (relay.c 側が rtsp_client_poll を呼ぶループを回す想定)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rtsp_sdp.h"

#ifdef __cplusplus
extern "C" {
#endif

// ソケット操作の抽象化。host テストではモック io を注入して実ネットワーク
// 無しで状態遷移を検証する。実機 (esp32p4 等) では NULL 指定時に
// rtsp_io_socket (lwip BSD socket) が既定で使われる。
typedef struct {
    // 成功で0、失敗で負値
    int (*connect)(void *io_ctx, const char *host, uint16_t port, uint32_t timeout_ms);
    // 送信できたバイト数 (全量送信できなければ負値を返す実装を想定、部分書き込みは扱わない)
    int (*send)(void *io_ctx, const uint8_t *buf, size_t len);
    // 読めたバイト数。0=timeout、負値=切断/エラー
    int (*recv)(void *io_ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    void (*close)(void *io_ctx);
} rtsp_io_ops_t;

typedef struct {
    const char *url;              // "rtsp://host[:port]/path" (認証情報はURLに埋め込まない)
    const char *username;         // NULL/空 = 認証なし
    const char *password;
    const rtsp_io_ops_t *io;      // NULL = 既定 (実機ではlwipソケット、linux targetでは非対応)
    void *io_ctx;                 // io 指定時のみ使用 (host testのモック用コンテキスト等)
    uint32_t response_timeout_ms; // 0 = 既定 (10000ms)
} rtsp_client_config_t;

typedef struct rtsp_client rtsp_client_t;

esp_err_t rtsp_client_open(const rtsp_client_config_t *cfg, rtsp_client_t **out);

// OPTIONS → DESCRIBE (401なら1回だけDigest再送) → SETUP → PLAY を実行する。
// 成功時 *video_out に SDP 解析結果 (payload type, control, SPS/PPS) を書く。
// H264以外のストリームは ESP_ERR_NOT_SUPPORTED。認証失敗は ESP_ERR_INVALID_STATE。
esp_err_t rtsp_client_play(rtsp_client_t *c, rtsp_sdp_video_t *video_out);

// best-effort。エラーが返ってもリソースリークはしない (呼び出し側は続けて
// rtsp_client_close を呼べばよい)
esp_err_t rtsp_client_teardown(rtsp_client_t *c);

void rtsp_client_close(rtsp_client_t *c);

// rtsp_client_play が PLAY 応答を読む過程で、ソケットから先読みしてしまった
// (RTP-over-TCP interleaved データの先頭を含むかもしれない) 未消費バイト列を
// 取り出す。PR4 の demux はまずこれを feed してから、以後は
// rtsp_client_get_io で得た io で recv を続けること。
// (rtsp_client 自身はタスクを持たないため、PLAY成功後の継続受信は
// 呼び出し側の責務)
size_t rtsp_client_take_buffered(rtsp_client_t *c, uint8_t *out, size_t out_cap);

void rtsp_client_get_io(rtsp_client_t *c, const rtsp_io_ops_t **io, void **io_ctx);

// SETUP応答のSessionヘッダで通知された timeout (秒)。省略時は既定の60。
// セッションが無ければ0を返す。
uint32_t rtsp_client_get_session_timeout_sec(const rtsp_client_t *c);

// Session timeoutを更新させるための定期 OPTIONS を送る (fire-and-forget)。
// TEARDOWN と同じ理由 (PLAY後はソケットにRTP-over-TCPのバイナリが流れ続けて
// おり、応答をテキストとして読もうとすると誤読しうる) で応答は読まない。
// 呼び出し側 (relay.c) の rtsp_rx_task が同じソケットを読み続けているため、
// このOPTIONS応答は rtsp_demux が '$' 待ち中の非バイナリとして読み飛ばす。
// セッションが無ければ何もせず ESP_OK を返す。
esp_err_t rtsp_client_send_keepalive(rtsp_client_t *c);

#ifdef __cplusplus
}
#endif
