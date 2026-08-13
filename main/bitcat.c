/*
 * BitCat: mascota y logo del reloj. Sprite de 31x24 en cinco poses.
 *
 * La cara es una pantalla apagada dentro de la cabeza y los ojos se estampan
 * encima como bloques de 4x4, asi que cualquier pose combina con cualquier
 * expresion sin duplicar el cuerpo.
 *
 * El arte se genera con tools/gen_bitcat.py, que calcula los tramos de
 * cada fila en vez de contarlos a mano.
 */

#include "bitcat.h"


/* Posicion de los ojos dentro del sprite. */
#define EYE_X_L 10
#define EYE_X_R 17
#define EYE_Y    6

static const char *BITCAT[BITCAT_POSES][BITCAT_H] = {
    [BITCAT_SIT] = {
        ".......##.............##.......",
        ".......###...........###.......",
        ".......####.........####.......",
        "......###################......",
        "......###################......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......###################......",
        "......###################......",
        "....##....###########..........",
        "....##....##.......##..........",
        "....##...#############.........",
        ".....##..#############.........",
        "......##.#############.........",
        ".......###############.........",
        ".........#############.........",
        ".........####.....####.........",
        ".........####.....####.........",
    },
    [BITCAT_WALK_A] = {
        ".......##.............##.......",
        ".......###...........###.......",
        ".......####.........####.......",
        "......###################......",
        "......###################......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......###################......",
        "......###################......",
        "....##....###########..........",
        "....##....##.......##..........",
        "....##...#############.........",
        ".....##..#############.........",
        "......##.#############.........",
        ".......###############.........",
        ".........#############.........",
        ".........####....####..........",
        ".........####....####..........",
    },
    [BITCAT_WALK_B] = {
        ".......##.............##.......",
        ".......###...........###.......",
        ".......####.........####.......",
        "......###################......",
        "......###################......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "......##...............##......",
        "...##.###################......",
        "...##.###################......",
        "...##.....###########..........",
        "....##....##.......##..........",
        ".....##..#############.........",
        "......##.#############.........",
        ".......###############.........",
        ".........#############.........",
        ".........#############.........",
        "..........####....####.........",
        "..........####....####.........",
    },
    [BITCAT_WAVE_A] = {
        ".......##.............##.......",
        ".......###...........###.......",
        ".......####.........####.......",
        "......###################.#####",
        "......###################.#####",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "...##.###################..###.",
        "...##.###################..###.",
        "...##.....###########......###.",
        "....##....##.......###########.",
        ".....##..#############.........",
        "......##.#############.........",
        ".......###############.........",
        ".........#############.........",
        ".........#############.........",
        ".........####.....####.........",
        ".........####.....####.........",
    },
    [BITCAT_WAVE_B] = {
        ".......##.............##.......",
        ".......###...........###.......",
        ".......####.........####.......",
        "......###################......",
        "......###################......",
        "......##...............##......",
        "......##...............##.#####",
        "......##...............##.#####",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "......##...............##..###.",
        "...##.###################..###.",
        "...##.###################..###.",
        "...##.....###########......###.",
        "....##....##.......###########.",
        ".....##..#############.........",
        "......##.#############.........",
        ".......###############.........",
        ".........#############.........",
        ".........#############.........",
        ".........####.....####.........",
        ".........####.....####.........",
    },
};

static const char *OJOS[BITCAT_EXPR_COUNT][2][4] = {
    [BITCAT_NORMAL] = {
        {"####", "####", "####", "####"},
        {"####", "####", "####", "####"},
    },
    [BITCAT_FELIZ] = {
        {"....", ".##.", "#..#", "...."},
        {"....", ".##.", "#..#", "...."},
    },
    [BITCAT_SORPRESA] = {
        {"....", ".##.", ".##.", "...."},
        {"....", ".##.", ".##.", "...."},
    },
    [BITCAT_DORMIDO] = {
        {"....", "....", "####", "...."},
        {"....", "....", "####", "...."},
    },
    [BITCAT_ENOJADO] = {
        {"#...", ".#..", "..#.", "...#"},
        {"...#", "..#.", ".#..", "#..."},
    },
    [BITCAT_AMOR] = {
        {"#..#", "####", ".##.", "...."},
        {"#..#", "####", ".##.", "...."},
    },
};


/* --------------------------------------------------------------- accesorios */

/* Estampa un bloquecito de arte relativo al origen del sprite. Recorre cada fila
   hasta el NUL, asi que no hay que declarar el ancho por separado. */
static void estampar(ssd1306_t *d, int x, int y, const char *const *filas, int n)
{
    for (int r = 0; r < n; r++) {
        for (int c = 0; filas[r][c] != '\0'; c++) {
            if (filas[r][c] == '#') {
                ssd1306_pixel(d, x + c, y + r, true);
            }
        }
    }
}

