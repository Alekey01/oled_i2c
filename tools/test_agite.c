/*
 * Banco de pruebas del detector de gestos: misma logica que main.c, pero con
 * formas de onda inventadas en vez del sensor. Comprueba sobre todo que el
 * sentido que devuelve es el del primer empujon y no el del pico mas grande.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AGITE_MUESTRAS  40
#define AGITE_CRUCES    2
/*
 * 20 y no 40. El frenazo de un manotazo es mas fuerte que la arrancada —paras
 * en menos recorrido del que aceleras— asi que el primer empujon, que es el que
 * lleva el sentido, vale bastante menos que el recorrido total. Pidiendole el
 * 40% se lo saltaba y cogia el frenazo, o sea el sentido cambiado.
 */
#define AGITE_SENTIDO_PCT 20
#define AGITE_SENTIDO_MIN_MG 150
#define AGITE_HIST_PCT    15
#define AGITE_HIST_MIN_MG 150
#define UMBRAL_MG       900

static int mediana(const int16_t *v, int n)
{
    int16_t orden[AGITE_MUESTRAS];
    memcpy(orden, v, n * sizeof(orden[0]));
    for (int i = 1; i < n; i++) {
        int16_t x = orden[i];
        int j = i - 1;
        while (j >= 0 && orden[j] > x) { orden[j + 1] = orden[j]; j--; }
        orden[j + 1] = x;
    }
    return orden[n / 2];
}

/* Copia exacta de la parte de decision de detectar_agite(). */
static int decidir(const int16_t *m, int n, int *swing_out, int *cruces_out)
{
    int min = INT16_MAX, max = INT16_MIN;
    for (int i = 0; i < n; i++) {
        if (m[i] < min) min = m[i];
        if (m[i] > max) max = m[i];
    }
    int swing = max - min;
    int reposo = mediana(m, n);
    *swing_out = swing;

    int sentido = 0;
    int disparo = swing * AGITE_SENTIDO_PCT / 100;
    if (disparo < AGITE_SENTIDO_MIN_MG) disparo = AGITE_SENTIDO_MIN_MG;
    for (int i = 0; i < n; i++) {
        int d = m[i] - reposo;
        if (d > disparo || d < -disparo) { sentido = d > 0 ? 1 : -1; break; }
    }
    if (swing < UMBRAL_MG || sentido == 0) { *cruces_out = 0; return 0; }

    int margen = swing * AGITE_HIST_PCT / 100;
    if (margen < AGITE_HIST_MIN_MG) margen = AGITE_HIST_MIN_MG;
    int cruces = 0, lado = 0;
    for (int i = 0; i < n; i++) {
        int d = m[i] - reposo;
        int nuevo = d > margen ? 1 : (d < -margen ? -1 : 0);
        if (nuevo != 0 && lado != 0 && nuevo != lado) cruces++;
        if (nuevo != 0) lado = nuevo;
    }
    *cruces_out = cruces;
    return cruces >= AGITE_CRUCES ? sentido : 0;
}

/*
 * Manotazo realista. Reposo en 'base' (la gravedad del eje). Primero la
 * arrancada hacia 'dir', luego el frenazo al otro lado —mas fuerte, porque
 * paras en menos recorrido del que aceleras—, luego la vuelta y otra vez quieto.
 * Es el caso que enga�aria a un detector que se quedara con el pico mayor.
 */
static void gen_manotazo(int16_t *m, int n, int base, int dir, int fuerza)
{
    for (int i = 0; i < n; i++) m[i] = base;
    for (int i = 4;  i < 9;  i++) m[i] = base + dir * fuerza;           /* arrancada */
    for (int i = 9;  i < 15; i++) m[i] = base - dir * fuerza * 3 / 2;   /* frenazo */
    for (int i = 17; i < 22; i++) m[i] = base - dir * fuerza / 2;       /* vuelta */
    for (int i = 22; i < 26; i++) m[i] = base + dir * fuerza / 2;
}

