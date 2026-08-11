idf_component_register(SRCS "bitcat.c" "weather_icon.c" "main.c"
                            "ssd1306.c"
                            "ble_sync.c"
                            "weather.c"
                            "ota.c"
                       REQUIRES esp_driver_i2c
                                esp_driver_gpio
                                bt
                                esp_timer
                                esp_pm
                                nvs_flash
                                app_update
                       INCLUDE_DIRS ".")
