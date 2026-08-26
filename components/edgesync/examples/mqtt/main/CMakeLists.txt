idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES edgesync nvs_flash esp_wifi esp_netif esp_event
)
