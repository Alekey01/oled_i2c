#include "imu.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

/* Registros del MPU-6050 que se usan aqui. */
#define REG_CONFIG        0x1A
#define REG_ACCEL_CONFIG  0x1C
#define REG_MOT_THR       0x1F
#define REG_MOT_DUR       0x20
#define REG_INT_PIN_CFG   0x37
#define REG_INT_ENABLE    0x38
#define REG_INT_STATUS    0x3A
#define REG_ACCEL_XOUT_H  0x3B
#define REG_MOT_DETECT_CTRL 0x69
#define REG_PWR_MGMT_1    0x6B
#define REG_PWR_MGMT_2    0x6C
#define REG_WHO_AM_I      0x75

#define INT_MOTION_BIT    0x40

/* A +/-2 g la escala son 16384 cuentas por g. */
#define LSB_POR_G 16384

static i2c_master_dev_handle_t s_dev;
static bool s_listo;
static uint32_t s_eventos;

static esp_err_t escribir(uint8_t reg, uint8_t valor)
{
    const uint8_t b[2] = {reg, valor};
    return i2c_master_transmit(s_dev, b, sizeof(b), 100);
}

static esp_err_t leer(uint8_t reg, uint8_t *destino, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, destino, n, 100);
}

esp_err_t imu_init(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t umbral, uint8_t duracion_ms)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add_device");

    /* Reset y espera: tras el reset el chip tarda unos ms en responder bien. */
    ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_1, 0x80), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_1, 0x00), TAG, "despertar");
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t quien = 0;
    ESP_RETURN_ON_ERROR(leer(REG_WHO_AM_I, &quien, 1), TAG, "who_am_i");
    if (quien != 0x68) {
        ESP_LOGE(TAG, "WHO_AM_I devolvio 0x%02X, no parece un MPU-6050", quien);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * La deteccion de movimiento del MPU-6050 mide el cambio respecto a la
     * muestra anterior, y para eso necesita el filtro paso alto encendido. Con
     * el apagado la gravedad cuenta como aceleracion y la interrupcion salta
     * sola en cuanto el reloj esta en cualquier posicion que no sea plana.
     */
    ESP_RETURN_ON_ERROR(escribir(REG_CONFIG, 0x00), TAG, "config");
    ESP_RETURN_ON_ERROR(escribir(REG_ACCEL_CONFIG, 0x01), TAG, "accel_config");  /* +/-2g, HPF 5 Hz */
    ESP_RETURN_ON_ERROR(escribir(REG_MOT_THR, umbral), TAG, "umbral");
    ESP_RETURN_ON_ERROR(escribir(REG_MOT_DUR, duracion_ms), TAG, "duracion");
    ESP_RETURN_ON_ERROR(escribir(REG_MOT_DETECT_CTRL, 0x15), TAG, "detect_ctrl");

    /* Activa a nivel bajo, enclavada y limpiable con cualquier lectura. Nivel y
       no flanco porque es lo unico que despierta del light sleep. */
    ESP_RETURN_ON_ERROR(escribir(REG_INT_PIN_CFG, 0xB0), TAG, "int_pin_cfg");
    ESP_RETURN_ON_ERROR(escribir(REG_INT_ENABLE, INT_MOTION_BIT), TAG, "int_enable");

    /*
     * Modo de bajo consumo: solo acelerometro, giroscopio y termometro en
     * reposo, y el chip despertando 5 veces por segundo para muestrear. Es la
     * diferencia entre unos 3.9 mA y unas pocas decenas de microamperios.
     */
    /*
     * 20 Hz y no 5. El contador de duracion solo avanza cuando hay una muestra
     * nueva, asi que la frecuencia de muestreo marca cuanto movimiento seguido
     * hace falta: a 5 Hz las muestras van cada 200 ms y exigir unas pocas
     * significa exigir casi un segundo agitando. A 20 Hz responde al momento y
     * la diferencia de consumo son decenas de microamperios.
     */
    ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_2, 0x87), TAG, "pwr2");   /* 20 Hz + giro en standby */
    ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_1, 0x28), TAG, "pwr1");   /* CYCLE + TEMP_DIS */

    s_listo = true;

    /* Soltar el enclavamiento aqui mismo deja la linea INT arriba antes de que
       nadie arme la interrupcion del GPIO. Con ella abajo, habilitar una
       interrupcion por nivel sin manejador cuelga el arranque. */
    uint8_t descarte = 0;
    leer(REG_INT_STATUS, &descarte, 1);

    ESP_LOGI(TAG, "MPU-6050 en 0x%02X, umbral %u, duracion %u ms", addr, umbral, duracion_ms);
    return ESP_OK;
}

