#include "anim.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "anim";

#define NVS_NS   "bitcat"
#define NVS_ANIM "anim"

/*
 * Los cuadros van seguidos en un solo bloque para que el desplazamiento que
 * manda el celular sea directo y no haya que partirlo por cuadros aqui.
 */
typedef struct {
    uint8_t frames;
    uint8_t delay_x10ms;
    uint8_t datos[ANIM_FRAMES_MAX * ANIM_FRAME_BYTES];
} anim_t;

static anim_t s_a;
static bool s_listo;
static bool s_recibiendo;
static volatile bool s_por_guardar;

/* Bytes que tiene que traer la transferencia en curso. */
static uint32_t esperados(void)
{
    return (uint32_t)s_a.frames * ANIM_FRAME_BYTES;
}

esp_err_t anim_begin(uint8_t frames, uint8_t delay_x10ms)
{
    if (frames == 0 || frames > ANIM_FRAMES_MAX) {
        ESP_LOGE(TAG, "%u cuadros: fuera de rango", frames);
        return ESP_ERR_INVALID_ARG;
    }
    /* Menos de 30 ms por cuadro no se veria: el panel entero se manda por I2C en
       algo mas de diez, y pedirle mas solo dejaria a la pantalla sin acabar de
       pintar un cuadro cuando ya toca el siguiente. */
    if (delay_x10ms < 3) {
        delay_x10ms = 3;
    }

    s_listo = false;          /* lo que se vea ahora deja de ser valido */
    s_recibiendo = true;
    s_a.frames = frames;
    s_a.delay_x10ms = delay_x10ms;
    memset(s_a.datos, 0, sizeof(s_a.datos));

    ESP_LOGI(TAG, "recibiendo %u cuadros a %u ms", frames, delay_x10ms * 10);
    return ESP_OK;
}

esp_err_t anim_write(uint16_t offset, const uint8_t *datos, uint16_t len)
{
    if (!s_recibiendo) {
        return ESP_ERR_INVALID_STATE;
    }
    /* En dos partes y con el total a la izquierda: 'offset + len' con enteros de
       16 bits podria dar la vuelta y colarse como valido. */
    if (offset > esperados() || len > esperados() - offset) {
        ESP_LOGE(TAG, "trozo fuera de sitio: %u+%u de %lu",
                 offset, len, (unsigned long)esperados());
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&s_a.datos[offset], datos, len);
    return ESP_OK;
}

esp_err_t anim_end(void)
{
    if (!s_recibiendo) {
        return ESP_ERR_INVALID_STATE;
    }
    s_recibiendo = false;
    s_listo = true;
    s_por_guardar = true;
    ESP_LOGI(TAG, "animacion lista: %u cuadros", s_a.frames);
    return ESP_OK;
}

void anim_borrar(void)
{
    s_listo = false;
    s_recibiendo = false;
    s_a.frames = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_ANIM);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "animacion borrada");
}

bool anim_disponible(void)
{
    return s_listo && s_a.frames > 0;
}

uint8_t anim_frames(void)
{
    return s_listo ? s_a.frames : 0;
}

uint16_t anim_delay_ms(void)
{
    /* Nunca cero: quien lo use como espera del bucle de dibujo se quedaria
       girando a toda velocidad si la flash devolviera un valor raro. */
    uint8_t d = s_a.delay_x10ms < 3 ? 3 : s_a.delay_x10ms;
    return (uint16_t)d * 10;
}

const uint8_t *anim_frame(uint8_t i)
{
    if (!anim_disponible() || i >= s_a.frames) {
        return NULL;
    }
    return &s_a.datos[(size_t)i * ANIM_FRAME_BYTES];
}

void anim_cargar(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* primera vez: todavia no existe el espacio */
    }

    size_t len = sizeof(s_a);
    esp_err_t err = nvs_get_blob(h, NVS_ANIM, &s_a, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_a) ||
        s_a.frames == 0 || s_a.frames > ANIM_FRAMES_MAX) {
        memset(&s_a, 0, sizeof(s_a));
        return;
    }
    s_listo = true;
    ESP_LOGI(TAG, "animacion recuperada de nvs: %u cuadros", s_a.frames);
}

void anim_guardar_si_toca(void)
{
    if (!s_por_guardar) {
        return;
    }
    s_por_guardar = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open fallo: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, NVS_ANIM, &s_a, sizeof(s_a));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no se pudo guardar la animacion: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "animacion guardada en flash");
    }
}
