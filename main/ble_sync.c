#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "anim.h"
#include "ble_sync.h"
#include "imu.h"
#include "ota.h"

static const char *TAG = "ble_sync";

/* Los UUID de 128 bits se declaran con los bytes al reves (LSB primero). */
#define UUID128_APP(b2, b3) \
    BLE_UUID128_INIT(0x90, 0x8a, 0x6f, 0x4e, 0x2d, 0x1b, 0x3a, 0x9c, \
                     0x1d, 0x4f, 0x2e, 0x7a, b2, b3, 0x8b, 0x5c)

static const ble_uuid128_t svc_uuid     = UUID128_APP(0x01, 0x00);
static const ble_uuid128_t chr_time     = UUID128_APP(0x02, 0x00);
static const ble_uuid128_t chr_weather  = UUID128_APP(0x03, 0x00);
static const ble_uuid128_t chr_ota_ctrl = UUID128_APP(0x04, 0x00);
static const ble_uuid128_t chr_ota_data = UUID128_APP(0x05, 0x00);
static const ble_uuid128_t chr_info     = UUID128_APP(0x06, 0x00);
static const ble_uuid128_t chr_anim     = UUID128_APP(0x07, 0x00);
static const ble_uuid128_t chr_fc       = UUID128_APP(0x08, 0x00);

/* Ordenes de la caracteristica de animacion. */
#define ANIM_CMD_START  0x01   /* + cuadros(1) + retardo en decenas de ms(1) */
#define ANIM_CMD_DATA   0x02   /* + offset(2, LE) + bytes */
#define ANIM_CMD_END    0x03
#define ANIM_CMD_BORRAR 0x04

/* Cota del trozo de animacion. Son 2 KB en total, asi que no merece la pena
   apurar el MTU: con trozos comodos entra en una decena de escrituras. */
#define ANIM_CHUNK_MAX 256

/* Ordenes que acepta la caracteristica de control del OTA. */
#define OTA_CMD_START  0x01   /* + uint32 tamano */
#define OTA_CMD_END    0x02
#define OTA_CMD_ABORT  0x03

/* Estados que devuelve por notificacion. */
#define OTA_EST_LISTO     0x01
#define OTA_EST_PROGRESO  0x02
#define OTA_EST_OK        0x03
#define OTA_EST_ERROR     0x04

/* Cada cuantos bytes avisa del avance. Notificar por paquete saturaria el
   enlace justo cuando lo estamos usando al maximo. */
#define OTA_AVISO_CADA (16 * 1024)

/* Cota del trozo de datos: con MTU de 517 el celular puede mandar 512 utiles. */
#define OTA_CHUNK_MAX 512

static ble_sync_cb_t s_cb;
static uint8_t s_addr_type;
static const char *s_name;
static volatile bool s_connected;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ota_ctrl_handle;
static uint32_t s_ota_ultimo_aviso;

/*
 * Diagnostico del enlace, que viaja en la caracteristica de info.
 *
 * Con light sleep la consola USB no sirve para esto: el reloj tiene que estar
 * suelto y en la muñeca para reproducir la caida, y ahi no hay cable. Asi que
 * el reloj se acuerda de por que se cayo la ultima vez y lo cuenta cuando el
 * celular vuelve a conectarse, que es justo el momento en que se puede leer.
 */
static uint16_t s_caidas;
static uint16_t s_ultima_razon;
static uint16_t s_itvl;        /* unidades de 1.25 ms */
static uint16_t s_latencia;
static uint16_t s_timeout;     /* unidades de 10 ms */

static void advertise(void);

/* NimBLE devuelve los codigos del HCI sumados a 0x200 (BLE_HS_HCI_ERR). */
static const char *razon_str(uint16_t razon)
{
    switch (razon) {
    case 0x208: return "supervision timeout (el reloj perdio eventos de radio)";
    case 0x213: return "lo cerro el celular";
    case 0x216: return "lo cerro el reloj";
    case 0x222: return "timeout de LMP/LL";
    case 0x23d: return "fallo de MIC/cifrado";
    case 0x23e: return "no se llego a establecer";
    case 0x228: return "instante pasado (parametros mal negociados)";
    default:    return "otra";
    }
}

