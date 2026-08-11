# BitCat Watch — reloj "8 bits" + clima · ESP32-S3 (Seeed Studio) + OLED 0.91" I2C

Reloj con estética de 8 bits en un SSD1306 de 0.91" (128x32). **La hora y el clima
llegan por Bluetooth LE desde el celular: no usa WiFi ni credenciales de red.**

La mascota es **BitCat**. Cinco vistas que rotan con el **botón BOOT** (GPIO 0):

```
RELOJ                      CLIMA                    24H              BITCAT           PASEO
      HH:MM                (icono)  22`C      24H      12`/28`   15:12      22`C   (BitCat cruza
 [#][ ][#][#][ ][ ][#][ ]   30x30   64% 18KMH   ▄▄█▀█▄▄               .-.          la pantalla
      JUE 07 AGO             px     PARC NUBLADO ███████             (o.o)         y a veces
                                                                     BitCat         saluda)
```

- **Segundos en 8 bits**: fila de 8 casillas, MSB a la izquierda; llena = 1, hueca = 0.
- **Vista 24H**: una barra por hora con la temperatura del último día. Las horas
  sin dato salen como un punto en el piso. Se archiva una muestra por hora cada
  vez que llega clima del celular.
- **Vista BitCat**: el gato al centro con la hora arriba a la izquierda y el clima
  arriba a la derecha. Su expresión cambia sola cada 15 minutos, al azar entre
  normal, feliz, sorpresa, dormido, enojado y amor — salvo que el clima mande:

  | Condición | BitCat |
  |---|---|
  | De 23:00 a 06:00 | dormido, con zetas |
  | Precipitación (llovizna, lluvia, nieve, tormenta) | saca la sombrilla |
  | 0 °C o menos | tirita, con cara de enojado |
  | Despejado y 25 °C o más | lentes oscuros y cara feliz |

## Un botón, dos acciones

| Vista | Pulsación corta | Pulsación larga (0.7 s) |
|---|---|---|
| Reloj · Clima · 24H · BitCat | siguiente vista | apagar la pantalla |
| Paseo | siguiente vista | **entrar al juego** |
| Juego | saltar | salir al paseo |

Con la pantalla apagada, el primer toque solo la enciende.

El **juego** es un endless runner: BitCat corre y hay que saltar los obstáculos.
La velocidad sube cada 10 puntos y el récord se guarda hasta el próximo reinicio.
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

> **La primera vez tras añadir el OTA hace falta borrar la flash entera.** El
> proyecto pasó de una sola partición de app a dos, así que el mapa viejo ya no
> coincide y el bootloader no arrancaría:
>
> ```bash
> idf.py -p COM3 erase-flash flash monitor
> ```
>
> Una sola vez. Después basta con `flash`, o directamente el OTA por BLE.

Ajustes opcionales (nombre BLE, GPIOs, dirección y alto del panel) en
`idf.py menuconfig` → **BitCat Watch (reloj 8 bits + clima por BLE)**.

El reloj se anuncia como `BITCAT WATCH v1 A3F2`, donde el sufijo son los dos
últimos bytes de su MAC. Es lo que permite distinguir varias unidades en el
selector del celular sin configurar nada por aparato: el UUID del servicio
identifica al modelo, no al reloj concreto.

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

`.github/workflows/pages.yml` compila el firmware y sube la carpeta `webapp/` —ya
con el `.bin` dentro— a GitHub Pages en cada push a `main`. Solo hay que activarlo
una vez en **Settings → Pages → Source: GitHub Actions**. Después queda en:

```
https://alekey01.github.io/oled_i2c/
```

Esa URL es HTTPS, así que se abre desde el celular sin la laptop de por medio.

### Instalarla como app

La página es una PWA: al abrirla en Chrome de Android aparece el botón
**Instalar app**, y queda con su ícono de BitCat, sin barra de direcciones.

El service worker cachea el casco (HTML e iconos), así que **abre sin red**. El
enlace BLE no usa internet: poner el reloj en hora funciona sin datos. Lo único
que necesita conexión es el clima, porque sale a Open-Meteo — si no hay red la
consola lo avisa y la hora se envía igual.

