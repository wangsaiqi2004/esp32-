idf_component_register(
    SRCS "portal_login.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_wifi esp_netif nvs_flash esp_timer
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
