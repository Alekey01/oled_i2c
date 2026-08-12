#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Indicador de bateria.
 *
 * La XIAO ESP32-S3 carga el LiPo pero no trae forma de medirlo: la wiki de Seeed
 * lo dice explicitamente, todos los GPIO estan asignados a otra funcion y
 * ninguno llega al pin de bateria. Hay que ponerle un divisor:
 *
 *     B+ ──[100k]──┬──[100k]── GND
 *                  │
 *                 A0 (GPIO 1)
 *
 * Dos resistencias iguales dejan los 4.2 V del LiPo en 2.1 V, dentro del rango
 * del ADC. Se eligen de 100k y no de 1M porque el ADC del ESP32 quiere ver menos
 * de ~100k de impedancia de fuente o la lectura se va; con dos de 100k quedan
 * 50k. El divisor consume 21 uA, medio mAh al dia: irrelevante frente al reloj.
 *
 * Sin APP_BATTERY_GPIO configurado todo esto se compila fuera.
 */

esp_err_t battery_init(void);

/* false si no hay indicador configurado o si el ADC no arranco. */
bool battery_disponible(void);

/* Toma una lectura nueva. Es lenta (promedia varias muestras): llamarla cada
   tantos segundos, nunca por cuadro. */
void battery_actualizar(void);

/* Ultima lectura conocida. mv y pct pueden ser NULL. */
bool battery_estado(int *mv, int *pct);