/* Levantar el brazo: un solo desplazamiento y se queda ahi. No es gesto. */
static void gen_levantar(int16_t *m, int n, int base)
{
    for (int i = 0; i < n; i++) m[i] = (i < 8) ? base : base + 1000;
    for (int i = 8; i < 14; i++) m[i] = base + 1400;   /* el tiron de arrancar */
}

/* Andar: vaiven suave y continuo, por debajo del umbral. */
static void gen_andar(int16_t *m, int n, int base)
{
    for (int i = 0; i < n; i++) m[i] = base + ((i / 4) % 2 ? 220 : -220);
}

static int fallos;

static void probar(const char *nombre, const int16_t *m, int n, int esperado)
{
    int swing = 0, cruces = 0;
    int got = decidir(m, n, &swing, &cruces);
    bool ok = (got == esperado);
    if (!ok) fallos++;
    printf("%-38s sentido=%+d (esperado %+d) swing=%4d cruces=%d  %s\n",
           nombre, got, esperado, swing, cruces, ok ? "ok" : "FALLO");
}

int main(void)
{
    int16_t m[AGITE_MUESTRAS];
    const int n = AGITE_MUESTRAS;

    /* Reposo tipico segun como tengas el brazo: eje plano, de canto, invertido. */
    const int bases[] = {0, 1000, -1000, 700};

    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        char nom[64];
        gen_manotazo(m, n, bases[b], +1, 1200);
        snprintf(nom, sizeof(nom), "manotazo derecha (reposo %+d)", bases[b]);
        probar(nom, m, n, +1);

        gen_manotazo(m, n, bases[b], -1, 1200);
        snprintf(nom, sizeof(nom), "manotazo izquierda (reposo %+d)", bases[b]);
        probar(nom, m, n, -1);
    }

    gen_manotazo(m, n, 1000, +1, 300);
    probar("manotazo flojo (por debajo del umbral)", m, n, 0);

    gen_levantar(m, n, 0);
    probar("levantar el brazo", m, n, 0);

    /*
     * Peor caso: la ventana arranca con el gesto ya empezado, porque entre que
     * salta la interrupcion y se puede leer pasan unas decenas de ms. Se recorta
     * el principio y se rellena el final con reposo, que es lo que de verdad
     * pasa; rotar la se�al haria trampa, porque devolveria al final unas
     * muestras del reposo anterior que el sensor ya no va a ver.
     */
    int16_t tarde[AGITE_MUESTRAS];
    int corte;

    gen_levantar(m, n, 0);
    corte = 9;   /* se pierde toda la parte de antes de levantar */
    for (int i = 0; i < n; i++) tarde[i] = (i + corte < n) ? m[i + corte] : m[n - 1];
    probar("levantar el brazo, empezando tarde", tarde, n, 0);

    gen_manotazo(m, n, 1000, +1, 1200);
    corte = 5;   /* se pierde parte de la arrancada, pero no toda */
    for (int i = 0; i < n; i++) tarde[i] = (i + corte < n) ? m[i + corte] : 1000;
    probar("manotazo derecha, empezando tarde", tarde, n, +1);

    gen_manotazo(m, n, 1000, -1, 1200);
    for (int i = 0; i < n; i++) tarde[i] = (i + corte < n) ? m[i + corte] : 1000;
    probar("manotazo izquierda, empezando tarde", tarde, n, -1);

    /* Con ruido encima, que el sensor no da valores limpios. */
    gen_manotazo(m, n, 1000, -1, 1200);
    for (int i = 0; i < n; i++) m[i] += (int16_t)(((i * 37) % 11) - 5) * 12;
    probar("manotazo izquierda con ruido", m, n, -1);

    gen_andar(m, n, 1000);
    probar("andar con el reloj puesto", m, n, 0);

    memset(m, 0, sizeof(m));
    probar("quieto", m, n, 0);

    /* Solo ruido del sensor en reposo: no debe inventarse nada. */
    for (int i = 0; i < n; i++) m[i] = (int16_t)(1000 + (((i * 29) % 7) - 3) * 8);
    probar("quieto con ruido", m, n, 0);

    printf("\n%s\n", fallos ? "HAY FALLOS" : "todo correcto");
    return fallos != 0;
}
