#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "font5x7.h"
#include "ssd1306.h"

static const char *TAG = "ssd1306";

#define CTRL_CMD  0x00
#define CTRL_DATA 0x40

static esp_err_t cmd(ssd1306_t *d, uint8_t c)
{
    uint8_t buf[2] = {CTRL_CMD, c};
    return i2c_master_transmit(d->dev, buf, sizeof(buf), 100);
}

esp_err_t ssd1306_init(ssd1306_t *d, int sda_gpio, int scl_gpio, uint8_t addr, uint32_t hz, int height)
{
    ESP_RETURN_ON_FALSE(height == 32 || height == 64, ESP_ERR_INVALID_ARG, TAG, "alto no soportado: %d", height);

    memset(d, 0, sizeof(*d));
    d->height = height;
    d->pages = height / 8;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &d->bus), TAG, "no se pudo crear el bus I2C");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(d->bus, &dev_cfg, &d->dev), TAG, "no se pudo agregar el dispositivo");

    if (i2c_master_probe(d->bus, addr, 200) != ESP_OK) {
        ESP_LOGW(TAG, "no hay ACK en 0x%02X (SDA=%d SCL=%d): revisa cableado/direccion", addr, sda_gpio, scl_gpio);
    }

    /* En 128x32 cambian el multiplex y la configuracion de pines COM. */
    const uint8_t mux = (uint8_t)(height - 1);
    const uint8_t com_pins = (height == 32) ? 0x02 : 0x12;

    const uint8_t seq[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock div */
        0xA8, mux,        /* multiplex */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line = 0 */
        0x8D, 0x14,       /* charge pump ON */
        0x20, 0x00,       /* memory mode = horizontal */
        0xA1,             /* segment remap */
        0xC8,             /* COM scan descendente */
        0xDA, com_pins,   /* COM pins */
        0x81, 0xCF,       /* contraste */
        0xD9, 0xF1,       /* precarga */
        0xDB, 0x40,       /* VCOMH */
        0xA4,             /* mostrar RAM */
        0xA6,             /* no invertido */
        0x2E,             /* sin scroll */
        0xAF,             /* display on */
    };
    for (size_t i = 0; i < sizeof(seq); i++) {
        ESP_RETURN_ON_ERROR(cmd(d, seq[i]), TAG, "fallo comando 0x%02X", seq[i]);
    }

    ssd1306_clear(d);
    return ssd1306_flush(d);
}

void ssd1306_clear(ssd1306_t *d)
{
    memset(d->fb, 0, sizeof(d->fb));
}

esp_err_t ssd1306_flush(ssd1306_t *d)
{
    ESP_RETURN_ON_ERROR(cmd(d, 0x21), TAG, "set col");       /* rango de columnas */
    ESP_RETURN_ON_ERROR(cmd(d, 0), TAG, "col ini");
    ESP_RETURN_ON_ERROR(cmd(d, SSD1306_WIDTH - 1), TAG, "col fin");
    ESP_RETURN_ON_ERROR(cmd(d, 0x22), TAG, "set page");      /* rango de paginas */
    ESP_RETURN_ON_ERROR(cmd(d, 0), TAG, "page ini");
    ESP_RETURN_ON_ERROR(cmd(d, d->pages - 1), TAG, "page fin");

    /* Se manda pagina por pagina para no depender de un buffer I2C grande. */
    uint8_t chunk[1 + SSD1306_WIDTH];
    chunk[0] = CTRL_DATA;
    for (int p = 0; p < d->pages; p++) {
        memcpy(&chunk[1], &d->fb[p * SSD1306_WIDTH], SSD1306_WIDTH);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(d->dev, chunk, sizeof(chunk), 200), TAG, "fallo envio de pagina %d", p);
    }
    return ESP_OK;
}

void ssd1306_pixel(ssd1306_t *d, int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= d->height) {
        return;
    }
    uint8_t *b = &d->fb[(y / 8) * SSD1306_WIDTH + x];
    uint8_t mask = 1 << (y % 8);
    if (on) {
        *b |= mask;
    } else {
        *b &= ~mask;
    }
}

void ssd1306_fill_rect(ssd1306_t *d, int x, int y, int w, int h, bool on)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            ssd1306_pixel(d, x + i, y + j, on);
        }
    }
}

void ssd1306_rect(ssd1306_t *d, int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int i = 0; i < w; i++) {
        ssd1306_pixel(d, x + i, y, on);
        ssd1306_pixel(d, x + i, y + h - 1, on);
    }
    for (int j = 0; j < h; j++) {
        ssd1306_pixel(d, x, y + j, on);
        ssd1306_pixel(d, x + w - 1, y + j, on);
    }
}

void ssd1306_line(ssd1306_t *d, int x0, int y0, int x1, int y1, bool on)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        ssd1306_pixel(d, x0, y0, on);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ssd1306_circle(ssd1306_t *d, int cx, int cy, int r, bool on)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        ssd1306_pixel(d, cx + x, cy + y, on);
        ssd1306_pixel(d, cx + y, cy + x, on);
        ssd1306_pixel(d, cx - y, cy + x, on);
        ssd1306_pixel(d, cx - x, cy + y, on);
        ssd1306_pixel(d, cx - x, cy - y, on);
        ssd1306_pixel(d, cx - y, cy - x, on);
        ssd1306_pixel(d, cx + y, cy - x, on);
        ssd1306_pixel(d, cx + x, cy - y, on);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void ssd1306_disc(ssd1306_t *d, int cx, int cy, int r, bool on)
{
    /* El umbral r*r + r evita los pixeles sueltos de un pixel en los extremos. */
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r + r) {
                ssd1306_pixel(d, cx + x, cy + y, on);
            }
        }
    }
}

static char normalize(char c)
{
    if (c >= 'a' && c <= 'z') {
        c -= 32;
    }
    if (c == '\xB0') {          /* '°' en latin-1 */
        c = '`';
    }
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) {
        c = '?';
    }
    return c;
}

static void draw_char(ssd1306_t *d, int x, int y, char c, int scale, bool on)
{
    const uint8_t *g = font5x7[(uint8_t)normalize(c) - FONT_FIRST_CHAR];
    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            if (g[col] & (1 << row)) {
                ssd1306_fill_rect(d, x + col * scale, y + row * scale, scale, scale, on);
            }
        }
    }
}

int ssd1306_text(ssd1306_t *d, int x, int y, const char *s, int scale, bool on)
{
    int cx = x;
    for (; *s; s++) {
        draw_char(d, cx, y, *s, scale, on);
        cx += (FONT_W + 1) * scale;
    }
    return cx - x;
}

int ssd1306_text_width(const char *s, int scale)
{
    return (int)strlen(s) * (FONT_W + 1) * scale;
}
