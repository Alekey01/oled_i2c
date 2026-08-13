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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "anim.h"
#include "battery.h"
#include "bitcat.h"
#include "ble_sync.h"
#include "imu.h"
#include "ota.h"
#include "ssd1306.h"
#include "weather.h"

static const char *TAG = "reloj8b";

typedef enum {
    VIEW_CLOCK = 0,
    VIEW_WEATHER,
    VIEW_FORECAST, /* pronostico de las proximas horas */
    VIEW_CAT,      /* BitCat en medio, hora y clima chicos arriba */
    VIEW_WALK,     /* BitCat cruzando la pantalla */
    VIEW_ANIM,     /* animacion dibujada en el celular; se salta si no hay */
    VIEW_COUNT,    /* fin del ciclo del boton corto */
    VIEW_GAME,     /* fuera del ciclo: se entra con pulsacion larga desde WALK */
} view_t;

/*
 * Avanza o retrocede en el ciclo, saltandose las vistas que ahora mismo no
 * tienen nada que enseñar. Sin esto, un reloj recien estrenado tendria en medio
 * del ciclo una pantalla que solo dice que no hay animacion.
 *
 * El limite de vueltas es por si algun dia se apagan todas: sin el, el bucle no
 * terminaria nunca.
 */
static view_t vista_vecina(view_t v, int paso)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        v = (view_t)((v + VIEW_COUNT + paso) % VIEW_COUNT);
        if (v != VIEW_ANIM || anim_disponible()) {
            return v;
        }
    }
    return VIEW_CLOCK;
}

/* Refresco rapido solo en las vistas animadas; las otras van a 1 Hz. */
#define REFRESH_MS_WALK   80
#define REFRESH_MS_GAME   60
#define WALK_STEP_PX      2

/* A partir de aqui una pulsacion cuenta como larga. */
#define LONG_PRESS_US (700 * 1000)

/* Saludo: dura 24 ciclos (~2 s) y la manita sube y baja cada 3 (~240 ms).
   La probabilidad se evalua por ciclo, solo mientras BitCat se ve entero;
   con 2% sale un saludo en algo mas de la mitad de las pasadas. */
#define WAVE_TICKS   24
#define WAVE_FLAP    3
#define WAVE_CHANCE  2

/* Cada cuanto cambia de humor la vista del gato. */
#define EXPR_PERIODO_US (15LL * 60 * 1000000)

static ssd1306_t s_oled;
static SemaphoreHandle_t s_mux;        /* estado compartido */
static SemaphoreHandle_t s_draw_mux;   /* framebuffer y bus I2C */

/* Estado compartido, protegido por s_mux. */
static weather_t s_weather;
static forecast_t s_fc;             /* pronostico; horas == 0 si no ha llegado */
static int32_t s_tz_offset;         /* segundos respecto a UTC, los manda el celular */
static bool s_time_set;
static int64_t s_last_sync_us;

#define NVS_NS   "bitcat"

static volatile view_t s_view = VIEW_CLOCK;

static TaskHandle_t s_display_h;
static TaskHandle_t s_button_h;

/* Solo las escribe la tarea de pantalla (s_screen_on) o la del boton
   (s_last_activity_us); la otra unicamente las lee. */
static volatile bool s_screen_on = true;
static volatile int64_t s_last_activity_us;

/* Apagado a mano con pulsacion larga. Cualquier toque posterior lo revierte. */
static volatile bool s_screen_req_off;

/* Animacion del paseo y humor del gato; solo los toca la tarea de pantalla. */
static int s_walk_x = -BITCAT_W;
static int s_walk_tick;
static int s_wave_left;        /* ciclos que le quedan al saludo, 0 = caminando */
static bitcat_expr_t s_expr = BITCAT_NORMAL;
static int64_t s_expr_us;

static const char *DIAS[7]   = {"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"};
static const char *MESES[12] = {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN",
                                "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"};

/* ------------------------------------------------- limpieza de la flash */

/*
 * La vista de las ultimas 24 h ya no existe, pero su blob sigue ocupando sitio
 * en los relojes que vienen de una version anterior. Se borra una vez: la
 * particion NVS son 20 KB y ahi tambien viven las animaciones, que son 2 KB
 * cada una, asi que no sobra espacio como para dejar basura.
 */
static void borrar_historial_viejo(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(h, "hist24") == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "borrado el historial de 24 h de una version anterior");
    }
    nvs_close(h);
}

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

static void on_forecast(const forecast_t *f)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_fc = *f;
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

/*
 * Pila de 12x6 con ocho columnas de relleno. Ocho y no diez para que cada
 * columna valga 12.5%: mas resolucion seria mentir, porque la lectura del ADC no
 * distingue mejor que eso en la zona plana de la curva del LiPo.
 */
static void draw_bat(int x, int y, int pct)
{
    ssd1306_rect(&s_oled, x, y, 10, 6, true);              /* carcasa */
    ssd1306_fill_rect(&s_oled, x + 10, y + 2, 2, 2, true); /* borne */

    int llenas = (pct * 8 + 50) / 100;
    if (llenas > 8) llenas = 8;
    if (llenas > 0) {
        ssd1306_fill_rect(&s_oled, x + 1, y + 1, llenas, 4, true);
    }
}

/* El ancho de ssd1306_text incluye el espacio final; se descuenta al centrar. */
static void text_center(int y, const char *s, int scale)
{
    int w = ssd1306_text_width(s, scale) - scale;
    ssd1306_text(&s_oled, (SSD1306_WIDTH - w) / 2, y, s, scale, true);
}

