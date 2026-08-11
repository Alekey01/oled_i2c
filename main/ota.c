#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static bool s_activo;
static uint32_t s_total;
static uint32_t s_recibido;

static void reiniciar_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "reiniciando con el firmware nuevo");
    esp_restart();
}

esp_err_t ota_begin(uint32_t tamano)
{
    ota_abort();

    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "no hay ranura libre: revisa la tabla de particiones");
        return ESP_ERR_NOT_FOUND;
    }
    if (tamano == 0 || tamano > s_part->size) {
        ESP_LOGE(TAG, "tamano invalido: %lu bytes en una ranura de %lu",
                 (unsigned long)tamano, (unsigned long)s_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * OTA_WITH_SEQUENTIAL_WRITES borra sector a sector conforme se escribe. Con
     * el tamano por delante, esp_ota_begin borraria los 2 MB de golpe y dejaria
     * la tarea del host BLE bloqueada un par de segundos, lo suficiente para que
     * el celular diera la conexion por perdida.
     */
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fallo: %s", esp_err_to_name(err));
        return err;
    }

    s_activo = true;
    s_total = tamano;
    s_recibido = 0;
    ESP_LOGI(TAG, "recibiendo %lu bytes hacia '%s'",
             (unsigned long)tamano, s_part->label);
    return ESP_OK;
}

esp_err_t ota_write(const uint8_t *datos, uint16_t len)
{
    if (!s_activo) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_recibido + len > s_total) {
        ESP_LOGE(TAG, "llegaron mas bytes de los anunciados, se cancela");
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(s_handle, datos, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write fallo: %s", esp_err_to_name(err));
        ota_abort();
        return err;
    }

    s_recibido += len;
    return ESP_OK;
}

esp_err_t ota_end(void)
{
    if (!s_activo) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Un paquete perdido se detecta aqui: sin esta cuenta, esp_ota_end aceptaria
       una imagen truncada si el corte cayo justo en un limite valido. */
    if (s_recibido != s_total) {
        ESP_LOGE(TAG, "faltan bytes: %lu de %lu",
                 (unsigned long)s_recibido, (unsigned long)s_total);
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_end(s_handle);   /* valida cabecera y checksum */
    s_activo = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "imagen invalida: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo cambiar el arranque: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "actualizacion lista, arrancara desde '%s'", s_part->label);
    xTaskCreate(reiniciar_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void ota_abort(void)
{
    if (!s_activo) {
        return;
    }
    esp_ota_abort(s_handle);
    s_activo = false;
    s_recibido = 0;
    s_total = 0;
    ESP_LOGW(TAG, "actualizacion cancelada");
}

void ota_estado(ota_estado_t *out)
{
    out->activo = s_activo;
    out->total = s_total;
    out->recibido = s_recibido;
}

void ota_marcar_valido(void)
{
    const esp_partition_t *actual = esp_ota_get_running_partition();
    esp_ota_img_states_t estado;

    if (esp_ota_get_state_partition(actual, &estado) != ESP_OK) {
        return;
    }
    if (estado != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   /* arranque normal: no hay rollback pendiente */
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "firmware nuevo confirmado, rollback cancelado");
    }
}
