#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Actualizacion de firmware por BLE.
 *
 * Este modulo solo maneja la escritura en flash y el cambio de particion; el
 * transporte lo pone ble_sync.c. La imagen entra en la ranura que no se esta
 * ejecutando, asi que un corte a media transferencia no deja al reloj sin
 * firmware: simplemente no se cambia el arranque.
 */

typedef struct {
    bool activo;
    uint32_t total;       /* bytes que anuncio el celular */
    uint32_t recibido;
} ota_estado_t;

/* Reserva la ranura y prepara la escritura. tamano en bytes. */
esp_err_t ota_begin(uint32_t tamano);

/* Agrega un trozo. Falla si no hay una actualizacion en curso o si se pasa. */
esp_err_t ota_write(const uint8_t *datos, uint16_t len);

/*
 * Valida la imagen, cambia la particion de arranque y programa un reinicio a los
 * 800 ms, para que dé tiempo a que salga la ultima notificacion.
 */
esp_err_t ota_end(void);

/* Cancela lo que haya en curso. Es seguro llamarla sin nada en curso. */
void ota_abort(void);

void ota_estado(ota_estado_t *out);

/*
 * Cancela el rollback tras comprobar que la version nueva arranca bien. Si nadie
 * la llama, el bootloader vuelve a la anterior en el siguiente reset. Sin
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE no hace nada.
 */
void ota_marcar_valido(void);
