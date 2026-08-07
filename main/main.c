/*
 * Reloj "8 bits" + clima para ESP32-S3 (Seeed Studio) con OLED SSD1306 0.91" (128x32).
 * La hora y el clima llegan por BLE desde el celular: no se usa WiFi.
 *
 *   SDA -> GPIO 5 (D4)   SCL -> GPIO 6 (D5)   addr 0x3C   boton BOOT -> GPIO 0
 *
 * Dos vistas que alternan con el boton:
 *   RELOJ  HH:MM en pixel-art, segundos en binario de 8 bits, fecha
 *   CLIMA  icono dibujado, temperatura, humedad, viento
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_sync.h"
#include "crab.h"
#include "ssd1306.h"
#include "weather.h"

static const char *TAG = "reloj8b";

typedef enum {
    VIEW_CLOCK = 0,
    VIEW_WEATHER,
    VIEW_CRAB,
    VIEW_COUNT,
} view_t;

/* Refresco rapido solo en la vista animada; las otras no lo necesitan. */
#define REFRESH_MS_STATIC 250
#define REFRESH_MS_CRAB   80
#define CRAB_STEP_PX      2

/* Saludo: dura 24 ciclos (~2 s) y el brazo sube y baja cada 3 (~240 ms).
   La probabilidad se evalua por ciclo, solo mientras el cangrejo se ve entero;
   con 2% sale un saludo en algo mas de la mitad de las pasadas. */
#define CRAB_WAVE_TICKS   24
#define CRAB_WAVE_FLAP    3
#define CRAB_WAVE_CHANCE  2

static ssd1306_t s_oled;
static SemaphoreHandle_t s_mux;        /* estado compartido */
static SemaphoreHandle_t s_draw_mux;   /* framebuffer y bus I2C */

/* Estado compartido, protegido por s_mux. */
static weather_t s_weather;
static int32_t s_tz_offset;         /* segundos respecto a UTC, los manda el celular */
static bool s_time_set;
static int64_t s_last_sync_us;

static volatile view_t s_view = VIEW_CLOCK;

/* Animacion del cangrejo; solo la toca la tarea de pantalla. */
static int s_crab_x = -CRAB_W;
static int s_crab_tick;
static int s_crab_wave_left;   /* ciclos que le quedan al saludo, 0 = caminando */

static const char *DIAS[7]   = {"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"};
static const char *MESES[12] = {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN",
                                "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"};

/* ------------------------------------------------ callbacks desde el BLE */

static void on_time(uint32_t epoch_utc, int32_t tz_offset_sec)
{
    struct timeval tv = {.tv_sec = (time_t)epoch_utc, .tv_usec = 0};
    settimeofday(&tv, NULL);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_tz_offset = tz_offset_sec;
    s_time_set = true;
    s_last_sync_us = esp_timer_get_time();
    xSemaphoreGive(s_mux);
}

static void on_weather(const weather_t *w)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_weather = *w;
    s_weather.valid = true;
    s_last_sync_us = esp_timer_get_time();
    xSemaphoreGive(s_mux);
}

/* --------------------------------------------------------------- dibujo */

#define BITS_BOX  6
#define BITS_STEP 7
#define BITS_W    (8 * BITS_STEP - (BITS_STEP - BITS_BOX))   /* 55 px */

/* Fila de 8 bits: MSB a la izquierda, caja llena = 1. */
static void draw_bits(int x, int y, uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        int bx = x + i * BITS_STEP;
        if (value & (0x80 >> i)) {
            ssd1306_fill_rect(&s_oled, bx, y, BITS_BOX, BITS_BOX, true);
        } else {
            ssd1306_rect(&s_oled, bx, y, BITS_BOX, BITS_BOX, true);
        }
    }
}

/*
 * Runa de Bluetooth de 5x9 en la esquina superior izquierda. Se borra un marco
 * de 1 px alrededor antes de dibujarla para que siga legible cuando pasa por
 * debajo algo dibujado, como el cangrejo.
 */
static void draw_bt_icon(int x, int y)
{
    static const char *BT[9] = {
        "..#..",
        "..##.",
        "#.#.#",
        ".###.",
        "..#..",
        ".###.",
        "#.#.#",
        "..##.",
        "..#..",
    };
    ssd1306_fill_rect(&s_oled, x - 1, y - 1, 7, 11, false);
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 5; c++) {
            if (BT[r][c] == '#') {
                ssd1306_pixel(&s_oled, x + c, y + r, true);
            }
        }
    }
}

