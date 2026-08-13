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
 *     char    5c8b0008-...  WRITE  2+3n bytes  pronostico
 *
 * Hora (little-endian):  uint32 epoch_utc | int32 offset_utc_segundos
 * Clima (little-endian): int16 temp_x10 | uint8 humedad_% | uint8 codigo_wmo
 *                        [ uint8 viento_kmh | uint8 direccion_grados/2 ]
 * Los dos ultimos bytes son opcionales: sin ellos no se muestra el viento.
 *
 * Pronostico: uint8 n_horas | uint8 hora_local_de_la_primera | n x (int8
 * temperatura_C, uint8 probabilidad_lluvia_%, uint8 codigo_wmo). Grados
 * enteros y no decimas: en una pantalla de 32 px de alto la decima no se
 * distingue, y asi el pronostico entero cabe en una sola escritura.
 */

#define BLE_SYNC_TIME_LEN         8
#define BLE_SYNC_WEATHER_LEN      4
#define BLE_SYNC_WEATHER_LEN_WIND 6

/* Seis horas: cada una ocupa una columna con su hora, su icono y su
   temperatura, y en 128 px salen a 21 px por columna. Con mas, el icono se
   queda sin sitio para leerse. */
#define BLE_SYNC_FC_MAX 6

typedef struct {
    uint8_t horas;                        /* cuantas entradas valen */
    uint8_t hora0;                        /* hora local (0..23) de la primera */
    int8_t  temp[BLE_SYNC_FC_MAX];        /* grados enteros */
    uint8_t prob[BLE_SYNC_FC_MAX];        /* probabilidad de lluvia, 0..100 */
    uint8_t wmo[BLE_SYNC_FC_MAX];         /* codigo de cielo, para el icono */
} forecast_t;

typedef struct {
    void (*on_time)(uint32_t epoch_utc, int32_t tz_offset_sec);
    void (*on_weather)(const weather_t *w);
    void (*on_forecast)(const forecast_t *f);
} ble_sync_cb_t;

typedef void (*ble_sync_progress_cb_t)(const char *paso);

esp_err_t ble_sync_start(const char *device_name, const ble_sync_cb_t *cb);
esp_err_t ble_sync_start_debug(const char *device_name, const ble_sync_cb_t *cb,
                               ble_sync_progress_cb_t progreso);

/* true mientras un celular este conectado. */
bool ble_sync_connected(void);
