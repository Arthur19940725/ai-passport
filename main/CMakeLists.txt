if(CONFIG_PASSPORT_GROK_MCP)
    idf_component_register(
        SRCS "grok_app.c" "grok_network.c" "grok_protocol.c"
        INCLUDE_DIRS "."
        REQUIRES bsp esp_event esp_netif esp_timer esp_wifi nvs_flash
                 esp_http_client mbedtls json esp_system esp_hw_support
    )
else()
idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)
endif()
