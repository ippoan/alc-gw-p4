#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Unit PoE-P4 の内蔵EMAC + IP101GRI PHY を起動する (docs/hardware-notes.md)。
// esp_netif_init/esp_event_loop_create_default もここで行う。
esp_err_t app_eth_init(void);

// IP 取得 (IP_EVENT_ETH_GOT_IP) をタイムアウト付きで待つ。
// app_eth_init() の後、RTSP/WHIP を始める前に呼ぶこと。
bool app_eth_wait_for_ip(uint32_t timeout_ms);
