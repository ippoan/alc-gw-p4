#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// P4 自身の device credential (device_id/device_secret、auth-worker
// role=device-gateway、Refs ippoan/alc-app-s3#83) を NVS に永続化する。
// site_id はここでは保持しない — POST /device/hub-token の応答から都度得る
// (ippoan/auth-worker#406、GW の site_id は provisioning 時に対象 hub の
// device_id を指定して割り当て済みのサーバ側の値)。
//
// 注入手段: console_cmds.c の `cred set <id> <secret>` コマンド (シリアル)。

#define CREDENTIAL_ID_MAX_LEN 64
#define CREDENTIAL_SECRET_MAX_LEN 128

// 保存済み credential を読み出す。バッファは呼び出し側が用意し、
// NUL 終端込みで書き込む。未設定 (または壊れている) なら false。
bool credential_load(char *device_id, size_t device_id_cap, char *device_secret, size_t device_secret_cap);

// device_id/device_secret を NVS に保存する。
esp_err_t credential_save(const char *device_id, const char *device_secret);
