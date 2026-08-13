#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Animacion dibujada en el celular.
 *
 * Los cuadros llegan por BLE ya en el formato del framebuffer del SSD1306:
 * paginas de ocho pixeles verticales, bit 0 arriba. Empaquetar en el celular y
 * no aqui deja el dibujo listo para un memcpy, sin conversion en el reloj y sin
 * gastar el cuadruple de aire mandando un byte por pixel.
 *
 * Se guarda en NVS, asi que sobrevive a los reinicios y a las actualizaciones.
 */

#define ANIM_FRAMES_MAX 4
#define ANIM_PAGES      4                    /* 32 px de alto */
#define ANIM_FRAME_BYTES (128 * ANIM_PAGES)  /* 512 bytes por cuadro */

/* Empieza a recibir. Descarta lo que hubiera: el reloj no tiene RAM de sobra
   para guardar la anterior mientras llega la nueva, y reenviarla son segundos. */
esp_err_t anim_begin(uint8_t frames, uint8_t delay_x10ms);

/* Escribe en el buffer, con desplazamiento absoluto dentro de todos los cuadros
   seguidos. Los trozos pueden llegar en cualquier orden. */
esp_err_t anim_write(uint16_t offset, const uint8_t *datos, uint16_t len);

/* Da por buena la animacion y pide guardarla. */
esp_err_t anim_end(void);

/* La borra, tambien de la flash. */
void anim_borrar(void);

bool anim_disponible(void);
uint8_t anim_frames(void);
uint16_t anim_delay_ms(void);

/* Puntero a los ANIM_FRAME_BYTES de un cuadro, o NULL si no hay animacion. */
const uint8_t *anim_frame(uint8_t i);

/* Recupera lo que hubiera guardado. Se llama una vez al arrancar. */
void anim_cargar(void);

/*
 * Guarda en flash si hay algo pendiente. La escritura son dos kilobytes y
 * decenas de milisegundos, asi que la hace la tarea de pantalla y no el callback
 * de una escritura BLE, que tiene que devolver el control enseguida.
 */
void anim_guardar_si_toca(void);