Al publicar una versión nueva de `webapp/`, sube el número de `CACHE` en
`webapp/sw.js`; ese cambio de nombre es lo que dispara el borrado del cache
viejo. Los iconos se generan del mismo sprite del firmware:

```bash
python tools/gen_icons.py
```

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

## Actualizar el firmware por Bluetooth

En la página, sección **FIRMWARE**, botón **Actualizar**. No hace falta cable, ni
abrir la caja, ni tener el `.bin` en el celular.

El workflow de GitHub Pages **compila el firmware** con `esp-idf-ci-action` y
publica `oled_i2c.bin` junto a la página, con un `firmware.json` que lleva
tamaño, sha256, commit y fecha. La página lo pide a su propio origen —sin CORS de
por medio—, comprueba el hash y lo manda por BLE. Lo que ofrece siempre es el
último commit de `main`.

`webapp/firmware/` lo genera el CI y está en `.gitignore`: el binario no vive en
el repositorio.

La página lee la versión del reloj por BLE y la compara con la publicada. Si
coinciden, el botón dice *Ya está al día* y no deja mandar nada.

Cómo funciona:

1. La imagen se escribe siempre en la ranura que **no** se está ejecutando.
   Un corte a media transferencia no deja el reloj sin firmware.
2. Durante el envío se pide un intervalo de conexión de 15–30 ms. Con los 300 ms
   del funcionamiento normal, medio mega tardaría horas.
3. Los datos van **sin respuesta**, que es lo que lo hace rápido. Al final el
   reloj compara los bytes recibidos con los anunciados: si falta alguno, la
   actualización se rechaza y no se toca la partición de arranque.
