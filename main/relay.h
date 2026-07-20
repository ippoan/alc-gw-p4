#pragma once

// C212 の stream2 (RTSP) を、拠点カメラ用シグナリング Durable Object
// (ippoan/alc-app cf-alc-signaling の CameraSignalingRoom、
// ippoan/alc-app#129) 経由の P2P WebRTC で admin (管理者ブラウザ) へ
// 中継するタスクを起動する。内部で無限に再接続ループを回す
// (signaling 切断時のみ 1s→60s 指数バックオフ)。
void relay_start(void);

// OTA (alc-gw-p4#15 フェーズ3) 中に CPU/メモリを空けるための一時停止/再開。
// 協調的なフラグ方式 (vTaskSuspendは使わない — TLSハンドシェイク等の途中で
// 止まるリスクがあるため)。relay_pause() は現在のsignalingセッションを
// クリーンに閉じてから idle に入る。relay_resume() で再開する。
void relay_pause(void);
void relay_resume(void);
