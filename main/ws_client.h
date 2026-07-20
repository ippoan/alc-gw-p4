#pragma once

#include <stddef.h>
#include "esp_err.h"

// 認証付きWebSocket接続の汎用層 (元は signaling_client.c に埋め込まれていた
// esp_websocket_client 配線部分を切り出したもの、alc-gw-p4#15)。
// バックオフ (再接続間隔) は持たない — 呼び出し側 (relay.c / recorder_link.c)
// が自分のセッションループで管理する。

typedef struct ws_client ws_client_t;

// すべて esp_websocket_client のイベントタスクのコンテキストで呼ばれる。
// 短く済ませてすぐ返すこと (キューに積むだけ等) — on_text の中から
// ws_client_send_text を呼ばない (呼び出し側のタスクループに送信を集約し、
// ws ハンドルへの再入を避けるため、relay.c の既存方針と同じ)。
typedef struct {
    void (*on_connected)(void *user_ctx);
    void (*on_disconnected)(void *user_ctx); // DISCONNECTED と ERROR を統合
    void (*on_text)(const char *json, void *user_ctx); // 再構成済み1フレーム分(NUL終端)
} ws_client_callbacks_t;

// uri へ接続する (token が非空なら "Authorization: Bearer <token>" ヘッダを付与)。
// 呼び出し側が query string 等を組み立てた最終的な uri を渡すこと。
esp_err_t ws_client_connect(const char *uri, const char *token,
                             const ws_client_callbacks_t *cbs, void *user_ctx,
                             ws_client_t **out);

esp_err_t ws_client_send_text(ws_client_t *c, const char *text, size_t len, int timeout_ms);

void ws_client_close(ws_client_t *c);
