#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * MPU-6050 para despertar la pantalla al mover el reloj.
 *
 * Va en el mismo bus I2C que el OLED: el panel responde en 0x3C y el sensor en
 * 0x68, asi que solo hay que soldar VCC, GND y los dos hilos del bus, mas el pin
 * INT a un GPIO libre.
 *
 * El giroscopio se queda apagado. Es el que consume —el chip entero ronda los
 * 3.9 mA con todo encendido, que doblaria el gasto del reloj—, y para detectar
 * movimiento no hace falta. Con solo el acelerometro en modo ciclo, el sensor se
 * despierta unas pocas veces por segundo y baja a decenas de microamperios.
 *
 * La interrupcion se configura enclavada y activa a nivel bajo, no por flanco:
 * del light sleep solo se puede despertar por nivel.
 */

esp_err_t imu_init(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t umbral, uint8_t duracion_ms);

bool imu_disponible(void);

/* Renuncia al sensor aunque haya respondido. Se usa cuando su linea INT no da
   señales de vida: mejor quedarse sin despertar por movimiento que armar una
   interrupcion que no se va a poder atender. */
void imu_descartar(void);

/* Limpia la interrupcion enclavada. Devuelve true si venia de movimiento. */
bool imu_atender_int(void);

/* Cuantos movimientos se han detectado desde el arranque. Se publica por BLE
   para poder afinar los umbrales sin cable: con light sleep la consola USB no
   sirve, y "la pantalla no se enciende" no distingue entre que la interrupcion
   no llegue y que llegue pero no se atienda. */
uint32_t imu_eventos(void);

/* Aceleracion en mili-g. Cualquiera puede ser NULL. */
esp_err_t imu_leer(int *x_mg, int *y_mg, int *z_mg);

/*
 * Saca al sensor del modo ciclo mientras se mide un gesto, y lo devuelve luego.
 *
 * En modo ciclo refresca a 20 Hz, que no da muestras suficientes dentro de un
 * manotazo para saber hacia donde empezo. Despierto refresca a 1 kHz.
 */
esp_err_t imu_modo_rapido(bool rapido);

/*
 * Ultimo gesto medido: que eje se movio mas (0=X, 1=Y, 2=Z), cuanto recorrio en
 * mili-g, y hacia donde empezo (+1, -1, o 0 si no se pudo decidir). Lo mide
 * quien tiene el bus, y se publica por BLE para poder elegir el eje y el sentido
 * sin abrir el reloj ni conectarlo por cable.
 *
 * eje = -1 mientras no se haya medido ninguno. Los punteros pueden ser NULL.
 */
void imu_anotar_agite(int eje, int swing, int sentido);
void imu_ultimo_agite(int *eje, int *swing, int *sentido);
