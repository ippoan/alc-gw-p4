#pragma once

#include "esp_err.h"

// recorder_link 経由で届く "ota" コマンドの実処理 (alc-gw-p4#15 フェーズ3)。
// esp_https_ota (advanced/handle-based API) で url からバイナリを取得し、
// 非アクティブな OTA スロットへ書き込む。ブロッキングなので呼び出し側は
// 専用タスクで呼ぶこと (recorder_link の WS 受信/ping タスクを塞がないため)。

// ota_link が進捗を呼び出し側 (recorder_link) の command_result フレームへ
// 変換してもらうためのコールバック。phase_json は CoreS3 ota.rs と同じ
// "phase" 規約の JSON オブジェクト文字列 (例:
// {"phase":"started","message":".."} / {"phase":"download","received":N,"total":N} /
// {"phase":"ok","bytes":N} / {"phase":"error","message":".."})。
typedef void (*ota_link_progress_cb_t)(const char *cmd_id, const char *phase_json, void *user);

// OTA を実行する。開始直後に hub_link を停止し relay を一時停止する。
// 成功時: esp_restart() で戻らない。失敗時: hub_link/relay を再開してから
// esp_err_t (エラー) を返す — 呼び出し側は再起動しないこと。
esp_err_t ota_link_handle_command(const char *cmd_id, const char *url,
                                   ota_link_progress_cb_t cb, void *user);

// 実行中パーティションが pending-verify 状態 (直前の OTA からの初回起動) なら
// ロールバックを解除する。通常起動時は no-op。何度呼んでも安全 — recorder_link
// が初回の認証付きWS接続に成功した時点で呼ぶこと (これを機体の健全性の
// 基準とする — recorder_link に到達できない機体は次回リセットで自動的に
// 前スロットへロールバックする設計)。
void ota_link_confirm_running_app(void);
