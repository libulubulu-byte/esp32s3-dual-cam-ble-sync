idf_component_register(
    SRCS
        "main.c"
        "logger.c"
        "camera_hal.c"
        "sd_hal.c"
        "ble_slave.c"
        "ble_host.c"
        "uart_cmd.c"
        "wifi_hal.c"
        "upload_telegram.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        esp_system
        nvs_flash
        fatfs
        sdmmc
        esp32-camera
        bt
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_psram
        mbedtls
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-unused-function")
