#pragma once

// admin dashboard (auth-worker /device/setup) の「接続」「バージョン」列を
// P4 (role=device-gateway) でも機能させるための cf-alc-recorder (/ws) 常設
// uplink (alc-gw-p4#15)。device JWT (POST /device/token、CONFIG_RELAY_AUTH_WORKER_URL
// 経由で mint) を Authorization: Bearer で付けて接続し、"version" コマンドに
// firmware バージョンで応答する。"ota" コマンドは本フェーズでは stub
// (フェーズ3の OTA 実装まで "not yet supported" を返すのみ)。
//
// CONFIG_RECORDER_LINK_URL が空なら起動しない (RELAY_SIGNALING_HOST と同じ規約)。
// app_eth_wait_for_ip() 完了後に呼ぶこと。
void recorder_link_start(void);
