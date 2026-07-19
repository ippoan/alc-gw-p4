#pragma once

// C212 の stream2 (RTSP) を WHIP publish へ中継するタスクを起動する。
// 内部で無限に再接続ループを回す (docs/whip-convention.md: 1s→60s 指数バックオフ)。
void relay_start(void);
