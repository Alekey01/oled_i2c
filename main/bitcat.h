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

/* Accesorios: reaccionan al clima y a la hora. Se dibujan encima de la pose, y
   estan pensados para caber dentro de la caja del sprite (mas unos pocos pixeles
   a los lados en el caso del frio), porque en la vista del gato los 8 px de
   arriba los ocupan la hora y la temperatura. */
typedef enum {
    BITCAT_ACC_NINGUNO = 0,
    BITCAT_ACC_PARAGUAS,   /* lluvia: sombrilla sobre la patita levantada */
    BITCAT_ACC_LENTES,     /* sol: lentes oscuros sobre los ojos */
    BITCAT_ACC_FRIO,       /* bajo cero: rayitas de tiritar a los costados */
    BITCAT_ACC_ZZZ,        /* madrugada: zetas subiendo */
    BITCAT_ACC_COUNT,
} bitcat_acc_t;

/* Dibuja a BitCat con la esquina superior izquierda en (x, y). */
void bitcat_draw(ssd1306_t *d, int x, int y, bitcat_pose_t pose, bitcat_expr_t expr);

/* Igual, con accesorio encima. bitcat_draw() equivale a pasar BITCAT_ACC_NINGUNO. */
void bitcat_draw_acc(ssd1306_t *d, int x, int y, bitcat_pose_t pose,
                     bitcat_expr_t expr, bitcat_acc_t acc);

/* Nombres cortos, para el log. */
const char *bitcat_expr_nombre(bitcat_expr_t expr);
const char *bitcat_acc_nombre(bitcat_acc_t acc);
