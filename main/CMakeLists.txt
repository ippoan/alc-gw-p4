idf_component_register(
    SRCS "main.c" "eth_init.c" "signaling_client.c" "relay.c"
         "credential.c" "console_cmds.c" "auth_http.c" "hub_link.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif esp_event nvs_flash esp_timer
             ethernet_init esp_peer rtsp_client esp_websocket_client json mbedtls
             esp_http_server esp_http_client console lwip
)
