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

static const char *TAG = "main";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(app_eth_init());
    ESP_LOGI(TAG, "waiting for ethernet link...");
    if (!app_eth_wait_for_ip(30000)) {
        ESP_LOGW(TAG, "no IP after 30s, continuing anyway (relay will retry with backoff)");
    }

    // DTLS証明書生成の事前ウォームアップ (esp_peer 公式サンプルの推奨)
    esp_peer_pre_generate_cert();

    relay_start();
}
