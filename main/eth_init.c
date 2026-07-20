// Unit PoE-P4 (esp32p4 内蔵EMAC + IP101GRI PHY) の有線 Ethernet 起動。
//
// espressif/esp-webrtc-solution の whip_demo (solutions/common/network.c の
// CONFIG_NETWORK_USE_ETHERNET 分岐) と同じ配線: ethernet_init_all
// (espressif/ethernet_init、旧 examples/ethernet/basic の example_eth_init
// から改名・コンポーネントレジストリへ切り出し済み) → esp_netif_new
// (ESP_NETIF_DEFAULT_ETH, ifkey="ETH_DEF") → esp_eth_new_netif_glue →
// esp_eth_start。GPIO 配線は M5Unit-PoE-P4-UserDemo の実機値を
// sdkconfig.defaults に反映済み (MDC=31 / MDIO=52 / RST=51 / addr=1)。
#include <string.h>

#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "ethernet_init.h"
#include "eth_init.h"

static const char *TAG = "eth_init";

static EventGroupHandle_t s_eth_events;
#define ETH_GOT_IP_BIT BIT0

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        xEventGroupClearBits(s_eth_events, ETH_GOT_IP_BIT);
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_eth_events, ETH_GOT_IP_BIT);
}

esp_err_t app_eth_init(void) {
    s_eth_events = xEventGroupCreate();
    if (s_eth_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // ethernet_init_all (espressif/ethernet_init) は内部で ETH_EVENT に対する
    // esp_event_handler_instance_register を行うため、デフォルトイベントループが
    // 先に存在している必要がある (旧 example_eth_init 時代には無かった制約)。
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;
    esp_err_t err = ethernet_init_all(&eth_handles, &eth_port_cnt);
    if (err != ESP_OK) {
        return err;
    }
    if (eth_port_cnt == 0) {
        ESP_LOGE(TAG, "no ethernet port found");
        return ESP_FAIL;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (eth_netif == NULL) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    return esp_eth_start(eth_handles[0]);
}

bool app_eth_wait_for_ip(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(s_eth_events, ETH_GOT_IP_BIT, pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(timeout_ms));
    return (bits & ETH_GOT_IP_BIT) != 0;
}
