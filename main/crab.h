#pragma once

#include "ssd1306.h"

#define CRAB_W 31
#define CRAB_H 22

/* Cuadros disponibles. Los dos primeros son la caminata, los dos ultimos el
   saludo: cara feliz con ojos "<" ">" y el brazo derecho levantado. */
typedef enum {
    CRAB_WALK_A = 0,
    CRAB_WALK_B,
    CRAB_WAVE_A,
    CRAB_WAVE_B,
    CRAB_FRAMES,
} crab_frame_t;

/* Dibuja el cangrejo con la esquina superior izquierda en (x, y). */
void crab_draw(ssd1306_t *d, int x, int y, crab_frame_t frame);
