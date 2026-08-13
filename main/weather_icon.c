/* Iconos de clima de 30x30 px dibujados con primitivas, estilo pixel-art. */

#include "weather.h"

#define ICON_SIZE 30

/* Sol: disco central con ocho rayos. */
static void sun(ssd1306_t *d, int cx, int cy, int r, int r1, int r2)
{
    static const int8_t dir[8][2] = {
        {10, 0}, {7, 7}, {0, 10}, {-7, 7}, {-10, 0}, {-7, -7}, {0, -10}, {7, -7},
    };
    ssd1306_disc(d, cx, cy, r, true);
    for (int i = 0; i < 8; i++) {
        int x1 = cx + dir[i][0] * r1 / 10, y1 = cy + dir[i][1] * r1 / 10;
        int x2 = cx + dir[i][0] * r2 / 10, y2 = cy + dir[i][1] * r2 / 10;
        ssd1306_line(d, x1, y1, x2, y2, true);
    }
}

/* Luna: disco lleno al que se le recorta otro disco desplazado. */
static void moon(ssd1306_t *d, int cx, int cy, int r)
{
    ssd1306_disc(d, cx, cy, r, true);
    ssd1306_disc(d, cx - r / 2 - 2, cy - r / 3, r, false);
}

/*
 * Nube maciza. (x, y) = esquina superior izquierda de la caja del icono.
 * 'grow' engorda la silueta: sirve para borrar un contorno alrededor de la nube
 * y que se despegue visualmente de lo que haya debajo (el sol o la luna).
 */
static void cloud_ex(ssd1306_t *d, int x, int y, int grow, bool on)
{
    ssd1306_disc(d, x + 9, y + 10, 6 + grow, on);
    ssd1306_disc(d, x + 17, y + 7, 7 + grow, on);
    ssd1306_disc(d, x + 23, y + 10, 5 + grow, on);
    ssd1306_fill_rect(d, x + 9, y + 10 - grow, 15, 6 + 2 * grow, on);
}

static void cloud(ssd1306_t *d, int x, int y)
{
    cloud_ex(d, x, y, 0, true);
}

/* Nube que tapa al sol/luna dejando 2 px de separacion. */
static void cloud_over(ssd1306_t *d, int x, int y)
{
    cloud_ex(d, x, y, 2, false);
    cloud_ex(d, x, y, 0, true);
}

/* Tres gotas inclinadas bajo la nube. */
static void rain(ssd1306_t *d, int x, int y, int n)
{
    for (int i = 0; i < n; i++) {
        int dx = x + 8 + i * 6;
        ssd1306_line(d, dx + 2, y, dx - 1, y + 5, true);
    }
}

/* Copos: cruces de 3x3. */
static void snow(ssd1306_t *d, int x, int y, int n)
{
    for (int i = 0; i < n; i++) {
        int cx = x + 9 + i * 6, cy = y + 2;
        ssd1306_line(d, cx - 2, cy, cx + 2, cy, true);
        ssd1306_line(d, cx, cy - 2, cx, cy + 2, true);
        ssd1306_pixel(d, cx - 1, cy - 1, true);
        ssd1306_pixel(d, cx + 1, cy + 1, true);
    }
}

/* Rayo en zigzag. */
static void bolt(ssd1306_t *d, int x, int y)
{
    ssd1306_line(d, x + 17, y, x + 12, y + 5, true);
    ssd1306_line(d, x + 12, y + 5, x + 16, y + 5, true);
    ssd1306_line(d, x + 16, y + 5, x + 11, y + 10, true);
    ssd1306_line(d, x + 16, y, x + 11, y + 5, true);
}

void weather_draw_icon(ssd1306_t *d, int x, int y, int wmo_code, bool night)
{
    switch (wmo_code) {
    case 0:   /* despejado */
        if (night) {
            moon(d, x + 15, y + 15, 10);
        } else {
            sun(d, x + 15, y + 15, 6, 9, 14);
        }
        break;

    case 1:   /* mayormente despejado */
    case 2:   /* parcialmente nublado */
        if (night) {
            moon(d, x + 20, y + 9, 7);
        } else {
            sun(d, x + 20, y + 9, 4, 6, 9);
        }
        cloud_over(d, x - 2, y + 13);
        break;

    case 3:   /* nublado */
        cloud(d, x, y + 7);
        break;

    case 45:  /* niebla */
    case 48:
        cloud(d, x, y + 2);
        for (int i = 0; i < 3; i++) {
            ssd1306_line(d, x + 4 + (i % 2) * 3, y + 21 + i * 3,
                         x + 25 - (i % 2) * 3, y + 21 + i * 3, true);
        }
        break;

    case 51:  /* llovizna */
    case 53:
    case 55:
    case 56:
    case 57:
        cloud(d, x, y + 2);
        rain(d, x, y + 21, 2);
        break;

    case 61:  /* lluvia */
    case 63:
    case 66:
    case 67:
    case 80:
    case 81:
        cloud(d, x, y + 2);
        rain(d, x, y + 21, 3);
        break;

    case 65:  /* lluvia fuerte / aguacero */
    case 82:
        cloud(d, x, y);
        rain(d, x, y + 19, 3);
        rain(d, x, y + 24, 3);
        break;

    case 71:  /* nieve */
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        cloud(d, x, y + 2);
        snow(d, x, y + 22, 3);
        break;

    case 95:  /* tormenta electrica */
    case 96:
    case 99:
        cloud(d, x, y + 2);
        bolt(d, x, y + 19);
        rain(d, x + 8, y + 21, 1);
        break;

    default:  /* sin dato: signo de interrogacion grande */
        ssd1306_text(d, x + 9, y + 9, "?", 2, true);
        break;
    }
}