static void boot_ble_status(const char *paso)
{
    char linea[24];
    snprintf(linea, sizeof(linea), "BLE: %s", paso);
    ssd1306_fill_rect(&s_oled, 0, 24, SSD1306_WIDTH, 8, false);
    ssd1306_text(&s_oled, 0, 25, linea, 1, true);
    ssd1306_flush(&s_oled);
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

/*
 * Fuente de 3x5 solo para la version. La de 5x7 ya esta en su tamaño minimo, y
 * ahi el numero competiria con la hora y la temperatura; a 3x5 se lee cuando lo
 * buscas y desaparece cuando no. Solo cubre lo que hace falta: digitos, punto,
 * uve y espacio.
 */
#define MINI_W 3
#define MINI_H 5

static const char *mini_glifo(char c)
{
    static const char DIGITOS[10][MINI_H][MINI_W + 1] = {
        {"###", "#.#", "#.#", "#.#", "###"},   /* 0 */
        {".#.", "##.", ".#.", ".#.", "###"},   /* 1 */
        {"###", "..#", "###", "#..", "###"},   /* 2 */
        {"###", "..#", "###", "..#", "###"},   /* 3 */
        {"#.#", "#.#", "###", "..#", "..#"},   /* 4 */
        {"###", "#..", "###", "..#", "###"},   /* 5 */
        {"###", "#..", "###", "#.#", "###"},   /* 6 */
        {"###", "..#", "..#", "..#", "..#"},   /* 7 */
        {"###", "#.#", "###", "#.#", "###"},   /* 8 */
        {"###", "#.#", "###", "..#", "###"},   /* 9 */
    };
    static const char PUNTO[MINI_H][MINI_W + 1] = {"...", "...", "...", "...", ".#."};
    static const char UVE[MINI_H][MINI_W + 1]   = {"#.#", "#.#", "#.#", "#.#", ".#."};
    static const char NADA[MINI_H][MINI_W + 1]  = {"...", "...", "...", "...", "..."};

    if (c >= '0' && c <= '9') return DIGITOS[c - '0'][0];
    if (c == '.')             return PUNTO[0];
    if (c == 'V' || c == 'v') return UVE[0];
    return NADA[0];
}

/* Ancho que ocupara la cadena. Se necesita antes de dibujar para centrarla, que
   es justo lo que draw_mini() no puede dar porque lo devuelve al terminar. */
static int draw_mini_ancho(const char *s)
{
    size_t n = strlen(s);
    return n > 0 ? (int)n * (MINI_W + 1) - 1 : 0;
}

/* Devuelve el ancho dibujado. Cada glifo ocupa 3 px mas 1 de separacion. */
static int draw_mini(int x, int y, const char *s)
{
    int ancho = 0;
    for (const char *p = s; *p != '\0'; p++) {
        const char *g = mini_glifo(*p);
        for (int r = 0; r < MINI_H; r++) {
            for (int c = 0; c < MINI_W; c++) {
                /* Las filas son cadenas contiguas de MINI_W+1 con su terminador. */
                if (g[r * (MINI_W + 1) + c] == '#') {
                    ssd1306_pixel(&s_oled, x + ancho + c, y + r, true);
                }
            }
        }
        ancho += MINI_W + 1;
    }
    return ancho > 0 ? ancho - 1 : 0;
}

/*
 * Vista 3: las proximas seis horas, cada una con su hora, su icono y su
 * temperatura.
 *
 * Seis columnas de 21 px. Con doce cabrian los numeros pero no el icono, y sin
 * icono hay que leerse la grafica entera para saber si va a llover, que es
 * justo lo que se mira de un vistazo.
 *
 * La hora va en la fuente de 3x5 y la temperatura en la de 5x7: las dos no caben
 * grandes, y de las dos la que se consulta es la temperatura. La hora solo hace
 * falta para situar la columna, y para eso basta con distinguirla.
 */
#define FC_COLS   6
#define FC_ANCHO  21
#define FC_X0     ((SSD1306_WIDTH - FC_COLS * FC_ANCHO) / 2)
#define FC_Y_HORA 0
#define FC_Y_ICON 6
#define FC_Y_TEMP 21

static void draw_forecast(const forecast_t *f, bool have_time)
{
    char buf[8];

    if (f->horas == 0) {
        text_center(4, "PRONOSTICO", 1);
        text_center(18, have_time ? "SINCRONIZA" : "SIN HORA AUN", 1);
        return;
    }

    for (int i = 0; i < f->horas && i < FC_COLS; i++) {
        int x = FC_X0 + i * FC_ANCHO;
        int hora = (f->hora0 + i) % 24;

        snprintf(buf, sizeof(buf), "%02d", hora);
        int w = draw_mini_ancho(buf);
        draw_mini(x + (FC_ANCHO - w) / 2, FC_Y_HORA, buf);

        /* La luna en vez del sol de noche, con la hora de esa columna y no con
           la actual: a las 18:00 las ultimas del pronostico ya son de noche. */
        bool noche = hora < 6 || hora >= 20;
        weather_draw_icon_mini(&s_oled, x + (FC_ANCHO - 13) / 2, FC_Y_ICON,
                               f->wmo[i], noche);

        snprintf(buf, sizeof(buf), "%d", f->temp[i]);
        /* ssd1306_text_width incluye el espacio final; se descuenta al centrar. */
        w = ssd1306_text_width(buf, 1) - 1;
        ssd1306_text(&s_oled, x + (FC_ANCHO - w) / 2, FC_Y_TEMP, buf, 1, true);
    }
}

/*
 * Vista 4: BitCat en medio, con la hora chica arriba a la izquierda y el clima
 * arriba a la derecha. El humor cambia solo cada 15 minutos.
 */
static void actualizar_humor(void)
{
    int64_t ahora = esp_timer_get_time();
    if (s_expr_us != 0 && ahora - s_expr_us < EXPR_PERIODO_US) {
        return;
    }
    /* Se descarta la repetida: si sale la misma, el cambio seria invisible. */
    bitcat_expr_t nueva;
    do {
        nueva = esp_random() % BITCAT_EXPR_COUNT;
    } while (nueva == s_expr);

    s_expr = nueva;
    s_expr_us = ahora;
    ESP_LOGI(TAG, "humor: %s", bitcat_expr_nombre(s_expr));
}

/*
 * Elige pose, expresion y accesorio. Las reglas del clima ganan sobre el humor
 * aleatorio: si esta lloviendo, BitCat saca la sombrilla aunque le tocara estar
 * enojado. Cuando no hay nada que reportar vuelve al humor de siempre, que es lo
 * que pasa la mayor parte del tiempo.
 */
static void humor_por_clima(const weather_t *w, const struct tm *t, bool have_time,
                            bitcat_pose_t *pose, bitcat_expr_t *expr, bitcat_acc_t *acc)
{
    *pose = BITCAT_SIT;
    *acc  = BITCAT_ACC_NINGUNO;

    /* De madrugada duerme pase lo que pase: nadie saca la sombrilla a las 3am. */
    if (have_time && (t->tm_hour >= 23 || t->tm_hour < 6)) {
        *expr = BITCAT_DORMIDO;
        *acc  = BITCAT_ACC_ZZZ;
        return;
    }

    if (w->valid) {
        if (w->temp_c <= 0.0f) {
            *expr = BITCAT_ENOJADO;
            *acc  = BITCAT_ACC_FRIO;
            return;
        }
        if (weather_hay_precipitacion(w->wmo_code)) {
            /* La sombrilla se apoya en la patita levantada de esta pose. */
            *pose = BITCAT_WAVE_A;
            *expr = BITCAT_NORMAL;
            *acc  = BITCAT_ACC_PARAGUAS;
            return;
        }
        if (weather_es_despejado(w->wmo_code) && w->temp_c >= 25.0f) {
            *expr = BITCAT_FELIZ;
            *acc  = BITCAT_ACC_LENTES;
            return;
        }
    }

    actualizar_humor();
    *expr = s_expr;
}

static void draw_cat(const weather_t *w, const struct tm *t, bool have_time)
{
    char buf[16];

    if (have_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    ssd1306_text(&s_oled, 0, 0, buf, 1, true);

    if (w->valid) {
        snprintf(buf, sizeof(buf), "%d`C", (int)(w->temp_c + (w->temp_c >= 0 ? 0.5f : -0.5f)));
    } else {
        snprintf(buf, sizeof(buf), "--`C");
    }
    ssd1306_text(&s_oled, SSD1306_WIDTH - ssd1306_text_width(buf, 1) + 1, 0, buf, 1, true);

    bitcat_pose_t pose;
    bitcat_expr_t expr;
    bitcat_acc_t acc;
    humor_por_clima(w, t, have_time, &pose, &expr, &acc);

    /* 24 px de alto a partir de y=8 llegan justo al borde inferior. Los
       accesorios estan dibujados para caber dentro de la caja del sprite, que es
       lo unico que queda libre debajo de la hora y la temperatura. */
    bitcat_draw_acc(&s_oled, (SSD1306_WIDTH - BITCAT_W) / 2, 8, pose, expr, acc);

    /* Version abajo a la izquierda, en el hueco que deja el gato. Va a este lado
       y no al derecho porque ahi parpadea el aviso de datos viejos. */
    const esp_app_desc_t *desc = esp_app_get_description();
    char ver[40];   /* version[] son 32 caracteres; buf de la hora se queda corto */
    snprintf(ver, sizeof(ver), "V %s", desc->version);
    draw_mini(0, 27, ver);
}

/* Vista 4: BitCat cruza la pantalla de izquierda a derecha. */
static void draw_walk(void)
{
    /* El sprite ocupa y=4..27 y el suelo va justo debajo de las patas. */
    for (int x = 0; x < SSD1306_WIDTH; x += 4) {
        ssd1306_fill_rect(&s_oled, x, 28, 2, 1, true);
    }

    bitcat_pose_t pose;
    if (s_wave_left > 0) {
        pose = (s_walk_tick / WAVE_FLAP) % 2 ? BITCAT_WAVE_B : BITCAT_WAVE_A;
    } else {
        pose = (s_walk_tick / 2) % 2 ? BITCAT_WALK_B : BITCAT_WALK_A;
    }
    /* Al saludar pone cara feliz; caminando va normal. */
    bitcat_draw(&s_oled, s_walk_x, 4, pose,
                s_wave_left > 0 ? BITCAT_FELIZ : BITCAT_NORMAL);
}

/* Avanza la animacion un paso. Se llama solo desde la tarea de pantalla. */
static void walk_step(void)
{
    s_walk_tick++;

    if (s_wave_left > 0) {
        s_wave_left--;   /* saludando: se queda quieto */
        return;
    }

    s_walk_x += WALK_STEP_PX;
    if (s_walk_x > SSD1306_WIDTH) {
        s_walk_x = -BITCAT_W;
        return;
    }

    /* Solo se detiene a saludar cuando se ve completo, para que no salude a
       medias fuera del borde. */
    bool completo = s_walk_x >= 0 && s_walk_x + BITCAT_W <= SSD1306_WIDTH;
    if (completo && (esp_random() % 100) < WAVE_CHANCE) {
        s_wave_left = WAVE_TICKS;
    }
}

/*
 * Vista 6: la animacion que hayas dibujado en el celular.
 *
 * Los cuadros ya vienen en el formato del framebuffer, asi que dibujar es
 * copiarlos encima. Se copia solo lo que quepa: la animacion esta hecha para 32
 * px de alto y en un panel de 64 ocuparia la mitad de arriba en vez de
 * desbordarse.
 */
static int s_anim_i;

static void draw_anim(void)
{
    const uint8_t *f = anim_frame((uint8_t)s_anim_i);
    if (f == NULL) {
        text_center(12, "SIN ANIMACION", 1);
        return;
    }
    int paginas = s_oled.pages < ANIM_PAGES ? s_oled.pages : ANIM_PAGES;
    memcpy(s_oled.fb, f, (size_t)paginas * SSD1306_WIDTH);
}

static void anim_step(void)
{
    uint8_t n = anim_frames();
    if (n > 0) {
        s_anim_i = (s_anim_i + 1) % n;
    }
}

/* ----------------------------------------------------------------- juego */

/*
 * Endless runner: BitCat corre en el sitio y el mundo pasa hacia la izquierda.
 * Pulsacion corta salta, larga sale. Toda la fisica es entera; a 60 ms por
 * cuadro el salto dura unos 900 ms, que con obstaculos de 6 a 10 px sobra.
 *
 * El estado lo mueve solo la tarea de pantalla. La del boton no escribe nada:
 * deja pedido el salto en una bandera y game_step() lo consume, que evita tener
 * que meter un mutex en el bucle del juego.
 */
#define GAME_SUELO    31                       /* fila de la linea de suelo */
#define GAME_CAT_X    6
#define GAME_CAT_Y    (GAME_SUELO - BITCAT_H)  /* patas justo encima del suelo */
#define GAME_VY0      5                        /* impulso: sube 15 px */
#define GAME_OBST     3
#define GAME_VEL_MIN  3
#define GAME_VEL_MAX  6
#define GAME_HUECO_MIN 46
#define GAME_HUECO_MAX 86

typedef struct {
    int x, w, h;
    bool activo;
    bool contado;   /* ya sumo punto al pasar al gato */
} obst_t;

static obst_t s_obst[GAME_OBST];
static int s_game_alto;      /* px por encima del suelo */
static int s_game_vy;
static int s_game_score;
static int s_game_best;
static int s_game_tick;
static bool s_game_over;
static bool s_game_arranco;
static volatile bool s_game_salto_pedido;

static int game_vel(void)
{
    int v = GAME_VEL_MIN + s_game_score / 10;
    return v > GAME_VEL_MAX ? GAME_VEL_MAX : v;
}

static void game_reset(void)
{
    memset(s_obst, 0, sizeof(s_obst));
    s_game_alto = 0;
    s_game_vy = 0;
    s_game_score = 0;
    s_game_tick = 0;
    s_game_over = false;
    s_game_arranco = false;
    s_game_salto_pedido = false;
}

/* La llama la tarea del boton. */
static void game_pedir_salto(void)
{
    s_game_salto_pedido = true;
}

/* Caja del gato: solo cuerpo y patas. La cabeza va muy arriba para chocar con
   algo apoyado en el suelo, y contarla haria el juego injustamente dificil. */
static bool game_choca(const obst_t *o, int cat_y)
{
    int cx0 = GAME_CAT_X + 9,  cx1 = GAME_CAT_X + 21;
    int cy0 = cat_y + 15,      cy1 = cat_y + 23;
    int ox0 = o->x,            ox1 = o->x + o->w - 1;
    int oy0 = GAME_SUELO - o->h, oy1 = GAME_SUELO - 1;

    return cx0 <= ox1 && cx1 >= ox0 && cy0 <= oy1 && cy1 >= oy0;
}

static void game_soltar_obstaculo(void)
{
    /* El hueco se mide desde el mas adelantado, no desde el borde: si no, dos
       obstaculos podrian salir pegados y no habria forma de saltarlos. */
    int derecha = SSD1306_WIDTH;
    for (int i = 0; i < GAME_OBST; i++) {
        if (s_obst[i].activo && s_obst[i].x + s_obst[i].w > derecha) {
            derecha = s_obst[i].x + s_obst[i].w;
        }
    }
    int hueco = GAME_HUECO_MIN + esp_random() % (GAME_HUECO_MAX - GAME_HUECO_MIN + 1);

    for (int i = 0; i < GAME_OBST; i++) {
        if (s_obst[i].activo) {
            continue;
        }
        s_obst[i].x = derecha + hueco;
        s_obst[i].w = 4 + esp_random() % 3;    /* 4..6 px */
        s_obst[i].h = 6 + esp_random() % 5;    /* 6..10 px */
        s_obst[i].activo = true;
        s_obst[i].contado = false;
        return;
    }
}

static void game_step(void)
{
    s_game_tick++;

    if (s_game_over) {
        /* En game over la pulsacion corta reinicia en vez de saltar. */
        if (s_game_salto_pedido) {
            s_game_salto_pedido = false;
            game_reset();
        }
        return;
    }

    if (s_game_salto_pedido) {
        s_game_salto_pedido = false;
        s_game_arranco = true;
        if (s_game_alto == 0) {
            s_game_vy = GAME_VY0;
        }
    }

    /* Hasta el primer salto el mundo no se mueve: da tiempo a leer la ayuda. */
    if (!s_game_arranco) {
        return;
    }

    s_game_alto += s_game_vy;
    s_game_vy--;
    if (s_game_alto <= 0) {
        s_game_alto = 0;
        s_game_vy = 0;
    }

    int vel = game_vel();
    int cat_y = GAME_CAT_Y - s_game_alto;
    int vivos = 0;

    for (int i = 0; i < GAME_OBST; i++) {
        obst_t *o = &s_obst[i];
        if (!o->activo) {
            continue;
        }
        o->x -= vel;
        if (o->x + o->w < 0) {
            o->activo = false;
            continue;
        }
        vivos++;
        if (!o->contado && o->x + o->w < GAME_CAT_X + 9) {
            o->contado = true;
            s_game_score++;
        }
        if (game_choca(o, cat_y)) {
            s_game_over = true;
            if (s_game_score > s_game_best) {
                s_game_best = s_game_score;
            }
            ESP_LOGI(TAG, "game over: %d puntos (record %d)", s_game_score, s_game_best);
            return;
        }
    }

    if (vivos < GAME_OBST) {
        game_soltar_obstaculo();
    }
}

static void draw_game(void)
{
    char buf[24];

    for (int x = 0; x < SSD1306_WIDTH; x += 4) {
        ssd1306_fill_rect(&s_oled, x, GAME_SUELO, 2, 1, true);
    }

    for (int i = 0; i < GAME_OBST; i++) {
        const obst_t *o = &s_obst[i];
        if (o->activo) {
            ssd1306_fill_rect(&s_oled, o->x, GAME_SUELO - o->h, o->w, o->h, true);
        }
    }

    if (s_game_over) {
        /* Se borra una banda para que el texto no compita con el gato. */
        ssd1306_fill_rect(&s_oled, 0, 4, SSD1306_WIDTH, 22, false);
        text_center(5, "GAME OVER", 1);
        snprintf(buf, sizeof(buf), "%d  RECORD %d", s_game_score, s_game_best);
        text_center(16, buf, 1);
        return;
    }

    /* En el aire va con las patas juntas; en el suelo alterna la carrera. */
    bitcat_pose_t pose = s_game_alto > 0 ? BITCAT_SIT
                       : ((s_game_tick / 2) % 2 ? BITCAT_WALK_B : BITCAT_WALK_A);
    bitcat_draw(&s_oled, GAME_CAT_X, GAME_CAT_Y - s_game_alto, pose,
                s_game_alto > 0 ? BITCAT_SORPRESA : BITCAT_NORMAL);

    if (!s_game_arranco) {
        text_center(2, "PULSA PARA SALTAR", 1);
        return;
    }

    snprintf(buf, sizeof(buf), "%d", s_game_score);
    ssd1306_text(&s_oled, SSD1306_WIDTH - ssd1306_text_width(buf, 1) + 1, 0, buf, 1, true);
}

/*
 * Mientras entra firmware nuevo la pantalla no muestra la vista: si el usuario
 * ve el reloj tan campante mientras el celular dice "actualizando", lo normal es
 * que desconecte creyendo que se colgo.
 */
static void draw_ota(const ota_estado_t *o)
{
    char buf[24];
    int pct = o->total ? (int)(((uint64_t)o->recibido * 100) / o->total) : 0;

    text_center(0, "ACTUALIZANDO", 1);

    ssd1306_rect(&s_oled, 4, 11, 120, 10, true);
    ssd1306_fill_rect(&s_oled, 6, 13, pct * 116 / 100, 6, true);

    snprintf(buf, sizeof(buf), "%d%%  %lu KB", pct, (unsigned long)(o->recibido / 1024));
    text_center(23, buf, 1);
}

/* Puede llamarse desde la tarea de pantalla o desde la del boton. */
static void render(void)
{
    xSemaphoreTake(s_draw_mux, portMAX_DELAY);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    weather_t w = s_weather;
    forecast_t fc = s_fc;
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

    ota_estado_t ota;
    ota_estado(&ota);

    if (ota.activo) {
        draw_ota(&ota);
    } else {
        switch (s_view) {
        case VIEW_WEATHER:
            draw_weather(&w, &tm_now, have_time);
            break;
        case VIEW_FORECAST:
            draw_forecast(&fc, have_time);
            break;
        case VIEW_CAT:
            draw_cat(&w, &tm_now, have_time);
            break;
        case VIEW_WALK:
            draw_walk();
            break;
        case VIEW_ANIM:
            draw_anim();
            break;
        case VIEW_GAME:
            draw_game();
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
        } else if (stale && s_view != VIEW_GAME && (tm_now.tm_sec % 2) == 0) {
            ssd1306_rect(&s_oled, 124, 28, 4, 4, true);
        }

        /* La pila va solo en la vista del reloj: es la unica con la esquina
           superior derecha libre, y es donde se mira de paso. */
        int pct;
        if (s_view == VIEW_CLOCK && battery_estado(NULL, &pct)) {
            draw_bat(SSD1306_WIDTH - 12, 0, pct);
        }
    }

    ssd1306_flush(&s_oled);

    xSemaphoreGive(s_draw_mux);
}

/* --------------------------------------------------------------- tareas */

/*
 * Milisegundos que faltan para el proximo segundo. Redibujar 4 veces por segundo
 * no aporta nada: lo unico que cambia a esa velocidad es el segundero binario,
 * y cambia una vez por segundo. Alinearse al borde ademas evita que el segundero
 * se vea saltar tarde.
 */
static uint32_t ms_al_proximo_segundo(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 1000 - (uint32_t)(tv.tv_usec / 1000) + 5;   /* 5 ms de margen */
}

static void display_task(void *arg)
{
    int64_t timeout_us = (int64_t)CONFIG_APP_SCREEN_TIMEOUT_S * 1000000;
    const int64_t sol_us = (int64_t)CONFIG_APP_OLED_SUN_S * 1000000;

#if CONFIG_APP_IMU_INT_GPIO >= 0
    /*
     * El apagado automatico solo se sostiene si algo vuelve a encender la
     * pantalla sola. Si el sensor no contesta —mal cableado, direccion I2C
     * distinta— dejarla apagarse cada 8 s seria peor que no apagarla: la unica
     * forma de ver la hora seria pulsar el boton cada vez.
     */
    if (!imu_disponible()) {
        timeout_us = 0;
    }
#endif
    view_t last = s_view;
    bool ota_confirmado = false;
    int contraste = -1;   /* -1 = todavia sin fijar */
    int64_t bateria_us = 0;

    s_last_activity_us = esp_timer_get_time();

    ESP_LOGI(TAG, "tarea de pantalla arrancada");

    while (1) {
        /* Al entrar a la vista del cangrejo se reinicia el recorrido, para que
           siempre empiece caminando desde fuera del borde izquierdo. Todo el
           estado de la animacion vive en esta tarea: nadie mas lo toca. */
        if (s_view != last) {
            last = s_view;
            if (last == VIEW_WALK) {
                s_walk_x = -BITCAT_W;
                s_walk_tick = 0;
                s_wave_left = 0;
            } else if (last == VIEW_ANIM) {
                s_anim_i = 0;   /* que siempre arranque por el primer cuadro */
            } else if (last == VIEW_GAME) {
                /* El reinicio vive aqui y no en la tarea del boton para que un
                   solo hilo sea dueño del estado del juego. */
                game_reset();
            }
        }

        /*
         * Confirmar el arranque solo despues de un rato en marcha: si algo del
         * firmware nuevo se cuelga en el arranque, nadie llega aqui y el
         * bootloader vuelve solo a la version anterior en el siguiente reset.
         */
        if (!ota_confirmado && esp_timer_get_time() > 30LL * 1000000) {
            ota_marcar_valido();
            ota_confirmado = true;
        }

        /* La lectura promedia 16 muestras, asi que no va por cuadro. Cada 30 s
           sobra: un LiPo no cambia de nivel en menos que eso. */
        if (battery_disponible() && esp_timer_get_time() - bateria_us > 30LL * 1000000) {
            bateria_us = esp_timer_get_time();
            battery_actualizar();
        }

        /* Dos kilobytes a la flash no pueden salir del
           callback de una escritura BLE. */
        anim_guardar_si_toca();

        ota_estado_t ota;
        ota_estado(&ota);

        /* Con una actualizacion en curso la pantalla no se apaga: es justo
           cuando el usuario quiere ver que va avanzando. */
        bool apagar = !ota.activo
                   && (s_screen_req_off
                       || (timeout_us > 0
                           && esp_timer_get_time() - s_last_activity_us > timeout_us));
        if (apagar) {
            if (s_screen_on) {
                xSemaphoreTake(s_draw_mux, portMAX_DELAY);
                ssd1306_power(&s_oled, false);
                xSemaphoreGive(s_draw_mux);
                s_screen_on = false;
                ESP_LOGI(TAG, "pantalla apagada (%s)",
                         s_screen_req_off ? "a mano" : "por inactividad");
            }
            /* Sin nada que dibujar, la tarea se bloquea de verdad: es lo que
               deja al chip entrar en light sleep. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /*
         * Brillo segun atencion. Si acabas de tocar el boton es que estas
         * mirando la pantalla —y probablemente de pie, a la calle—, asi que se
         * sube al maximo un rato. En reposo baja, que es donde se pasa el 99%
         * del tiempo y de donde sale el ahorro.
         *
         * No hace falta temporizador propio: s_last_activity_us ya lleva la
         * cuenta y esta tarea pasa por aqui una vez por segundo.
         */
        int deseado = (sol_us > 0 && esp_timer_get_time() - s_last_activity_us < sol_us)
                    ? CONFIG_APP_OLED_CONTRAST_SUN
                    : CONFIG_APP_OLED_CONTRAST;
        if (deseado != contraste) {
            xSemaphoreTake(s_draw_mux, portMAX_DELAY);
            ssd1306_contrast(&s_oled, (uint8_t)deseado);
            xSemaphoreGive(s_draw_mux);
            contraste = deseado;
        }

        /* Se dibuja el cuadro nuevo antes de encender, para no mostrar por un
           instante lo que quedo en la RAM del panel. */
        bool encendiendo = !s_screen_on;

        uint32_t espera_ms;
        if (ota.activo) {
            render();
            espera_ms = 250;   /* la barra tiene que verse avanzar */
        } else if (s_view == VIEW_GAME) {
            game_step();
            render();
            espera_ms = REFRESH_MS_GAME;
        } else if (s_view == VIEW_WALK) {
            walk_step();
            render();
            espera_ms = REFRESH_MS_WALK;
        } else if (s_view == VIEW_ANIM) {
            anim_step();
            render();
            espera_ms = anim_delay_ms();
        } else {
            render();
            espera_ms = ms_al_proximo_segundo();
        }

        if (encendiendo) {
            xSemaphoreTake(s_draw_mux, portMAX_DELAY);
            ssd1306_power(&s_oled, true);
            xSemaphoreGive(s_draw_mux);
            s_screen_on = true;
        }

        /* La espera es interrumpible: el boton notifica y se redibuja al toque. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(espera_ms));
    }
}

/*
 * Un boton, dos acciones. La corta es la de siempre; la larga depende de donde
 * estes, que es lo que evita tener que agregar hardware para el juego.
 *
 *   vista            corta            larga
 *   RELOJ/CLIMA/24H  siguiente vista  apagar la pantalla
 *   GATO             siguiente vista  apagar la pantalla
 *   PASEO            siguiente vista  entrar al juego
 *   JUEGO            saltar           salir al paseo
 */
static void boton_corto(void)
{
    if (s_view == VIEW_GAME) {
        game_pedir_salto();
        return;
    }
    s_view = vista_vecina(s_view, +1);
    ESP_LOGI(TAG, "vista %d", s_view);
}

static void boton_largo(void)
{
    if (s_view == VIEW_GAME) {
        s_view = VIEW_WALK;
        ESP_LOGI(TAG, "saliendo del juego");
        return;
    }
    if (s_view == VIEW_WALK) {
        s_view = VIEW_GAME;
        ESP_LOGI(TAG, "entrando al juego");
        return;
    }
    s_screen_req_off = true;
    ESP_LOGI(TAG, "apagando la pantalla a mano");
}

/* Mismo caso que la del sensor: la interrupcion es de nivel y el boton sigue
   abajo mientras lo tengas pulsado, asi que hay que callarla aqui dentro o la
   tarea que tiene que atenderla no llega a correr. */
static void IRAM_ATTR button_isr(void *arg)
{
    gpio_intr_disable(CONFIG_APP_BUTTON_GPIO);

    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_button_h, &hp);
    portYIELD_FROM_ISR(hp);
}

/*
 * El boton BOOT esta a GND con pull-up interno. Se atiende por interrupcion, no
 * por sondeo: un sondeo cada 20 ms despertaria al CPU 50 veces por segundo y
 * anularia el light sleep.
 */
static void button_task(void *arg)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_APP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    /* Igual que en la tarea del sensor: el handle se toma aqui, no del
       xTaskCreate, porque la ISR puede llegar antes de que aquel lo escriba.
       Aqui hace falta arrancar con el boton pulsado para provocarlo, pero el
       fallo seria el mismo. */
    s_button_h = xTaskGetCurrentTaskHandle();

    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_APP_BUTTON_GPIO, button_isr, NULL));

    /* Para que el boton tambien saque al chip del light sleep. Del light sleep
       solo se puede despertar por nivel, no por flanco. */
    ESP_ERROR_CHECK(gpio_wakeup_enable(CONFIG_APP_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* La interrupcion ya viene apagada desde la ISR; si se apagara aqui, la
           tarea no llegaria a correr para hacerlo. */
        vTaskDelay(pdMS_TO_TICKS(30));   /* antirrebote */

        if (gpio_get_level(CONFIG_APP_BUTTON_GPIO) == 0) {
            /* Con la pantalla apagada, el primer toque solo la enciende: seria
               molesto que ademas cambiara de vista sin que la hayas visto. */
            bool estaba_encendida = s_screen_on && !s_screen_req_off;
            s_screen_req_off = false;
            s_last_activity_us = esp_timer_get_time();

            /* Sondear cada 20 ms solo mientras el boton siga abajo no estorba al
               light sleep: el chip ya esta despierto porque lo estas tocando. La
               accion larga se dispara al cruzar el umbral, no al soltar, para
               que se vea que paso algo sin tener que adivinar cuanto falta. */
            int64_t t0 = esp_timer_get_time();
            bool largo = false;
            while (gpio_get_level(CONFIG_APP_BUTTON_GPIO) == 0) {
                if (esp_timer_get_time() - t0 >= LONG_PRESS_US) {
                    largo = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            if (estaba_encendida) {
                if (largo) {
                    boton_largo();
                } else {
                    boton_corto();
                }
            }
            xTaskNotifyGive(s_display_h);

            while (gpio_get_level(CONFIG_APP_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));   /* espera a que lo suelten */
            }
        }

        ulTaskNotifyTake(pdTRUE, 0);   /* descarta lo que hayan dejado los rebotes */
        gpio_intr_enable(CONFIG_APP_BUTTON_GPIO);
    }
}

#if CONFIG_APP_IMU_INT_GPIO >= 0

static TaskHandle_t s_imu_h;

/*
 * Deteccion del agite.
 *
 * La interrupcion del sensor solo dice "algo se movio". Para separar una
 * sacudida a proposito de andar con el reloj puesto se muestrea medio segundo
 * despues del aviso y se piden dos cosas a la vez:
 *
 *   recorrido  cuanto va y viene el eje. La gravedad es un valor fijo, asi que
 *              al restar el minimo del maximo se cancela sola y da igual como
 *              este orientado el reloj.
 *   idas y venidas  una sacudida cruza el centro varias veces. Levantar el
 *              brazo tiene recorrido de sobra pero cruza una sola vez, y es lo
 *              que separa el gesto del movimiento normal.
 *
 * En modo ciclo el sensor refresca a 20 Hz, o sea una muestra nueva cada 50 ms:
 * pedirlas mas seguidas solo devolveria la misma dos veces.
 */
#define AGITE_MUESTRAS 12
#define AGITE_PASO_MS  50
#define AGITE_CRUCES   3

static bool detectar_agite(void)
{
    int min[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
    int max[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    int muestras[3][AGITE_MUESTRAS];
    int n = 0;

    for (int i = 0; i < AGITE_MUESTRAS; i++) {
        int v[3];
        xSemaphoreTake(s_draw_mux, portMAX_DELAY);
        esp_err_t err = imu_leer(&v[0], &v[1], &v[2]);
        xSemaphoreGive(s_draw_mux);

        if (err == ESP_OK) {
            for (int e = 0; e < 3; e++) {
                muestras[e][n] = v[e];
                if (v[e] < min[e]) min[e] = v[e];
                if (v[e] > max[e]) max[e] = v[e];
            }
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(AGITE_PASO_MS));
    }

    if (n < AGITE_CRUCES + 1) {
        return false;   /* el bus fallo demasiadas veces para decidir nada */
    }

    /* Eje que mas se movio. */
    int eje = 0;
    for (int e = 1; e < 3; e++) {
        if (max[e] - min[e] > max[eje] - min[eje]) {
            eje = e;
        }
    }
    int swing = max[eje] - min[eje];

    /* Se anota siempre, incluso si no llega a contar como gesto: es lo que deja
       ver por BLE que eje mueve el reloj de verdad y con cuanta fuerza. */
    imu_anotar_agite(eje, swing);

    if (swing < CONFIG_APP_IMU_AGITE_MG) {
        return false;
    }
#if CONFIG_APP_IMU_EJE_AGITE >= 0
    if (eje != CONFIG_APP_IMU_EJE_AGITE) {
        return false;   /* se movio, pero no en el eje que cuenta */
    }
#endif

    /* Idas y venidas: cuantas veces cruza el punto medio de su propio recorrido. */
    int centro = (max[eje] + min[eje]) / 2;
    int cruces = 0;
    bool arriba = muestras[eje][0] > centro;
    for (int i = 1; i < n; i++) {
        bool ahora = muestras[eje][i] > centro;
        if (ahora != arriba) {
            cruces++;
            arriba = ahora;
        }
    }

    return cruces >= AGITE_CRUCES;
}

/* Agitando no se salta en el juego: ahi el salto es del boton, y un tropiezo
   no deberia costarte la partida. */
static void agite_cambiar_vista(void)
{
    if (s_view == VIEW_GAME) {
        return;
    }
    s_view = vista_vecina(s_view, +1);
    ESP_LOGI(TAG, "agite: vista %d", s_view);
}

/*
 * Callar la interrupcion aqui dentro, no en la tarea, es lo unico que rompe la
 * tormenta.
 *
 * La linea INT es de nivel y queda enclavada abajo hasta que alguien lee
 * INT_STATUS por I2C. Notificar a la tarea no baja esa linea, asi que al salir
 * de la ISR la condicion sigue puesta y el hardware vuelve a entrar de
 * inmediato. Una ISR gana a cualquier tarea, asi que la que tenia que leer
 * INT_STATUS no llegaba a ejecutarse nunca: el CPU se quedaba dando vueltas en
 * la interrupcion hasta que el perro guardian reiniciaba el reloj.
 *
 * Desactivarla aqui la deja disparar una sola vez por armado. La tarea la vuelve
 * a armar cuando ya ha soltado el enclavamiento.
 */
static void IRAM_ATTR imu_isr(void *arg)
{
    gpio_intr_disable(CONFIG_APP_IMU_INT_GPIO);

    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_imu_h, &hp);
    portYIELD_FROM_ISR(hp);
}

/*
 * El MPU-6050 avisa por su pin INT cuando detecta movimiento. Eso no cambia de
 * vista: solo cuenta como actividad, igual que tocar el boton, y por tanto
 * enciende la pantalla y reinicia la cuenta atras para apagarla.
 *
 * La interrupcion esta enclavada, asi que hay que leer INT_STATUS para soltarla.
 * Esa lectura va por el mismo bus I2C que el OLED, de ahi el mutex de dibujo.
 */
static void imu_task(void *arg)
{
    /*
     * El handle se toma aqui y no del xTaskCreate. Esta tarea corre con mas
     * prioridad que la que la crea, asi que empieza antes de que xTaskCreate
     * llegue a escribir su salida: si la interrupcion llegara en ese hueco, la
     * ISR notificaria a un handle todavia sin asignar y el sistema entra en
     * panico. Y llega, porque la linea INT del MPU es activa a nivel bajo y
     * queda enclavada en cuanto el reloj se mueve, o sea desde antes de arrancar.
     */
    s_imu_h = xTaskGetCurrentTaskHandle();

    /* Soltar el enclavamiento ANTES de habilitar la interrupcion: si el pin ya
       esta abajo, en cuanto se arma empieza a dispararse sin parar. */
    xSemaphoreTake(s_draw_mux, portMAX_DELAY);
    imu_atender_int();
    xSemaphoreGive(s_draw_mux);

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_APP_IMU_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_APP_IMU_INT_GPIO, imu_isr, NULL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(CONFIG_APP_IMU_INT_GPIO, GPIO_INTR_LOW_LEVEL));

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * La interrupcion ya viene apagada desde la ISR. Falta el despertador
         * del light sleep, que vive en otro bit del mismo pin y no se entera.
         * Con la linea INT enclavada abajo el chip intentaria dormirse, el
         * nivel bajo lo despertaria al instante, y vuelta a empezar: girando en
         * vacio toda la pausa.
         */
        gpio_wakeup_disable(CONFIG_APP_IMU_INT_GPIO);

        xSemaphoreTake(s_draw_mux, portMAX_DELAY);
        bool movimiento = imu_atender_int();
        xSemaphoreGive(s_draw_mux);

        bool agitado = false;
        if (movimiento) {
            /* La pantalla se enciende ya, sin esperar a saber si ademas fue una
               sacudida: medio segundo de retraso en encender se nota. */
            s_screen_req_off = false;
            s_last_activity_us = esp_timer_get_time();
            xTaskNotifyGive(s_display_h);

            agitado = detectar_agite();
            if (agitado) {
                agite_cambiar_vista();
                s_last_activity_us = esp_timer_get_time();
                xTaskNotifyGive(s_display_h);
            }
        }

        /*
         * Mientras el reloj siga en movimiento la interrupcion se redispara sin
         * parar, y cada una saca al chip del light sleep. A 200 ms eran cinco
         * despertares por segundo llevandolo puesto, suficiente para que el
         * radio perdiera eventos de conexion y el enlace se cayera cada poco.
         *
         * Dos segundos bastan: lo unico que hace falta es reiniciar la cuenta
         * atras de la pantalla, y esa es de ocho.
         *
         * Tras una sacudida se espera mucho menos. Ahi estas usando el reloj a
         * proposito y lo normal es querer encadenar varias para llegar a la
         * vista que buscas; dos segundos y medio entre una y otra harian pensar
         * que no te ha hecho caso.
         */
        vTaskDelay(pdMS_TO_TICKS(agitado ? 250 : 2000));

        ulTaskNotifyTake(pdTRUE, 0);
        /* Sin ESP_ERROR_CHECK: esto corre en un bucle mientras el reloj se usa,
           y ahi abortar significa reiniciarlo. Quedarse sin despertador por
           movimiento es un incordio; reiniciar en la muñeca, un fallo. */
        esp_err_t werr = gpio_wakeup_enable(CONFIG_APP_IMU_INT_GPIO, GPIO_INTR_LOW_LEVEL);
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "no se pudo rearmar el despertador del sensor: %s",
                     esp_err_to_name(werr));
        }
        gpio_intr_enable(CONFIG_APP_IMU_INT_GPIO);
    }
}

