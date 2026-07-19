#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// 拠点カメラ用シグナリング Durable Object (ippoan/alc-app cf-alc-signaling
// の CameraSignalingRoom、ippoan/alc-app#129) への device 役 WebSocket
// クライアント。ippoan/alc-gw の internal/whip/signaling.go と同じ
// JSON プロトコル (sdp_offer/sdp_answer/ice_candidate/ping/pong/
// peer_joined/peer_left/error) を話す。
//
// 非トリックル: ICE candidate は個別送信せず、収集完了後の1通のSDPに
// 含める (esp_peer 側の実装変更を避ける設計判断)。admin (ブラウザ) 側が
// trickle で ice_candidate を送ってきても本クライアントは無視する。

typedef enum {
    SIGNALING_MSG_CONNECTED,          // WebSocket確立 (admin待ち状態に入る)
    SIGNALING_MSG_DISCONNECTED,       // WebSocket切断 (エラー・close問わず、呼び出し側は再接続が要る)
    SIGNALING_MSG_PEER_JOINED_ADMIN,  // adminが接続した (offerを送るタイミング)
    SIGNALING_MSG_PEER_LEFT_ADMIN,    // adminが切断した (viewerを畳む)
    SIGNALING_MSG_SDP_ANSWER,         // sdp は呼び出し側が free する
    SIGNALING_MSG_SERVER_ERROR,
} signaling_msg_type_t;

typedef struct {
    signaling_msg_type_t type;
    char *sdp; // SIGNALING_MSG_SDP_ANSWER のときのみ非NULL、呼び出し側がfree
} signaling_msg_t;

typedef struct signaling_client signaling_client_t;

// endpoint ("wss://<signaling>/cam-room/<拠点ID>", role=deviceクエリは
// この関数が付与する) へ接続を開始する (非同期、接続完了は out_queue に
// 届く SIGNALING_MSG_CONNECTED で分かる)。out_queue は呼び出し側が
// 事前に作成すること (要素サイズ sizeof(signaling_msg_t))。
esp_err_t signaling_client_connect(const char *endpoint, const char *token,
                                    QueueHandle_t out_queue, signaling_client_t **out);

// offer_sdp を送る (fire-and-forget)。応答は out_queue に
// SIGNALING_MSG_SDP_ANSWER として非同期で届く。
esp_err_t signaling_client_send_offer(signaling_client_t *c, const char *offer_sdp);

// Session timeout 相当のものはこのプロトコルには無いが、経路上の
// 中間ノードでの idle timeout に備えて定期的に打つ (best-effort)。
esp_err_t signaling_client_ping(signaling_client_t *c);

void signaling_client_close(signaling_client_t *c);
