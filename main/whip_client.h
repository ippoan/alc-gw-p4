#pragma once

#include "esp_err.h"

// 1 回の WHIP publish セッションの状態 (SFU への DELETE 先を覚えておくだけ)。
// ゼロ初期化で使い始めてよい。
typedef struct {
    char *location; // 201 応答の Location を解決した絶対URL (DELETE先)。NULL なら未publish
} whip_client_t;

// offer_sdp を endpoint へ POST し、SDP answer を *answer_sdp_out (呼び出し側が
// free する) に格納する。成功時 whip->location を更新する (Close で使う)。
// docs/whip-convention.md の規約 (non-trickle, Bearer認証) に準拠。
esp_err_t whip_client_publish(whip_client_t *whip, const char *endpoint, const char *token,
                               const char *offer_sdp, char **answer_sdp_out);

// publish 済みなら Location へ DELETE を送る (best-effort)。未publish (location
// が NULL) なら何もしない。呼び出し後 whip->location はクリアされる。
void whip_client_close(whip_client_t *whip, const char *token);