#endif /* CONFIG_APP_IMU_INT_GPIO >= 0 */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_SW:         return "software (esp_restart)";
    case ESP_RST_PANIC:      return "panico/excepcion";
    case ESP_RST_INT_WDT:    return "perro guardian de interrupciones";
    case ESP_RST_TASK_WDT:   return "perro guardian de tareas";
    case ESP_RST_WDT:        return "perro guardian";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_DEEPSLEEP:  return "deep sleep";
    case ESP_RST_JTAG:       return "JTAG";
    default:                 return "desconocido";
    }
}

void app_main(void)
{
    /*
     * Diagnostico: motivo del ultimo reinicio y RAM libre. Con light sleep la
     * consola USB es poco fiable, asi que este log es lo que confirma si el
     * bucle de arranque es un panico (PANIC), un perro guardian (INT_WDT /
     * TASK_WDT) o un esp_restart() (SW).
     */
    ESP_LOGI(TAG, "reinicio por: %s (%d), heap libre %lu",
             reset_reason_str(esp_reset_reason()), (int)esp_reset_reason(),
             (unsigned long)esp_get_free_heap_size());

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

    /*
     * Dejar limpios los pines con interrupcion antes de tocar nada mas.
     *
     * Un reinicio por software no limpia la configuracion del GPIO, asi que una
     * interrupcion por nivel armada en el arranque anterior sigue armada. Si
     * ademas la linea esta abajo —y la del MPU lo esta, es activa a nivel bajo y
     * queda enclavada—, en cuanto gpio_install_isr_service() habilita la
     * interrupcion empieza una tormenta sin manejador que la desactive, y el
     * perro guardian de interrupciones mata el arranque. Que a su vez reinicia
     * por software y deja el terreno igual: se realimenta solo.
     */
    gpio_reset_pin(CONFIG_APP_BUTTON_GPIO);
#if CONFIG_APP_IMU_INT_GPIO >= 0
    gpio_reset_pin(CONFIG_APP_IMU_INT_GPIO);
#endif

    borrar_historial_viejo();
    anim_cargar();

    /* Sin APP_BATTERY_GPIO configurado no hace nada y no falla. */
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "sin indicador de bateria");
    }

    ESP_ERROR_CHECK(ssd1306_init(&s_oled,
                                 CONFIG_APP_I2C_SDA_GPIO,
                                 CONFIG_APP_I2C_SCL_GPIO,
                                 CONFIG_APP_OLED_ADDR,
                                 400000,
                                 CONFIG_APP_OLED_HEIGHT));

