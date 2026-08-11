# Reloj "8 bits" + Clima — ESP32-S3 (Seeed Studio) + OLED 0.91" I2C

Reloj con estética de 8 bits en un SSD1306 de 0.91" (128x32). **La hora y el clima
llegan por Bluetooth LE desde el celular: no usa WiFi ni credenciales de red.**

La mascota es **BitCat**. Cuatro vistas que rotan con el **botón BOOT** (GPIO 0):

```
RELOJ                      CLIMA                     BITCAT              PASEO
      HH:MM                (icono)  22`C        15:12      22`C      (BitCat cruza
 [#][ ][#][#][ ][ ][#][ ]   30x30   64% 18KMH             .-.         la pantalla
      JUE 07 AGO             px     PARC NUBLADO         (o.o)        y a veces
                                                          BitCat       saluda)
```

- **Segundos en 8 bits**: fila de 8 casillas, MSB a la izquierda; llena = 1, hueca = 0.
- **Vista BitCat**: el gato al centro con la hora arriba a la izquierda y el clima
  arriba a la derecha. Su expresión cambia sola cada 15 minutos, al azar entre
  normal, feliz, sorpresa, dormido, enojado y amor.
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
| Apagado del OLED por inactividad | Desactivado por defecto (`APP_SCREEN_TIMEOUT_S=0`). Ponle segundos para activarlo |
| Anuncio BLE a 500–1000 ms | Por defecto va a ~30 ms |
| Conexión BLE a 300 ms con latencia 4 | El radio despierta como mucho cada 1.5 s |

El controlador BLE del ESP32-S3 usa el **cristal principal** como reloj de baja
potencia (`CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL`), así que el light sleep funciona
sin cristal externo de 32 kHz. A cambio, ese cristal sigue alimentado mientras
duerme, así que el ahorro es bueno pero no tan profundo como con un 32 kHz.

> Con el light sleep activo la consola USB no es confiable. Para depurar,
> desactiva `APP_LIGHT_SLEEP` en `menuconfig`.

En la vista del paseo el refresco sube a 80 ms: es la vista más cara en CPU y
en bus I2C.

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

El indicador de estado es el mismo BitCat del firmware, dibujado en un canvas
con los sprites idénticos a `main/bitcat.c`: camina mientras el enlace trabaja y
saluda con la cara feliz cuando está conectado. Abajo hay una consola con el
detalle de cada operación.

> **Requiere Chrome en Android.** Web Bluetooth no existe en Safari/iOS, y la
> página **debe servirse por HTTPS o desde localhost** — abrirla con `file://`
> no funciona, el navegador bloquea la API.

### Publicarla (recomendado)

`.github/workflows/pages.yml` sube la carpeta `webapp/` a GitHub Pages en cada
push a `main`. Solo hay que activarlo una vez en **Settings → Pages → Source:
GitHub Actions**. Después queda en:

```
https://alekey01.github.io/oled_i2c/
```

Esa URL es HTTPS, así que se abre desde el celular sin la laptop de por medio.
Conviene agregarla a la pantalla de inicio de Chrome para tenerla a un toque.

### Probar en local

Desde la PC:

```bash
python -m http.server 8000 --directory webapp
```

y abrir `http://localhost:8000` en Chrome de escritorio.

Desde el celular sin publicar nada, con el cable USB y depuración USB activa —
`adb reverse` hace que el `localhost` del teléfono apunte a la PC, y `localhost`
sí cuenta como origen seguro:

```bash
adb reverse tcp:8000 tcp:8000
```

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
| `main/main.c` | Arranque, las cuatro vistas, botón, ahorro de energía, estado compartido |
| `main/weather_icon.c` | Iconos de clima de 30x30 px y rosa de los vientos |
| `main/bitcat.c/.h` | BitCat 31x24: cinco poses y seis expresiones combinables |
| `main/ble_sync.c/.h` | Servidor GATT con NimBLE (anuncio, conexión, escrituras) |
| `main/ssd1306.c/.h` | Driver SSD1306 sobre `driver/i2c_master`, framebuffer y primitivas |
| `main/font5x7.h` | Fuente pixel 5x7 (ASCII 0x20–0x5F, `` ` `` = grado), escalable |
| `main/weather.c/.h` | Códigos WMO a texto corto |
| `webapp/index.html` | Página de sincronización (Web Bluetooth) |
| `tools/gen_bitcat.py` | Genera el arte de BitCat. `--c` emite las tablas de `bitcat.c`, `--js` las de la página; sin argumentos imprime una vista previa |