/*
 * Motivo del ultimo arranque, en corto. Va por BLE porque es la unica via que
 * queda: el fallo solo sale con light sleep, y con light sleep no hay consola
 * USB. Es lo que distingue un panico de un perro guardian o de un brownout, que
 * llevan a sitios completamente distintos.
 */
static const char *reset_corto(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panico";
    case ESP_RST_INT_WDT:   return "intwdt";
    case ESP_RST_TASK_WDT:  return "taskwdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "otro";
    }
}

/* Anota los parametros que de verdad estan en vigor, que no tienen por que ser
   los que se pidieron: el celular puede quedarse con los suyos. */
static void anotar_params(uint16_t handle)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(handle, &d) != 0) {
        return;
    }
    s_itvl = d.conn_itvl;
    s_latencia = d.conn_latency;
    s_timeout = d.supervision_timeout;
}

static void avisar_paso(ble_sync_progress_cb_t progreso, const char *paso)
{
    ESP_LOGI(TAG, "arranque BLE: %s", paso);
    if (progreso) {
        progreso(paso);
    }
}

/* ------------------------------------------------------------------- OTA */

/*
 * Durante la transferencia hace falta el enlace rapido: con los 300 ms y la
 * latencia 4 del funcionamiento normal, medio mega tardaria horas. Al terminar
 * se vuelve al modo lento, que es el que hace que la bateria dure.
 */
static void pedir_conexion(bool rapida)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct ble_gap_upd_params p;
    if (rapida) {
        p = (struct ble_gap_upd_params){
            .itvl_min = 12,               /* 15 ms */
            .itvl_max = 24,               /* 30 ms */
            .latency = 0,
            /* Holgado: borrar un sector de flash puede dejar al reloj sin
               atender el radio varias decenas de ms, y con 4 s el celular daba
               la conexion por perdida a media actualizacion. */
            .supervision_timeout = 1000,  /* 10 s */
        };
    } else {
        p = (struct ble_gap_upd_params){
            .itvl_min = 80,               /* 100 ms */
            .itvl_max = 240,              /* 300 ms */
            .latency = 4,
            /*
             * 20 s y no 6. Con latencia 4 el radio ya se salta 4 eventos por
             * diseño, asi que 6 s dejaban margen para perder apenas un par mas.
             * Cada interrupcion del sensor saca al chip del light sleep, y ese
             * vaivén hace que se pierdan eventos de vez en cuando: con el margen
             * corto, cada racha terminaba en desconexion. Esperar mas a darla
             * por perdida no cuesta bateria; reconectar, si.
             */
            .supervision_timeout = 2000,  /* 20 s */
        };
    }

    int rc = ble_gap_update_params(s_conn, &p);
    if (rc != 0) {
        ESP_LOGW(TAG, "no se pudo cambiar el intervalo a %s: %d",
                 rapida ? "rapido" : "lento", rc);
    }
}

/*
 * Los parametros lentos no se piden al conectar, sino unos segundos despues.
 *
 * La spec le pide al periferico que espere T_GAP(conn_pause_peripheral) = 5 s
 * antes de tocar los parametros, porque en ese hueco el celular esta
 * descubriendo servicios. Pidiendolo dentro del propio evento CONNECT, Android
 * lo ignoraba: el enlace se quedaba con sus valores de arranque —7.5 ms de
 * intervalo, latencia 0— que son los mas agresivos que hay. O sea que el modo
 * de ahorro no se activaba nunca y el radio iba a tope todo el rato.
 */
#define PAUSA_PARAMS_US (5LL * 1000000)

static esp_timer_handle_t s_timer_params;

static void params_lentos_cb(void *arg)
{
    pedir_conexion(false);
}