#if CONFIG_APP_IMU_INT_GPIO >= 0
    /* Antes de la pantalla de arranque para poder decir ahi si respondio.
       Comparte el bus que acaba de crear el driver del OLED. */
    if (imu_init(s_oled.bus, CONFIG_APP_IMU_ADDR,
                 CONFIG_APP_IMU_UMBRAL, CONFIG_APP_IMU_DURACION_MS) != ESP_OK) {
        ESP_LOGW(TAG, "sin MPU-6050; la pantalla solo respondera al boton");
    } else {
        /*
         * Con el enclavamiento recien soltado, la linea INT tiene que estar
         * arriba. Si sigue abajo es que ese pin no es INT: pasa con AD0, que
         * esta al lado en el modulo y lleva su propia resistencia a masa, asi
         * que se queda clavado en bajo. Armar una interrupcion por nivel sobre
         * el es una tormenta que no para nunca.
         *
         * El sensor contesta igual por I2C, asi que sin esta comprobacion todo
         * parece correcto hasta que el arranque se cuelga sin explicacion.
         */
        const gpio_config_t sonda = {
            .pin_bit_mask = 1ULL << CONFIG_APP_IMU_INT_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&sonda));
        if (gpio_get_level(CONFIG_APP_IMU_INT_GPIO) == 0) {
            ESP_LOGE(TAG, "GPIO %d clavado en bajo: no parece la linea INT del "
                          "MPU-6050 (¿es AD0?). Se renuncia al sensor.",
                     CONFIG_APP_IMU_INT_GPIO);
            imu_descartar();
        }
    }
