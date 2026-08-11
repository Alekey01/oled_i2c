# La clave privada del OTA cifrado viaja dentro del binario: es lo que hace que
# solo el reloj pueda descifrar la imagen publicada. No esta en el repositorio,
# se genera una vez y se guarda aparte (ver README).
idf_component_register(SRCS "bitcat.c" "weather_icon.c" "main.c"
                            "ssd1306.c"
                            "ble_sync.c"
                            "weather.c"
                            "ota.c"
                       EMBED_TXTFILES "ota_private_key.pem"
                       REQUIRES esp_driver_i2c
                                esp_driver_gpio
                                bt
                                esp_timer
                                esp_pm
                                nvs_flash
                                app_update
                       INCLUDE_DIRS ".")
