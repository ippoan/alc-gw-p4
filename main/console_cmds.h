#pragma once

#include "esp_err.h"

// UART 上に esp_console REPL を立ち上げ、device credential 注入用の
// `cred` コマンドを登録する。CoreS3 (alc-app-s3) の `AUTH SET` シリアル
// コマンドに相当するが、alc-app-s3 のような独自行プロトコルは持たず
// ESP-IDF 標準の esp_console を使う (使い方は `cred help`)。
esp_err_t console_cmds_start(void);