static void programar_params_lentos(void)
{
    if (s_timer_params == NULL) {
        const esp_timer_create_args_t args = {
            .callback = params_lentos_cb,
            .name = "ble_params",
        };
        if (esp_timer_create(&args, &s_timer_params) != ESP_OK) {
            ESP_LOGW(TAG, "sin temporizador: se piden los parametros ya mismo");
            pedir_conexion(false);
            return;
        }
    }
    esp_timer_stop(s_timer_params);   /* da igual si no estaba armado */
    esp_timer_start_once(s_timer_params, PAUSA_PARAMS_US);
}

/* Notificacion de 6 bytes: estado, bytes recibidos (uint32 LE) y codigo de error. */
static void notificar_ota(uint8_t estado, uint8_t err)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_ota_ctrl_handle == 0) {
        return;
    }

    ota_estado_t e;
    ota_estado(&e);

    uint8_t p[6];
    p[0] = estado;
    memcpy(&p[1], &e.recibido, 4);
    p[5] = err;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(p, sizeof(p));
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn, s_ota_ctrl_handle, om);
    }
}

static int ota_ctrl_write(const uint8_t *buf, uint16_t len)
{
    if (len < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    switch (buf[0]) {
    case OTA_CMD_START: {
        if (len != 5) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint32_t tamano;
        memcpy(&tamano, &buf[1], 4);
        s_ota_ultimo_aviso = 0;

        esp_err_t err = ota_begin(tamano);
        if (err != ESP_OK) {
            notificar_ota(OTA_EST_ERROR, 1);
            return BLE_ATT_ERR_UNLIKELY;
        }
        pedir_conexion(true);
        notificar_ota(OTA_EST_LISTO, 0);
        return 0;
    }

    case OTA_CMD_END: {
        esp_err_t err = ota_end();
        pedir_conexion(false);
        notificar_ota(err == ESP_OK ? OTA_EST_OK : OTA_EST_ERROR, err == ESP_OK ? 0 : 2);
        return err == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
    }

    case OTA_CMD_ABORT:
        ota_abort();
        pedir_conexion(false);
        notificar_ota(OTA_EST_ERROR, 3);
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ------------------------------------------------------------- ANIMACION */

static int anim_ctrl_write(const uint8_t *buf, uint16_t len)
{
    if (len < 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    switch (buf[0]) {
    case ANIM_CMD_START:
        if (len != 3) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        return anim_begin(buf[1], buf[2]) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;

    case ANIM_CMD_DATA: {
        if (len < 4) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint16_t off = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
        return anim_write(off, &buf[3], len - 3) == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
    }

    case ANIM_CMD_END:
        return anim_end() == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;

    case ANIM_CMD_BORRAR:
        anim_borrar();
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ------------------------------------------------------------------ GATT */

/*
 * Version y ranura activa, en texto: "1.0.0 app0". Es lo que deja a la pagina
 * decidir si hay algo que actualizar en vez de mandar medio mega a ciegas.
 */
static int chr_info_read(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();

    /* La pagina parte por espacios y solo usa los dos primeros campos, asi que
       lo que se añada detras viaja sin romper nada. Holgado a proposito:
       version[] son 32 caracteres y label[] 17. */
    /* Eje de la ultima sacudida, en letra: es lo que hay que poner en
       APP_IMU_EJE_AGITE si agitando de lado no cambia de vista. */
    int eje = -1, swing = 0;
    imu_ultimo_agite(&eje, &swing);
    const char *eje_txt = (eje == 0) ? "X" : (eje == 1) ? "Y" : (eje == 2) ? "Z" : "-";

    char info[192];
    int n = snprintf(info, sizeof(info),
                     "%s %s mov=%lu caidas=%u razon=0x%03x itvl=%u lat=%u tmo=%u "
                     "reset=%s up=%lu ag=%s:%d",
                     desc->version, run->label, (unsigned long)imu_eventos(),
                     s_caidas, s_ultima_razon,
                     (unsigned)(s_itvl * 125 / 100),   /* ms */
                     s_latencia,
                     (unsigned)(s_timeout * 10),       /* ms */
                     reset_corto(),
                     /* Segundos desde el arranque. Si al reconectar vale menos
                        que el hueco que hubo sin enlace, es que se reinicio. */
                     (unsigned long)(esp_timer_get_time() / 1000000),
                     eje_txt, swing);
    if (n < 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (n >= (int)sizeof(info)) {
        n = sizeof(info) - 1;
    }

    return os_mbuf_append(ctxt->om, info, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* El opcode de escritura sin respuesta llega con el mismo op, asi que esto
       vale para las dos formas. */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /*
     * Los trozos del OTA se atienden aparte porque no caben en el buffer de 16
     * bytes que basta para hora y clima. El buffer es estatico y no de pila:
     * medio kilobyte dentro de la tarea del host BLE es demasiado, y como el
     * callback siempre corre en esa misma tarea no hay concurrencia que cuidar.
     */
    if (ble_uuid_cmp(ctxt->chr->uuid, &chr_ota_data.u) == 0) {
        static uint8_t datos[OTA_CHUNK_MAX];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, datos, sizeof(datos), &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (ota_write(datos, len) != ESP_OK) {
            pedir_conexion(false);
            notificar_ota(OTA_EST_ERROR, 4);
            return BLE_ATT_ERR_UNLIKELY;
        }

        ota_estado_t e;
        ota_estado(&e);
        if (e.recibido - s_ota_ultimo_aviso >= OTA_AVISO_CADA) {
            s_ota_ultimo_aviso = e.recibido;
            notificar_ota(OTA_EST_PROGRESO, 0);
        }
        return 0;
    }

    /*
     * La animacion tambien va aparte: sus trozos no caben en los 16 bytes que
     * bastan para hora y clima. Buffer propio y no el del OTA, que aunque nunca
     * coinciden, compartirlo ataria dos cosas que no tienen nada que ver.
     */
    if (ble_uuid_cmp(ctxt->chr->uuid, &chr_anim.u) == 0) {
        static uint8_t datos[ANIM_CHUNK_MAX];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, datos, sizeof(datos), &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        return anim_ctrl_write(datos, len);
    }

    /* 40 y no 16: el pronostico son 2 + 2x12 = 26 bytes, y con el buffer justo
       para la hora y el clima llegaba recortado sin dar ningun error. */
    uint8_t buf[40];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &chr_ota_ctrl.u) == 0) {
        return ota_ctrl_write(buf, len);
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &chr_time.u) == 0) {
        if (len != BLE_SYNC_TIME_LEN) {
            ESP_LOGW(TAG, "hora: se esperaban %d bytes, llegaron %d", BLE_SYNC_TIME_LEN, len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint32_t epoch;
        int32_t offset;
        memcpy(&epoch, &buf[0], 4);
        memcpy(&offset, &buf[4], 4);
        ESP_LOGI(TAG, "hora recibida: epoch=%lu offset=%ld s", (unsigned long)epoch, (long)offset);
        if (s_cb.on_time) {
            s_cb.on_time(epoch, offset);
        }
        return 0;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &chr_fc.u) == 0) {
        if (len < 2 || buf[0] == 0 || buf[0] > BLE_SYNC_FC_MAX ||
            len != (uint16_t)(2 + 2 * buf[0])) {
            ESP_LOGW(TAG, "pronostico: %d bytes no cuadran con %d horas", len, buf[0]);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        forecast_t f = {.horas = buf[0], .hora0 = buf[1]};
        for (int i = 0; i < f.horas; i++) {
            f.temp[i] = (int8_t)buf[2 + i * 2];
            f.prob[i] = buf[3 + i * 2];
        }
        ESP_LOGI(TAG, "pronostico: %u horas desde las %u:00", f.horas, f.hora0);
        if (s_cb.on_forecast) {
            s_cb.on_forecast(&f);
        }
        return 0;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &chr_weather.u) == 0) {
        if (len != BLE_SYNC_WEATHER_LEN && len != BLE_SYNC_WEATHER_LEN_WIND) {
            ESP_LOGW(TAG, "clima: se esperaban %d o %d bytes, llegaron %d",
                     BLE_SYNC_WEATHER_LEN, BLE_SYNC_WEATHER_LEN_WIND, len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        int16_t temp_x10;
        memcpy(&temp_x10, &buf[0], 2);

        weather_t w = {
            .valid = true,
            .temp_c = temp_x10 / 10.0f,
            .humidity = buf[2],
            .wmo_code = buf[3],
            .wind_kmh = -1,
            .wind_deg = -1,
        };
        if (len == BLE_SYNC_WEATHER_LEN_WIND) {
            w.wind_kmh = buf[4];
            w.wind_deg = buf[5] * 2;   /* viene dividido entre 2 para caber en un byte */
        }

        ESP_LOGI(TAG, "clima recibido: %.1f C, %d %%, wmo %d, viento %d km/h %d deg",
                 w.temp_c, w.humidity, w.wmo_code, w.wind_kmh, w.wind_deg);
        if (s_cb.on_weather) {
            s_cb.on_weather(&w);
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &chr_time.u,
                .access_cb = chr_write,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &chr_weather.u,
                .access_cb = chr_write,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                /* Control del OTA: ordenes con respuesta, avance por notificacion. */
                .uuid = &chr_ota_ctrl.u,
                .access_cb = chr_write,
                .val_handle = &s_ota_ctrl_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                /*
                 * Acepta las dos formas de escritura a proposito. Sin respuesta
                 * es mas rapido, pero no tiene control de flujo: el celular
                 * sigue mandando aunque el reloj este ocupado borrando un sector
                 * de flash, y el enlace acaba cayendose a media transferencia.
                 * Con respuesta, cada trozo espera a que el anterior este
                 * grabado. A 15 ms de intervalo son unos 30 KB/s, de sobra.
                 */
                .uuid = &chr_ota_data.u,
                .access_cb = chr_write,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &chr_info.u,
                .access_cb = chr_info_read,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                /* Solo con respuesta: son 2 KB y llegan por trozos numerados,
                   asi que perder uno en silencio dejaria un cuadro con basura
                   dentro y sin forma de enterarse. */
                .uuid = &chr_anim.u,
                .access_cb = chr_write,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &chr_fc.u,
                .access_cb = chr_write,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {0},
        },
    },
    {0},
};

/* ------------------------------------------------------------------- GAP */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_connected = (event->connect.status == 0);
        ESP_LOGI(TAG, "conexion %s", s_connected ? "establecida" : "fallida");
        if (!s_connected) {
            advertise();
            break;
        }
        s_conn = event->connect.conn_handle;
        /*
         * Los parametros lentos salen de pedir_conexion() y no de una copia
         * local. Los habia duplicados aqui y las dos copias se separaron: al
         * subir el supervision timeout se toco solo la de pedir_conexion(), que
         * unicamente corre al terminar un OTA. Toda conexion normal seguia
         * naciendo con el valor viejo, o sea que el arreglo no llegaba nunca al
         * caso que importaba.
         */
        anotar_params(s_conn);   /* lo que eligio el celular al conectar */
        programar_params_lentos();
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /*
         * Lo que se pide no es lo que queda: el celular puede rechazar la
         * peticion y quedarse con los suyos. Sin esto no hay forma de saber con
         * que intervalo y que timeout esta corriendo el enlace de verdad, que es
         * justo lo que hace falta para saber por que se cae.
         */
        anotar_params(event->conn_update.conn_handle);
        ESP_LOGI(TAG, "parametros: itvl %u (%u ms), latencia %u, timeout %u (%u ms), status %d",
                 s_itvl, s_itvl * 125 / 100, s_latencia,
                 s_timeout, s_timeout * 10, event->conn_update.status);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_caidas++;
        s_ultima_razon = event->disconnect.reason;
        if (s_timer_params != NULL) {
            esp_timer_stop(s_timer_params);   /* ya no hay a quien pedirselo */
        }
        ESP_LOGI(TAG, "desconectado: razon 0x%03x, %s (van %u)",
                 s_ultima_razon, razon_str(s_ultima_razon), s_caidas);
        s_connected = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        /* Si el enlace se cae a media actualizacion hay que soltar la ranura:
           la que esta corriendo no se toco, asi que el reloj sigue igual. */
        ota_abort();
        advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %d", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    ESP_LOGI(TAG, "advertise: preparando campos");
    /* El paquete de anuncio solo lleva flags + nombre; el UUID de 128 bits
       no cabe junto al nombre en 31 bytes, asi que va en el scan response. */
    /* Presupuesto de los 31 bytes: 3 de flags + 3 de tx power + 2 de cabecera
       del nombre dejan 23 para el nombre. Si no cabe se recorta y se marca como
       incompleto: pasarse hace que adv_set_fields falle y el reloj no se
       anuncie, que es mucho peor que un nombre corto. */
    enum { NOMBRE_MAX = 23 };
    size_t largo = strlen(s_name);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = largo > NOMBRE_MAX ? NOMBRE_MAX : largo;
    fields.name_is_complete = (largo <= NOMBRE_MAX);
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    if (largo > NOMBRE_MAX) {
        ESP_LOGW(TAG, "nombre de %d caracteres, se anuncia recortado a %d",
                 (int)largo, NOMBRE_MAX);
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields fallo: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields fallo: %d", rc);
        return;
    }

    /* Unidades de 0.625 ms. El anuncio por defecto va a ~30 ms, que para un
       reloj es un desperdicio de radio: a 500-1000 ms tarda un poco mas en
       aparecer al buscarlo y consume una fraccion. */
    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 800,    /* 500 ms */
        .itvl_max = 1600,   /* 1000 ms */
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start fallo: %d", rc);
    } else {
        ESP_LOGI(TAG, "anunciando como \"%s\"", s_name);
    }
}

static void on_sync(void)
{
    /* Aqui empieza la vida del host NimBLE: se llama al sincronizar con el
       controlador. Si el log anterior ("host task") sale y estos no, el que se
       cuelga o aborta es la inicializacion del controlador dentro de
       nimble_port_run(). */
    ESP_LOGI(TAG, "host sync: obteniendo direccion");
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no se pudo asegurar la direccion: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "host sync: direccion lista");
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no se pudo determinar la direccion: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "host sync: tipo de direccion %d, anunciando", s_addr_type);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host BLE reiniciado, razon %d", reason);
    s_connected = false;
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "host task: nimble_port_run()");
    nimble_port_run();              /* solo regresa al hacer deinit */
    ESP_LOGW(TAG, "host task: nimble_port_run() regreso");
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------- API */

bool ble_sync_connected(void)
{
    return s_connected;
}

esp_err_t ble_sync_start(const char *device_name, const ble_sync_cb_t *cb)
{
    return ble_sync_start_debug(device_name, cb, NULL);
}

esp_err_t ble_sync_start_debug(const char *device_name, const ble_sync_cb_t *cb,
                               ble_sync_progress_cb_t progreso)
{
    s_name = device_name;
    s_cb = *cb;

    avisar_paso(progreso, "nimble init");
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fallo: %s", esp_err_to_name(err));
        return err;
    }

    avisar_paso(progreso, "callbacks");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    avisar_paso(progreso, "servicios base");
    ble_svc_gap_init();
    ble_svc_gatt_init();

    avisar_paso(progreso, "contar gatt");
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg fallo: %d", rc);
        return ESP_FAIL;
    }
    avisar_paso(progreso, "agregar gatt");
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs fallo: %d", rc);
        return ESP_FAIL;
    }
    avisar_paso(progreso, "nombre");
    rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "no se pudo fijar el nombre: %d", rc);
        return ESP_FAIL;
    }

    avisar_paso(progreso, "host task");
    nimble_port_freertos_init(host_task);
    avisar_paso(progreso, "ok");
    return ESP_OK;
}