/* --------------------------------------------------- iconos de 13x13 px */

/*
 * Version chica, para el pronostico por horas. No se puede escalar la grande:
 * son 30x30 y en 128 px no caben seis. A este tamaño tampoco vale reducir los
 * mismos trazos, porque el sol pierde los rayos y la nube se convierte en una
 * mancha; hay que redibujarlos pensados para trece pixeles.
 */
#define MINI_LADO 13

static const char *const IC_SOL[MINI_LADO] = {
    ".............",
    "......#......",
    "..#.......#..",
    "....#####....",
    "...#######...",
    "...#######...",
    "#..#######..#",
    "...#######...",
    "...#######...",
    "....#####....",
    "..#.......#..",
    "......#......",
    ".............",
};

static const char *const IC_LUNA[MINI_LADO] = {
    ".............",
    "...#####.....",
    "..###..##....",
    ".####...#....",
    ".#####.......",
    "######.......",
    "######.......",
    "######.......",
    ".#####.......",
    ".####...#....",
    "..###..##....",
    "...#####.....",
    ".............",
};

/* Sol chico con sus rayos asomando por encima de la nube: a este tamaño es lo
   unico que distingue "parcialmente nublado" de "nublado". */
static const char *const IC_PARCIAL[MINI_LADO] = {
    ".............",
    ".......#...#.",
    "........###..",
    "......#.###.#",
    "........###..",
    ".......#...#.",
    "....#####....",
    "..##.....##..",
    ".##.......##.",
    "#...........#",
    "#############",
    ".............",
    ".............",
};

static const char *const IC_NUBE[MINI_LADO] = {
    ".............",
    ".............",
    ".............",
    "....#####....",
    "..##.....##..",
    ".##.......##.",
    "#...........#",
    "#...........#",
    "#############",
    ".............",
    ".............",
    ".............",
    ".............",
};

static const char *const IC_NIEBLA[MINI_LADO] = {
    ".............",
    "....#####....",
    "..##.....##..",
    ".##.......##.",
    "#...........#",
    "#############",
    ".............",
    ".###.###.###.",
    ".............",
    "###.###.###..",
    ".............",
    ".###.###.###.",
    ".............",
};

static const char *const IC_LLUVIA[MINI_LADO] = {
    ".............",
    "....#####....",
    "..##.....##..",
    ".##.......##.",
    "#...........#",
    "#############",
    ".............",
    "..#...#...#..",
    ".#...#...#...",
    ".............",
    "..#...#...#..",
    ".#...#...#...",
    ".............",
};

static const char *const IC_NIEVE[MINI_LADO] = {
    ".............",
    "....#####....",
    "..##.....##..",
    ".##.......##.",
    "#...........#",
    "#############",
    ".............",
    "..#.#...#.#..",
    "...#.....#...",
    "..#.#...#.#..",
    ".............",
    ".....#.#.....",
    "......#......",
};

static const char *const IC_TORMENTA[MINI_LADO] = {
    ".............",
    "....#####....",
    "..##.....##..",
    ".##.......##.",
    "#...........#",
    "#############",
    "......###....",
    ".....###.....",
    "....###......",
    "...#######...",
    ".....###.....",
    "....###......",
    "...##........",
};

static void estampar_mini(ssd1306_t *d, int x, int y, const char *const *filas)
{
    for (int r = 0; r < MINI_LADO; r++) {
        for (int c = 0; c < MINI_LADO; c++) {
            if (filas[r][c] == '#') {
                ssd1306_pixel(d, x + c, y + r, true);
            }
        }
    }
}

void weather_draw_icon_mini(ssd1306_t *d, int x, int y, int wmo_code, bool night)
{
    const char *const *ic;

    switch (wmo_code) {
    case 0:
        ic = night ? IC_LUNA : IC_SOL;
        break;
    case 1: case 2:
        ic = IC_PARCIAL;
        break;
    case 3:
        ic = IC_NUBE;
        break;
    case 45: case 48:
        ic = IC_NIEBLA;
        break;
    case 71: case 73: case 75: case 77: case 85: case 86:
        ic = IC_NIEVE;
        break;
    case 95: case 96: case 99:
        ic = IC_TORMENTA;
        break;
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82:
        ic = IC_LLUVIA;
        break;
    default:
        /* Codigo que no conocemos: mejor una nube que un hueco, que en una fila
           de seis columnas se leeria como que falta el dato. */
        ic = IC_NUBE;
        break;
    }
    estampar_mini(d, x, y, ic);
}

/* Rosa de los vientos de 16 puntos, en abreviaturas en espanol. */
const char *weather_wind_dir(int degrees)
{
    static const char *pts[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSO", "SO", "OSO", "O", "ONO", "NO", "NNO",
    };
    if (degrees < 0) {
        return "";
    }
    int deg = ((degrees % 360) + 360) % 360;
    return pts[((deg * 100 + 1125) / 2250) % 16];   /* redondeo a sectores de 22.5 grados */
}
