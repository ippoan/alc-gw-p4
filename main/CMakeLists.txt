idf_component_register(
    SRCS "main.c" "eth_init.c" "whip_client.c" "relay.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif esp_event nvs_flash esp_http_client esp_timer
             ethernet_init esp_peer rtsp_client
)