/*
 * Sombrilla, centrada sobre la patita levantada de BITCAT_WAVE_A (columna 28).
 *
 * Se dibuja por encima de la caja del sprite, no dentro. Metida dentro solo
 * caben tres filas justo encima de la patita, y a ese tamaño se pega al brazo y
 * se lee como un bulto, no como una sombrilla. En la vista del gato la franja de
 * arriba solo la ocupan la hora (a la izquierda) y la temperatura (a la
 * derecha), asi que el centro esta libre y ahi cabe entera.
 *
 * Tres cosas la hacen reconocible a este tamaño, y ninguna sobra: el remate de
 * arriba, el arco —que sin el es un triangulo— y el borde dentado, que son las
 * varillas y es lo que la separa de una loma. El centro del dentado va macizo
 * porque por ahi entra el mango, y un hueco justo ahi lo dejaria suelto.
 */
static const char *const ACC_PARAGUAS[6] = {
    "............#............",
    ".........#######.........",
    "......#############......",
    "...###################...",
    "#########################",
    ".###.###.#######.###.###.",
};

/* Del toldo a la patita. El brazo del gato sigue debajo y hace de continuacion. */
static const char *const ACC_MANGO[5] = {"#", "#", "#", "#", "#"};

/* Lentes: cubren por completo los ojos de 4x4, asi que funcionan con cualquier
   expresion sin tener que forzar la de abajo. */
static const char *const ACC_LENTES[4] = {
    "#####..#####",
    "############",
    ".####..####.",
    "..###..###..",
};

/* Tiritar: rayitas de movimiento a los costados, fuera de la caja del sprite. */
static const char *const ACC_RAYAS[7] = {
    "###",
    "",
    "",
    "###",
    "",
    "",
    "###",
};

static const char *const ACC_Z[3] = {
    "###",
    ".#.",
    "###",
};

static void dibujar_accesorio(ssd1306_t *d, int x, int y, bitcat_acc_t acc)
{
    switch (acc) {
    case BITCAT_ACC_PARAGUAS:
        estampar(d, x + 16, y - 8, ACC_PARAGUAS, 6);
        estampar(d, x + 28, y - 2, ACC_MANGO, 5);
        break;
    case BITCAT_ACC_LENTES:
        estampar(d, x + 9, y + 6, ACC_LENTES, 4);
        break;
    case BITCAT_ACC_FRIO:
        /* A la altura de la cabeza (filas 6, 9 y 12), en las columnas que deja
           libres a cada lado. Mas abajo se pegarian a la cola y mas afuera se
           leerian como suciedad en vez de como movimiento. */
        estampar(d, x + 1, y + 6, ACC_RAYAS, 7);
        estampar(d, x + 27, y + 6, ACC_RAYAS, 7);
        break;
    case BITCAT_ACC_ZZZ:
        estampar(d, x + 26, y + 4, ACC_Z, 3);
        estampar(d, x + 28, y + 0, ACC_Z, 3);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------- dibujo */

void bitcat_draw_acc(ssd1306_t *d, int x, int y, bitcat_pose_t pose,
                     bitcat_expr_t expr, bitcat_acc_t acc)
{
    bitcat_draw(d, x, y, pose, expr);
    dibujar_accesorio(d, x, y, acc % BITCAT_ACC_COUNT);
}

void bitcat_draw(ssd1306_t *d, int x, int y, bitcat_pose_t pose, bitcat_expr_t expr)
{
    const char **filas = BITCAT[pose % BITCAT_POSES];
    for (int r = 0; r < BITCAT_H; r++) {
        for (int c = 0; c < BITCAT_W; c++) {
            if (filas[r][c] == '#') {
                ssd1306_pixel(d, x + c, y + r, true);
            }
        }
    }

    /* Ojos: 4x4 cada uno, dentro de la pantalla de la cara. */
    const char *(*ojos)[4] = OJOS[expr % BITCAT_EXPR_COUNT];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (ojos[0][r][c] == '#') {
                ssd1306_pixel(d, x + EYE_X_L + c, y + EYE_Y + r, true);
            }
            if (ojos[1][r][c] == '#') {
                ssd1306_pixel(d, x + EYE_X_R + c, y + EYE_Y + r, true);
            }
        }
    }
}

const char *bitcat_expr_nombre(bitcat_expr_t expr)
{
    static const char *n[BITCAT_EXPR_COUNT] = {
        "normal", "feliz", "sorpresa", "dormido", "enojado", "amor",
    };
    return n[expr % BITCAT_EXPR_COUNT];
}

const char *bitcat_acc_nombre(bitcat_acc_t acc)
{
    static const char *n[BITCAT_ACC_COUNT] = {
        "ninguno", "paraguas", "lentes", "frio", "zzz",
    };
    return n[acc % BITCAT_ACC_COUNT];
}
