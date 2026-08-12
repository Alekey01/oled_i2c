#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "weather.h"

/*
 * Servidor GATT que recibe hora y clima desde el celular (Web Bluetooth).
 *
 *   Servicio  5c8b0001-7a2e-4f1d-9c3a-1b2d4e6f8a90
 *     char    5c8b0002-...  WRITE  8 bytes     hora
 *     char    5c8b0003-...  WRITE  4 o 6 bytes clima
 *
 * Hora (little-endian):  uint32 epoch_utc | int32 offset_utc_segundos
 * Clima (little-endian): int16 temp_x10 | uint8 humedad_% | uint8 codigo_wmo
 *                        [ uint8 viento_kmh | uint8 direccion_grados/2 ]
 * Los dos ultimos bytes son opcionales: sin ellos no se muestra el viento.
 */

#define BLE_SYNC_TIME_LEN         8
#define BLE_SYNC_WEATHER_LEN      4
#define BLE_SYNC_WEATHER_LEN_WIND 6

typedef struct {
    void (*on_time)(uint32_t epoch_utc, int32_t tz_offset_sec);
    void (*on_weather)(const weather_t *w);
} ble_sync_cb_t;

typedef void (*ble_sync_progress_cb_t)(const char *paso);

esp_err_t ble_sync_start(const char *device_name, const ble_sync_cb_t *cb);
esp_err_t ble_sync_start_debug(const char *device_name, const ble_sync_cb_t *cb,
                               ble_sync_progress_cb_t progreso);

/* true mientras un celular este conectado. */
bool ble_sync_connected(void);