4. Tras arrancar la versión nueva, el firmware espera 30 s antes de confirmarse.
   Si se cuelga antes, el bootloader vuelve solo a la anterior en el siguiente
   reset (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

Mientras dura, el OLED muestra una barra de progreso en lugar de la vista.

## OTA cifrado

**El `.bin` publicado no es legible.** Va cifrado con
[`esp_encrypted_img`](https://components.espressif.com/components/espressif/esp_encrypted_img):
una clave AES-GCM aleatoria por imagen, envuelta con **RSA-3072**. La clave
privada está incrustada en el firmware del reloj y no viaja nunca — ni por el
aire, ni por la página. Descargar el archivo de GitHub Pages no sirve de nada.

Conviene decirlo claro: **cifrar y descifrar en el navegador no habría servido de
nada.** La clave tendría que estar en el JavaScript, que es público. Solo protege
si el único que puede descifrar es el reloj.

También conviene decir lo que **no** protege: si el repositorio es público, el
código fuente ya lo es, y cualquiera puede compilar su propio binario. Esto evita
que se distribuya *tu* imagen, no que se reconstruya.

### Generar el par de claves

Una vez, y guárdalo bien: **si pierdes la clave privada, los relojes ya
flasheados no aceptarán ninguna actualización más.**

```bash
python managed_components/espressif__esp_encrypted_img/tools/esp_enc_img_gen.py --generate_rsa_key
```

Deja `rsa_priv_key.pem` y `rsa_pub_key.pem` en la raíz. Colócalos así:

| Archivo | Dónde va | En git |
|---|---|---|
| `main/ota_private_key.pem` | se incrusta en el firmware al compilar | **no**, está en `.gitignore` |
| `ota_public_key.pem` | lo usa el CI para cifrar | sí, es pública por diseño |

Y añade el contenido de la privada como secreto **`OTA_PRIVATE_KEY`** en
*Settings → Secrets and variables → Actions*. El workflow la escribe en disco
para compilar y la borra al terminar, pase lo que pase.

Sin ese archivo la compilación falla — es deliberado: es preferible a producir un
firmware que no pueda actualizarse nunca.

## Versiones

| Qué | Dónde se sube |
|---|---|
| Firmware | `version.txt` en la raíz. ESP-IDF lo mete en el descriptor de la app |
| Página | `"version"` en `webapp/manifest.json` y `CACHE` en `webapp/sw.js` |

La del firmware sale en la pantalla de arranque (`V1.0.0  app0`), se lee por BLE
y viaja en `firmware.json`. La de la página se muestra al pie.

Los dos sitios de la página van a la par a propósito: `CACHE` es lo que invalida
el service worker, y si no lo subes la app instalada seguirá sirviendo la
anterior.

**Cómo comprobar que entró:** la pantalla de arranque muestra la ranura activa y
la hora de compilación (`app0  15:23:11`). Tras un OTA correcto tiene que haber
cambiado de `app0` a `app1`, o al revés. Va en el OLED y no en el log porque con
light sleep la consola USB no es fiable.

**No apagues el reloj durante los primeros 30 segundos** tras actualizar. Es la
ventana en la que el firmware nuevo todavía no se ha confirmado: un corte ahí
hace que el bootloader vuelva a la versión anterior, que es exactamente para lo
que está el rollback.

Si usas un archivo propio, es `build/oled_i2c.bin`, no `bootloader.bin` ni
`partition-table.bin`. Esos dos no cambian y el OTA no los toca.

## Protocolo BLE

Servicio `5c8b0001-7a2e-4f1d-9c3a-1b2d4e6f8a90`, todo en little-endian:

| Característica | Modo | Bytes | Contenido |
|---|---|---|---|
| `…0002-…` hora | escritura | 8 | `uint32` epoch UTC · `int32` offset UTC en segundos |
| `…0003-…` clima | escritura | 6 | `int16` temperatura ×10 · `uint8` humedad % · `uint8` código WMO · `uint8` viento km/h · `uint8` dirección en grados÷2 |
| `…0004-…` control OTA | escritura + notificación | 1 o 5 | orden: `01`+`uint32` tamaño = iniciar, `02` = terminar, `03` = cancelar |
| `…0005-…` datos OTA | escritura sin respuesta | ≤ 512 | trozo de la imagen cifrada |
| `…0006-…` info | lectura | texto | versión y ranura activa, p. ej. `1.0.0 app0` |

Los dos bytes de viento son opcionales: una escritura de 4 bytes también se acepta
y simplemente no muestra el viento.

El control del OTA responde por notificación con 6 bytes: `uint8` estado
(`01` listo, `02` avance, `03` terminado, `04` error), `uint32` bytes recibidos y
`uint8` código de error. El avance se manda cada 16 KB, no por paquete: notificar
cada trozo saturaría el enlace justo cuando se está usando al máximo.

La hora del sistema se guarda en UTC y el offset se aplica al dibujar, así que no
hace falta configurar zona horaria en el firmware: el celular la trae puesta.

## Archivos

| Archivo | Qué hace |
|---------|----------|
| `main/main.c` | Arranque, las cinco vistas y el juego, botón, ahorro de energía, estado compartido |
| `main/weather_icon.c` | Iconos de clima de 30x30 px y rosa de los vientos |
| `main/bitcat.c/.h` | BitCat 31x24: cinco poses, seis expresiones y cuatro accesorios, todos combinables |
| `main/ble_sync.c/.h` | Servidor GATT con NimBLE (anuncio, conexión, escrituras, transporte del OTA) |
| `main/ota.c/.h` | Descifrado de la imagen, escritura en la ranura libre y cambio de arranque |
| `partitions.csv` | Dos ranuras de app de 2 MB, necesarias para el OTA |
| `version.txt` | Versión del firmware |
| `ota_public_key.pem` | Clave pública con la que el CI cifra la imagen |
| `main/ssd1306.c/.h` | Driver SSD1306 sobre `driver/i2c_master`, framebuffer y primitivas |
| `main/font5x7.h` | Fuente pixel 5x7 (ASCII 0x20–0x5F, `` ` `` = grado), escalable |
| `main/weather.c/.h` | Códigos WMO a texto corto |
| `webapp/index.html` | Página de sincronización (Web Bluetooth) |
| `webapp/manifest.json` | Manifiesto de la PWA (nombre, iconos, modo standalone) |
| `webapp/sw.js` | Service worker: cachea el casco para que abra sin red |
| `tools/gen_bitcat.py` | Genera el arte de BitCat. `--c` emite las tablas de `bitcat.c`, `--js` las de la página; sin argumentos imprime una vista previa |
| `tools/gen_icons.py` | Genera los PNG de la PWA desde el mismo sprite, sin dependencias |
