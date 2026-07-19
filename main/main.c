// alc-gw-p4: C212 の RTSP (stream2) を、拠点カメラ用シグナリング Durable
// Object 経由の P2P WebRTC で中継するファームウェア (alc-gw-p4#1)。
// Unit PoE-P4 (ESP32-P4 + IP101GRI, 有線 Ethernet のみ・無線なし)。
// 詳細は ippoan/alc-app#129 (シグナリング方式) および relay.c の説明を参照。
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "eth_init.h"
#include "relay.h"
#include "esp_peer.h"
#include "console_cmds.h"
#include "hub_link.h"

static const char *TAG = "main";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // device credential (device_id/device_secret) 注入用の `cred` コマンド
    // (ippoan/alc-app-s3#83 GW側実装)。失敗してもRTSP中継自体は続行する
    if (console_cmds_start() != ESP_OK) {
        ESP_LOGW(TAG, "console start failed (credential 注入手段なしで続行)");
    }

    ESP_ERROR_CHECK(app_eth_init());
    ESP_LOGI(TAG, "waiting for ethernet link...");
    if (!app_eth_wait_for_ip(30000)) {
        ESP_LOGW(TAG, "no IP after 30s, continuing anyway (relay will retry with backoff)");
    }

    // hub_link: CoreS3 が接続してくる GW 側 (WSサーバー :9000 + ビーコン
    // :9001、遠隔・無人拠点で alc-gw の代わりを担う、ippoan/alc-app-s3#83)
    if (hub_link_start() != ESP_OK) {
        ESP_LOGW(TAG, "hub_link start failed (CoreS3 連携なしで続行)");
    }

    // DTLS証明書生成の事前ウォームアップ (esp_peer 公式サンプルの推奨)
    esp_peer_pre_generate_cert();

    relay_start();
}