#endif

    /*
     * Pantalla de arranque con la ranura y la hora de compilacion. Es la unica
     * forma comoda de comprobar que un OTA entro: tras actualizar tiene que
     * cambiar de app0 a app1 (o al reves) y la hora tiene que ser la nueva. Va
     * en el OLED y no en el log porque con light sleep la consola USB no es
     * fiable, que es justo la configuracion normal del reloj.
     */
    {
        const esp_app_desc_t *desc = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
        /* Holgado a proposito: version[32], label[17] y el sufijo del sensor no
           caben en menos, y el compilador trata el truncado posible como error.
           En pantalla son "V1.1.2  app0  IMU", diecisiete caracteres. */
        char linea[72];

        /*
         * La pantalla de arranque es el unico sitio fiable para informar: con
         * light sleep la consola USB no aguanta, asi que si el sensor no
         * responde no habria forma comoda de enterarse.
         */
        const char *imu = "";
#if CONFIG_APP_IMU_INT_GPIO >= 0
        imu = imu_disponible() ? "  IMU" : "  SIN IMU";
#endif

        text_center(2, "BITCAT WATCH", 1);
        snprintf(linea, sizeof(linea), "V%s  %s%s", desc->version, run->label, imu);
        text_center(14, linea, 1);
        ssd1306_text(&s_oled, 0, 25, "INICIANDO BLE...", 1, true);
        ssd1306_flush(&s_oled);

        ESP_LOGI(TAG, "version %s desde '%s', compilado %s %s",
                 desc->version, run->label, desc->date, desc->time);
    }

    /* El UUID del servicio es el mismo en todas las unidades: identifica al
       modelo, no al aparato. Para poder distinguirlos en el selector del celular
       se le pega al nombre los dos ultimos bytes de la MAC, unica de fabrica,
       asi que el mismo binario sirve para todos sin configurar nada por unidad. */
    static char ble_name[32];
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_BT);
    if (mac_err == ESP_OK) {
        snprintf(ble_name, sizeof(ble_name), "%s %02X%02X",
                 CONFIG_APP_BLE_NAME, mac[4], mac[5]);
    } else {
        /* Sin MAC legible se anuncia con el nombre pelado: peor para distinguir
           dos relojes, pero preferible a no anunciarse. */
        ESP_LOGW(TAG, "no se pudo leer la MAC (%s), nombre sin sufijo",
                 esp_err_to_name(mac_err));
        snprintf(ble_name, sizeof(ble_name), "%s", CONFIG_APP_BLE_NAME);
    }

    const ble_sync_cb_t cb = {.on_time = on_time, .on_weather = on_weather,
                              .on_forecast = on_forecast};
    err = ble_sync_start_debug(ble_name, &cb, boot_ble_status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE no pudo arrancar: %s", esp_err_to_name(err));
        ssd1306_clear(&s_oled);
        text_center(2, "BITCAT WATCH", 1);
        text_center(14, "BLE FALLO", 1);
        ssd1306_text(&s_oled, 0, 25, esp_err_to_name(err), 1, true);
        ssd1306_flush(&s_oled);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "listo como \"%s\", esperando al celular", ble_name);

    /* Una sola vez para todos los pines con interrupcion. */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    xTaskCreate(display_task, "display", 4096, NULL, 4, &s_display_h);
    xTaskCreate(button_task, "button", 3072, NULL, 5, &s_button_h);

#if CONFIG_APP_IMU_INT_GPIO >= 0
    if (imu_disponible()) {
        /* 4096 y no 3072: la ventana del gesto guarda 40 muestras de los tres
           ejes mas la copia que ordena la mediana, y eso es medio kilo de pila
           que antes no hacia falta. */
        xTaskCreate(imu_task, "imu", 4096, NULL, 5, &s_imu_h);
    }
#endif

    ESP_LOGI(TAG, "tareas creadas, configurando PM");

#if CONFIG_APP_LIGHT_SLEEP
    /* Se configura al final, cuando ya no queda nada que despierte al CPU de
       forma periodica: el boton va por interrupcion y la pantalla se refresca
       una vez por segundo. */
    const esp_pm_config_t pm = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,     /* frecuencia del cristal principal */
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
    ESP_LOGI(TAG, "light sleep activo (80/40 MHz)");
#else
    ESP_LOGI(TAG, "PM configurado (sin light sleep)");
#endif
    ESP_LOGI(TAG, "PM ok, app lista");
}
