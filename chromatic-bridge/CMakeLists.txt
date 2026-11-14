idf_component_register(SRCS "udp_ws_bridge.c" "bridge_gzip.c"
                       INCLUDE_DIRS "."
                       REQUIRES cwl nw console esp_http_server esp_netif lwip mbedtls nvs_flash)
