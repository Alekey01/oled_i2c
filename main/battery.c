#include "battery.h"

#include "sdkconfig.h"

#if CONFIG_APP_BATTERY_GPIO >= 0

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "bateria";

#define MUESTRAS 16

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_listo;
static int s_mv = -1;
static int s_pct = -1;

/*
 * Curva de descarga de un LiPo de una celda. No es lineal ni de lejos: entre
 * 4.2 y 3.8 V se va la mitad de la carga, y por debajo de 3.6 cae en picado.
 * Tomar el voltaje como porcentaje directo daria un 50% cuando queda el 15%.
 */
static const struct { int mv, pct; } CURVA[] = {
    {4200, 100}, {4060, 90}, {3980, 80}, {3920, 70}, {3870, 60},
    {3820,  50}, {3790, 40}, {3770, 30}, {3740, 20}, {3680, 10},
    {3450,   5}, {3000,  0},
};

static int mv_a_pct(int mv)
{
    if (mv >= CURVA[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < sizeof(CURVA) / sizeof(CURVA[0]); i++) {
        if (mv >= CURVA[i].mv) {
            /* Interpolacion lineal dentro del tramo. */
            int rango_mv = CURVA[i - 1].mv - CURVA[i].mv;
            int rango_pct = CURVA[i - 1].pct - CURVA[i].pct;
            return CURVA[i].pct + (mv - CURVA[i].mv) * rango_pct / rango_mv;
        }
    }
    return 0;
}

esp_err_t battery_init(void)
{
    adc_unit_t unidad;
    esp_err_t err = adc_oneshot_io_to_channel(CONFIG_APP_BATTERY_GPIO, &unidad, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d no es un pin de ADC", CONFIG_APP_BATTERY_GPIO);
        return err;
    }

    const adc_oneshot_unit_init_cfg_t cfg = {.unit_id = unidad};
    err = adc_oneshot_new_unit(&cfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }

    /* 12 dB de atenuacion llegan a ~3.1 V; el divisor deja el LiPo en 2.1 V. */
    const adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, s_chan, &chan);
    if (err != ESP_OK) {
        return err;
    }

    /* Sin calibracion el error del ADC ronda el 10%, que en la zona plana de la
       curva son veinte puntos de porcentaje. */
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = unidad,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "sin calibracion de fabrica, la lectura sera aproximada");
        s_cali = NULL;
    }

    s_listo = true;
    ESP_LOGI(TAG, "indicador en GPIO %d, divisor %d.%02d",
             CONFIG_APP_BATTERY_GPIO,
             CONFIG_APP_BATTERY_DIVISOR_X100 / 100,
             CONFIG_APP_BATTERY_DIVISOR_X100 % 100);

    battery_actualizar();
    return ESP_OK;
}

bool battery_disponible(void)
{
    return s_listo;
}

void battery_actualizar(void)
{
    if (!s_listo) {
        return;
    }

    /* El ADC del ESP32 es ruidoso de por si; promediar unas cuantas muestras
       evita que el icono baile entre dos niveles sin que pase nada. */
    int suma = 0, validas = 0;
    for (int i = 0; i < MUESTRAS; i++) {
        int crudo;
        if (adc_oneshot_read(s_adc, s_chan, &crudo) != ESP_OK) {
            continue;
        }
        int mv = crudo;
        if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, crudo, &mv) != ESP_OK) {
            continue;
        }
        suma += mv;
        validas++;
    }
    if (validas == 0) {
        return;
    }

    int mv_pin = suma / validas;
    s_mv = mv_pin * CONFIG_APP_BATTERY_DIVISOR_X100 / 100;
    s_pct = mv_a_pct(s_mv);
}

bool battery_estado(int *mv, int *pct)
{
    if (!s_listo || s_mv < 0) {
        return false;
    }
    if (mv) *mv = s_mv;
    if (pct) *pct = s_pct;
    return true;
}

#else /* sin GPIO configurado */

esp_err_t battery_init(void) { return ESP_OK; }
bool battery_disponible(void) { return false; }
void battery_actualizar(void) {}
bool battery_estado(int *mv, int *pct) { (void)mv; (void)pct; return false; }

#endif
