#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

#include "hub_link.h"
#include "relay.h"
#include "ota_link.h"

static const char *TAG = "ota_link";

// ダウンロード進捗の送信間隔 (WS/DOへの過剰送信を避けるためのスロットリング)
#define DOWNLOAD_REPORT_STEP_PCT 10
// SRAMのみの実機のピークメモリを抑えるため Range リクエストで分割取得する
// (esp_https_ota の partial_http_download)。relay.c の H264_AU_BUF_SIZE と
// 同じ「実機で確保できる」目安の値
#define OTA_HTTP_REQUEST_SIZE (64 * 1024)
#define OTA_HTTP_TIMEOUT_MS 15000

static void send_result(ota_link_progress_cb_t cb, const char *cmd_id, void *user, cJSON *payload) {
    if (cb == NULL) {
        cJSON_Delete(payload);
        return;
    }
    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (json == NULL) {
        return;
    }
    cb(cmd_id, json, user);
    free(json);
}

static void send_started(ota_link_progress_cb_t cb, const char *cmd_id, void *user) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        return;
    }
    cJSON_AddStringToObject(p, "phase", "started");
    cJSON_AddStringToObject(p, "message", "firmware update starting");
    send_result(cb, cmd_id, user, p);
}

static void send_download(ota_link_progress_cb_t cb, const char *cmd_id, void *user, int received, int total) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        return;
    }
    cJSON_AddStringToObject(p, "phase", "download");
    cJSON_AddNumberToObject(p, "received", received);
    cJSON_AddNumberToObject(p, "total", total);
    send_result(cb, cmd_id, user, p);
}

static void send_ok(ota_link_progress_cb_t cb, const char *cmd_id, void *user, int bytes) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        return;
    }
    cJSON_AddStringToObject(p, "phase", "ok");
    cJSON_AddNumberToObject(p, "bytes", bytes);
    send_result(cb, cmd_id, user, p);
}

// 失敗経路を一本化する: エラーを command_result で送り、hub_link/relay を
// 再開してから err を返す。全失敗経路がここを通ることで resume 漏れを防ぐ。
static esp_err_t fail(ota_link_progress_cb_t cb, const char *cmd_id, void *user, const char *message,
                       esp_err_t err) {
    ESP_LOGE(TAG, "ota failed: %s", message);
    cJSON *p = cJSON_CreateObject();
    if (p != NULL) {
        cJSON_AddStringToObject(p, "phase", "error");
        cJSON_AddStringToObject(p, "message", message);
        send_result(cb, cmd_id, user, p);
    }
    if (hub_link_start() != ESP_OK) {
        ESP_LOGW(TAG, "ota: hub_link 再開に失敗 (次回起動まで CoreS3 連携なし)");
    }
    relay_resume();
    return err;
}

esp_err_t ota_link_handle_command(const char *cmd_id, const char *url, ota_link_progress_cb_t cb, void *user) {
    // OTA 中は CPU/メモリを空けるため、既存の常時接続2本を止める/一時停止する。
    relay_pause();
    if (hub_link_stop() != ESP_OK) {
        ESP_LOGW(TAG, "ota: hub_link_stop failed (続行するが CoreS3 接続が残る可能性がある)");
    }

    send_started(cb, cmd_id, user);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .partial_http_download = true,
        .max_http_request_size = OTA_HTTP_REQUEST_SIZE,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        return fail(cb, cmd_id, user, esp_err_to_name(err), err);
    }

    int total = esp_https_ota_get_image_size(handle);
    int last_reported_pct = -DOWNLOAD_REPORT_STEP_PCT;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int received = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            int pct = (int)(100LL * received / total);
            if (pct >= last_reported_pct + DOWNLOAD_REPORT_STEP_PCT) {
                send_download(cb, cmd_id, user, received, total);
                last_reported_pct = pct;
            }
        }
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return fail(cb, cmd_id, user, esp_err_to_name(err), err);
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        return fail(cb, cmd_id, user, "incomplete image data received", ESP_ERR_INVALID_SIZE);
    }

    int total_read = esp_https_ota_get_image_len_read(handle);
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        return fail(cb, cmd_id, user, esp_err_to_name(err), err);
    }

    send_ok(cb, cmd_id, user, total_read);
    ESP_LOGI(TAG, "ota: success (%d bytes), rebooting", total_read);
    esp_restart();
    return ESP_OK; // 到達しない (esp_restart は戻らない)
}

void ota_link_confirm_running_app(void) {
    static bool confirmed = false;
    if (confirmed) {
        return;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "running app confirmed valid (rollback cancelled)");
        } else {
            ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(err));
        }
    }
    confirmed = true;
}
