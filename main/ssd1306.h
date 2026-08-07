#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#define SSD1306_WIDTH     128
#define SSD1306_MAX_PAGES 8      /* soporta paneles de 32 px (0.91") y 64 px (0.96"/1.3") */

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int height;                  /* 32 o 64 */
    int pages;                   /* height / 8 */
    uint8_t fb[SSD1306_WIDTH * SSD1306_MAX_PAGES];
} ssd1306_t;

/* Crea el bus I2C y configura el panel. height debe ser 32 o 64. */
esp_err_t ssd1306_init(ssd1306_t *d, int sda_gpio, int scl_gpio, uint8_t addr, uint32_t hz, int height);

/* Framebuffer: nada se ve hasta llamar ssd1306_flush(). */
void ssd1306_clear(ssd1306_t *d);
esp_err_t ssd1306_flush(ssd1306_t *d);

void ssd1306_pixel(ssd1306_t *d, int x, int y, bool on);
void ssd1306_fill_rect(ssd1306_t *d, int x, int y, int w, int h, bool on);
void ssd1306_rect(ssd1306_t *d, int x, int y, int w, int h, bool on);
void ssd1306_line(ssd1306_t *d, int x0, int y0, int x1, int y1, bool on);
void ssd1306_circle(ssd1306_t *d, int cx, int cy, int r, bool on);
void ssd1306_disc(ssd1306_t *d, int cx, int cy, int r, bool on);

/* Fuente 5x7 escalada por 'scale' (1 = 5x7 px, 2 = 10x14 px). Devuelve el ancho dibujado. */
int ssd1306_text(ssd1306_t *d, int x, int y, const char *s, int scale, bool on);
int ssd1306_text_width(const char *s, int scale);