bool imu_disponible(void)
{
    return s_listo;
}

/*
 * Sube el sensor a pleno rendimiento un momento, y luego lo devuelve a dormir.
 *
 * En modo ciclo el acelerometro solo refresca 20 veces por segundo: una muestra
 * cada 50 ms. Un manotazo dura dos o tres decimas, o sea que caben cinco o seis
 * muestras en todo el gesto —demasiado pocas para saber hacia donde empezo—.
 * Fuera del modo ciclo el registro se actualiza a 1 kHz y se puede leer todo lo
 * seguido que deje el bus.
 *
 * Se paga con corriente: unos 500 uA en vez de decenas. Como solo dura mientras
 * se mide el gesto y solo ocurre cuando ya te has movido, es medio segundo de
 * cada vez y no se nota en la bateria.
 */
esp_err_t imu_modo_rapido(bool rapido)
{
    if (!s_listo) {
        return ESP_ERR_INVALID_STATE;
    }

    if (rapido) {
        /* Primero salir del ciclo, luego quitar el temporizador de despertar. */
        ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_1, 0x08), TAG, "pwr1 rapido");
        ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_2, 0x07), TAG, "pwr2 rapido");
        /* El acelerometro tarda un par de muestras en dar valores buenos tras
           salir del ciclo; sin esta pausa las primeras son basura, que es justo
           donde se decide el sentido del gesto. */
        vTaskDelay(pdMS_TO_TICKS(20));
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_2, 0x87), TAG, "pwr2 lento");
    ESP_RETURN_ON_ERROR(escribir(REG_PWR_MGMT_1, 0x28), TAG, "pwr1 lento");
    return ESP_OK;
}

void imu_descartar(void)
{
    s_listo = false;
}

bool imu_atender_int(void)
{
    if (!s_listo) {
        return false;
    }
    uint8_t estado = 0;
    if (leer(REG_INT_STATUS, &estado, 1) != ESP_OK) {
        return false;
    }
    if ((estado & INT_MOTION_BIT) == 0) {
        return false;
    }
    s_eventos++;
    return true;
}

uint32_t imu_eventos(void)
{
    return s_eventos;
}

/*
 * Que eje del sensor cae en el sentido izquierda-derecha depende de como este
 * pegado el modulo, y desde el firmware no hay forma de saberlo. Guardando aqui
 * la ultima medida y publicandola por BLE, se averigua agitando el reloj y
 * mirando el numero, sin cable y sin tener que abrir nada.
 */
static int s_agite_eje = -1;
static int s_agite_swing;
static int s_agite_sentido;

void imu_anotar_agite(int eje, int swing, int sentido)
{
    s_agite_eje = eje;
    s_agite_swing = swing;
    s_agite_sentido = sentido;
}

void imu_ultimo_agite(int *eje, int *swing, int *sentido)
{
    if (eje)     *eje = s_agite_eje;
    if (swing)   *swing = s_agite_swing;
    if (sentido) *sentido = s_agite_sentido;
}

esp_err_t imu_leer(int *x_mg, int *y_mg, int *z_mg)
{
    if (!s_listo) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t b[6];
    ESP_RETURN_ON_ERROR(leer(REG_ACCEL_XOUT_H, b, sizeof(b)), TAG, "accel");

    const int16_t crudo[3] = {
        (int16_t)((b[0] << 8) | b[1]),
        (int16_t)((b[2] << 8) | b[3]),
        (int16_t)((b[4] << 8) | b[5]),
    };
    int *salida[3] = {x_mg, y_mg, z_mg};
    for (int i = 0; i < 3; i++) {
        if (salida[i]) {
            *salida[i] = crudo[i] * 1000 / LSB_POR_G;
        }
    }
    return ESP_OK;
}
