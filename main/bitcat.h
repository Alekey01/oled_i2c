#pragma once

#include "ssd1306.h"

#define BITCAT_W 31
#define BITCAT_H 24

/* Poses del cuerpo. */
typedef enum {
    BITCAT_SIT = 0,   /* sentado, quieto */
    BITCAT_WALK_A,    /* caminata: patas adelantadas */
    BITCAT_WALK_B,
    BITCAT_WAVE_A,    /* saludo: manita arriba */
    BITCAT_WAVE_B,
    BITCAT_POSES,
} bitcat_pose_t;

/* Expresiones. Son ojos de 4x4 que se estampan dentro de la pantalla de la cara,
   asi que cualquier pose se puede combinar con cualquier expresion. */
typedef enum {
    BITCAT_NORMAL = 0,
    BITCAT_FELIZ,
    BITCAT_SORPRESA,
    BITCAT_DORMIDO,
    BITCAT_ENOJADO,
    BITCAT_AMOR,
    BITCAT_EXPR_COUNT,
} bitcat_expr_t;

/* Dibuja a BitCat con la esquina superior izquierda en (x, y). */
void bitcat_draw(ssd1306_t *d, int x, int y, bitcat_pose_t pose, bitcat_expr_t expr);

/* Nombre corto de la expresion, para el log. */
const char *bitcat_expr_nombre(bitcat_expr_t expr);
