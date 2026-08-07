# Reloj "8 bits" + Clima — ESP32-S3 (Seeed Studio) + OLED 0.91" I2C

Reloj con estética de 8 bits en un SSD1306 de 0.91" (128x32). **La hora y el clima
llegan por Bluetooth LE desde el celular: no usa WiFi ni credenciales de red.**

Tres vistas que rotan con el **botón BOOT** (GPIO 0):

```
VISTA RELOJ                       VISTA CLIMA                VISTA CANGREJO
        HH:MM                      (icono)   22`C             (criatura 31x22
   [#][ ][#][#][ ][ ][#][ ]         30x30    64%  18KMH NE     caminando y
        JUE 07 AGO                    px     PARC NUBLADO      saludando)
```

- **Segundos en 8 bits**: fila de 8 casillas, MSB a la izquierda; llena = 1, hueca = 0.
- **Iconos**: sol, luna (de noche), nube, sol tras nube, lluvia, chubasco, nieve,
  niebla y tormenta, dibujados con primitivas — sin bitmaps en flash.
- **Runa de Bluetooth** arriba a la izquierda de la vista del reloj mientras el
  celular está conectado. Si no lo está y los datos llevan más de una hora sin
  refrescarse, parpadea un cuadro hueco abajo a la derecha.
- Entre sincronizaciones el reloj corre con el oscilador interno del ESP32-S3, que
  deriva unos segundos por día. Vuelve a sincronizar cada tanto.

## Ahorro de energía

Pensado para funcionar con LiPo:

| Medida | Qué hace |
|---|---|
| Light sleep automático | CPU a 80 MHz (mínimo 40) y el chip duerme entre eventos |
| Botón por interrupción | Antes se sondeaba cada 20 ms, lo que despertaba el CPU 50 veces/s |
| Refresco a 1 Hz alineado al segundo | Antes 4 Hz; solo el segundero binario cambia a esa velocidad |
| OLED se apaga a los 5 min | Vuelve con el botón; el reloj sigue contando y recibiendo por BLE |
| Anuncio BLE a 500–1000 ms | Por defecto va a ~30 ms |
| Conexión BLE a 300 ms con latencia 4 | El radio despierta como mucho cada 1.5 s |

El controlador BLE del ESP32-S3 usa el **cristal principal** como reloj de baja
potencia (`CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL`), así que el light sleep funciona
sin cristal externo de 32 kHz. A cambio, ese cristal sigue alimentado mientras
duerme, así que el ahorro es bueno pero no tan profundo como con un 32 kHz.

> Con el light sleep activo la consola USB no es confiable. Para depurar,
> desactiva `APP_LIGHT_SLEEP` en `menuconfig`.

En la vista del cangrejo el refresco sube a 80 ms: es la vista más cara, pero se
apaga sola a los 5 min como las demás.

## Conexiones

| OLED | ESP32-S3 (XIAO) | Pin de la placa |
|------|-----------------|-----------------|
| SCL  | GPIO 6          | D5              |
| SDA  | GPIO 5          | D4              |
| VCC  | 3V3             | 3V3             |
| GND  | GND             | GND             |

Dirección I2C por defecto `0x3C`, bus a 400 kHz, pull-ups internos habilitados
(si el módulo no trae pull-ups propios y el bus falla, agrega 4.7 kΩ a 3V3).

## Compilar y flashear

```bash
idf.py -p COM3 flash monitor
```

Ajustes opcionales (nombre BLE, GPIOs, dirección y alto del panel) en
`idf.py menuconfig` → **Reloj 8 bits + Clima (BLE)**.

## Sincronizar desde el celular

`webapp/index.html` es una página con Web Bluetooth: lee la hora del teléfono,
pide el clima de tu ubicación a [Open-Meteo](https://open-meteo.com) y lo escribe
por BLE. No hay que instalar nada.

> **Requiere Chrome en Android.** Web Bluetooth no existe en Safari/iOS, y la
> página **debe servirse por HTTPS o desde localhost** — abrirla con `file://`
> no funciona, el navegador bloquea la API.

La forma más simple es publicarla en GitHub Pages (o cualquier hosting estático)
y abrir esa URL en el celular. Para probar desde la PC:

```bash
python -m http.server 8000 --directory webapp
```

y abrir `http://localhost:8000` en Chrome de escritorio.

## Protocolo BLE

Servicio `5c8b0001-7a2e-4f1d-9c3a-1b2d4e6f8a90`, dos características de escritura,
todo en little-endian:

| Característica | Bytes | Contenido |
|---|---|---|
| `…0002-…` hora | 8 | `uint32` epoch UTC · `int32` offset UTC en segundos |
| `…0003-…` clima | 6 | `int16` temperatura ×10 · `uint8` humedad % · `uint8` código WMO · `uint8` viento km/h · `uint8` dirección en grados÷2 |

Los dos bytes de viento son opcionales: una escritura de 4 bytes también se acepta
y simplemente no muestra el viento.

La hora del sistema se guarda en UTC y el offset se aplica al dibujar, así que no
hace falta configurar zona horaria en el firmware: el celular la trae puesta.

## Archivos

| Archivo | Qué hace |
|---------|----------|
| `main/main.c` | Arranque, las tres vistas, botón, ahorro de energía, estado compartido |
| `main/weather_icon.c` | Iconos de clima de 30x30 px y rosa de los vientos |
| `main/crab.c/.h` | Criatura pixel-art de 31x22: dos cuadros de caminata y dos de saludo |
| `main/ble_sync.c/.h` | Servidor GATT con NimBLE (anuncio, conexión, escrituras) |
| `main/ssd1306.c/.h` | Driver SSD1306 sobre `driver/i2c_master`, framebuffer y primitivas |
| `main/font5x7.h` | Fuente pixel 5x7 (ASCII 0x20–0x5F, `` ` `` = grado), escalable |
| `main/weather.c/.h` | Códigos WMO a texto corto |
| `webapp/index.html` | Página de sincronización (Web Bluetooth) |
