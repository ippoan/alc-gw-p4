#pragma once

#include "esp_err.h"

// hub_link: CoreS3 (ippoan/alc-app-s3、gw_link.rs) が接続してくる GW 側実装
// (ippoan/alc-app-s3#83)。遠隔・無人拠点 (Windows レス) では alc-gw
// (Windows/Go) の代わりに P4 がこの役を担う。
//
// - UDP 9001 へ 5 秒毎 beacon をブロードキャストし CoreS3 に自動発見させる
//   (alc-gw の internal/discovery と同一プロトコル)
// - WS :9000 でサーバーとして listen する
// - hello → auth_challenge (nonce) → auth (CoreS3 の hub-token) →
//   introspect 検証 → 自分の hub-token を mint → auth_ok、という相互認証
//   ハンドシェイクを行う。検証失敗は auth_fail を返し切断する
// - 検証完了後に届く measurement/ble_status は今回はログ出力のみ
//   (Android への中継は別途)
//
// device credential (device_id/device_secret、role=device-gateway) は
// NVS 保存が前提 (credential.c、console_cmds.c の `cred set` で注入)。
// 未設定のまま CoreS3 が接続してきた場合は auth_fail を返す。
//
// app_eth_wait_for_ip() 完了後に呼ぶこと (IP 未確定で beacon を送っても
// 送信元アドレスが定まらないため)。
esp_err_t hub_link_start(void);
