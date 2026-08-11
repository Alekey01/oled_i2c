#pragma once

#include <stdbool.h>
#include "ssd1306.h"

typedef struct {
    bool valid;
    float temp_c;
    int humidity;      /* % */
    int wmo_code;
    int wind_kmh;      /* -1 si el celular no lo mando */
    int wind_deg;      /* grados de donde viene el viento, -1 si no hay dato */
} weather_t;

/* Descripcion corta (mayusculas, <= 13 caracteres) del codigo WMO. */
const char *weather_wmo_desc(int code);

/* true si del cielo esta cayendo algo: llovizna, lluvia, nieve o tormenta. */
bool weather_hay_precipitacion(int code);

/* true si el cielo esta despejado o casi. */
bool weather_es_despejado(int code);

/* Icono de 30x30 px con la esquina superior izquierda en (x, y). */
void weather_draw_icon(ssd1306_t *d, int x, int y, int wmo_code, bool night);

/* Grados a punto cardinal ("NE", "SSO", ...). */
const char *weather_wind_dir(int degrees);