/* El ancho de ssd1306_text incluye el espacio final; se descuenta al centrar. */
static void text_center(int y, const char *s, int scale)
{
    int w = ssd1306_text_width(s, scale) - scale;
    ssd1306_text(&s_oled, (SSD1306_WIDTH - w) / 2, y, s, scale, true);
}

/* Vista 1: hora, segundos en binario y fecha, las tres centradas. */
static void draw_clock(const struct tm *t, bool have_time)
{
    char buf[32];

    if (have_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    text_center(0, buf, 2);

    draw_bits((SSD1306_WIDTH - BITS_W) / 2, 16, have_time ? (uint8_t)t->tm_sec : 0);

    if (have_time) {
        snprintf(buf, sizeof(buf), "%s %02d %s",
                 DIAS[t->tm_wday % 7], t->tm_mday, MESES[t->tm_mon % 12]);
    } else {
        snprintf(buf, sizeof(buf), "%s", ble_sync_connected() ? "SINCRONIZANDO" : "ESPERA CELULAR");
    }
    text_center(24, buf, 1);
}

/* Vista 2: icono, temperatura, humedad y viento. */
static void draw_weather(const weather_t *w, const struct tm *t, bool have_time)
{
    char buf[32];

    if (!w->valid) {
        ssd1306_text(&s_oled, 6, 4, "SIN DATOS DE", 1, true);
        ssd1306_text(&s_oled, 6, 14, "CLIMA. SINCRONIZA", 1, true);
        ssd1306_text(&s_oled, 6, 24, "DESDE EL CELULAR", 1, true);
        return;
    }

    /* De noche el icono de "despejado" usa luna en lugar de sol. */
    bool night = have_time && (t->tm_hour < 6 || t->tm_hour >= 20);
    weather_draw_icon(&s_oled, 0, 1, w->wmo_code, night);

    snprintf(buf, sizeof(buf), "%d`C", (int)(w->temp_c + (w->temp_c >= 0 ? 0.5f : -0.5f)));
    ssd1306_text(&s_oled, 34, 0, buf, 2, true);

    if (w->humidity >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", w->humidity);
        ssd1306_text(&s_oled, 34, 17, buf, 1, true);
    }

    if (w->wind_kmh >= 0) {
        snprintf(buf, sizeof(buf), "%dKMH %s", w->wind_kmh, weather_wind_dir(w->wind_deg));
        ssd1306_text(&s_oled, 64, 17, buf, 1, true);
    }

    snprintf(buf, sizeof(buf), "%s", weather_wmo_desc(w->wmo_code));
    buf[15] = '\0';   /* 15 caracteres = 90 px, lo que queda a la derecha del icono */
    ssd1306_text(&s_oled, 34, 25, buf, 1, true);
}

/* Vista 3: el cangrejo cruza la pantalla de izquierda a derecha. */
static void draw_crab(void)
{
    /* El sprite ocupa y=3..24 y el suelo va justo debajo, para que las patas
       largas de cada cuadro lo toquen y las cortas queden levantadas. */
    for (int x = 0; x < SSD1306_WIDTH; x += 4) {
        ssd1306_fill_rect(&s_oled, x, 25, 2, 1, true);
    }

    crab_frame_t frame;
    if (s_crab_wave_left > 0) {
        frame = (s_crab_tick / CRAB_WAVE_FLAP) % 2 ? CRAB_WAVE_B : CRAB_WAVE_A;
    } else {
        frame = (s_crab_tick / 2) % 2 ? CRAB_WALK_B : CRAB_WALK_A;
    }
    crab_draw(&s_oled, s_crab_x, 3, frame);
}

/* Avanza la animacion un paso. Se llama solo desde la tarea de pantalla. */
static void crab_step(void)
{
    s_crab_tick++;

    if (s_crab_wave_left > 0) {
        s_crab_wave_left--;   /* saludando: se queda quieto */
        return;
    }

    s_crab_x += CRAB_STEP_PX;
    if (s_crab_x > SSD1306_WIDTH) {
        s_crab_x = -CRAB_W;
        return;
    }

    /* Solo se detiene a saludar cuando se ve completo, para que no salude a
       medias fuera del borde. */
    bool fully_visible = s_crab_x >= 0 && s_crab_x + CRAB_W <= SSD1306_WIDTH;
    if (fully_visible && (esp_random() % 100) < CRAB_WAVE_CHANCE) {
        s_crab_wave_left = CRAB_WAVE_TICKS;
    }
}

/* Puede llamarse desde la tarea de pantalla o desde la del boton. */
static void render(void)
{
    xSemaphoreTake(s_draw_mux, portMAX_DELAY);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    weather_t w = s_weather;
    int32_t tz = s_tz_offset;
    bool have_time = s_time_set;
    int64_t last_sync = s_last_sync_us;
    xSemaphoreGive(s_mux);

    /* La hora del sistema se guarda en UTC; el offset del celular la vuelve local. */
    struct tm tm_now = {0};
    if (have_time) {
        time_t local = time(NULL) + tz;
        gmtime_r(&local, &tm_now);
    }

    ssd1306_clear(&s_oled);

    switch (s_view) {
    case VIEW_WEATHER:
        draw_weather(&w, &tm_now, have_time);
        break;
    case VIEW_CRAB:
        draw_crab();
        break;
    default:
        draw_clock(&tm_now, have_time);
        break;
    }

    /* La runa de Bluetooth va solo en la vista del reloj: es la unica con la
       esquina libre. Si no hay conexion y los datos ya llevan mas de una hora
       sin refrescarse, parpadea un cuadro hueco abajo a la derecha. */
    bool stale = have_time && (esp_timer_get_time() - last_sync) > 3600LL * 1000000LL;
    if (ble_sync_connected()) {
        if (s_view == VIEW_CLOCK) {
            draw_bt_icon(1, 1);
        }
    } else if (stale && (tm_now.tm_sec % 2) == 0) {
        ssd1306_rect(&s_oled, 124, 28, 4, 4, true);
    }

    ssd1306_flush(&s_oled);

    xSemaphoreGive(s_draw_mux);
}

/* --------------------------------------------------------------- tareas */

static void display_task(void *arg)
{
    view_t last = s_view;

    while (1) {
        /* Al entrar a la vista del cangrejo se reinicia el recorrido, para que
           siempre empiece caminando desde fuera del borde izquierdo. Todo el
           estado de la animacion vive en esta tarea: nadie mas lo toca. */
        if (s_view != last) {
            last = s_view;
            if (last == VIEW_CRAB) {
                s_crab_x = -CRAB_W;
                s_crab_tick = 0;
                s_crab_wave_left = 0;
            }
        }

        if (s_view == VIEW_CRAB) {
            crab_step();
            render();
            vTaskDelay(pdMS_TO_TICKS(REFRESH_MS_CRAB));
        } else {
            render();
            vTaskDelay(pdMS_TO_TICKS(REFRESH_MS_STATIC));
        }
    }
}

/* Sondeo con antirrebote: el boton BOOT esta a GND con pull-up interno. */
static void button_task(void *arg)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_APP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    bool prev = true;   /* sin presionar = alto */
    while (1) {
        bool level = gpio_get_level(CONFIG_APP_BUTTON_GPIO) != 0;
        if (prev && !level) {                    /* flanco de bajada */
            vTaskDelay(pdMS_TO_TICKS(30));       /* antirrebote */
            if (gpio_get_level(CONFIG_APP_BUTTON_GPIO) == 0) {
                s_view = (s_view + 1) % VIEW_COUNT;
                ESP_LOGI(TAG, "vista %d", s_view);
                render();                        /* respuesta inmediata al toque */
                while (gpio_get_level(CONFIG_APP_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));   /* espera a que lo suelten */
                }
            }
        }
        prev = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_mux = xSemaphoreCreateMutex();
    s_draw_mux = xSemaphoreCreateMutex();
    s_weather.wind_kmh = -1;
    s_weather.wind_deg = -1;

    ESP_ERROR_CHECK(ssd1306_init(&s_oled,
                                 CONFIG_APP_I2C_SDA_GPIO,
                                 CONFIG_APP_I2C_SCL_GPIO,
                                 CONFIG_APP_OLED_ADDR,
                                 400000,
                                 CONFIG_APP_OLED_HEIGHT));
    ssd1306_text(&s_oled, 0, 8, "INICIANDO BLE...", 1, true);
    ssd1306_flush(&s_oled);

    const ble_sync_cb_t cb = {.on_time = on_time, .on_weather = on_weather};
    ESP_ERROR_CHECK(ble_sync_start(CONFIG_APP_BLE_NAME, &cb));
    ESP_LOGI(TAG, "listo, esperando al celular");

    xTaskCreate(display_task, "display", 4096, NULL, 4, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
}
