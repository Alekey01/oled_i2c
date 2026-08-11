# -*- coding: utf-8 -*-
"""
Genera el arte ASCII de BitCat calculando los tramos, para no contar columnas a
mano. Imprime una vista previa y el bloque C listo para pegar en main/bitcat.c.

Geometria (31x24):
  cola   cols 3..8   (izquierda, se enrosca hacia arriba)
  cabeza cols 6..24, filas 3..14   con orejas encima
  cara   cols 8..22, filas 5..12   (interior apagado; los ojos se estampan)
  cuerpo cols 9..21, filas 15..23
  brazo  cols 26..30 (derecha, solo en el saludo)
"""
import sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

W, H = 31, 24

def fila(*tramos):
    r = ['.'] * W
    for a, b in tramos:
        for c in range(a, b + 1):
            if 0 <= c < W:
                r[c] = '#'
    return ''.join(r)

def unir(base, *tramos_por_fila):
    """Superpone tramos {fila: [(a,b), ...]} sobre una lista de filas."""
    out = [list(f) for f in base]
    for mapa in tramos_por_fila:
        for r, tramos in mapa.items():
            for a, b in tramos:
                for c in range(a, b + 1):
                    if 0 <= r < len(out) and 0 <= c < W:
                        out[r][c] = '#'
    return [''.join(f) for f in out]

# --- cabeza y orejas: iguales en todas las poses ------------------------------
CABEZA = [
    fila((7, 8), (22, 23)),      # 0  puntas de orejas
    fila((7, 9), (21, 23)),      # 1
    fila((7, 10), (20, 23)),     # 2
    fila((6, 24)),               # 3  borde superior
    fila((6, 24)),               # 4
    fila((6, 7), (23, 24)),      # 5  pantalla de la cara (interior apagado)
    fila((6, 7), (23, 24)),      # 6
    fila((6, 7), (23, 24)),      # 7
    fila((6, 7), (23, 24)),      # 8
    fila((6, 7), (23, 24)),      # 9
    fila((6, 7), (23, 24)),      # 10
    fila((6, 7), (23, 24)),      # 11
    fila((6, 7), (23, 24)),      # 12
    fila((6, 24)),               # 13 borde inferior
    fila((6, 24)),               # 14
]

# --- cola: se enrosca hacia arriba por la izquierda ---------------------------
COLA_ABAJO = {15: [(4, 5)], 16: [(4, 5)], 17: [(4, 5)],
              18: [(5, 6)], 19: [(6, 7)], 20: [(7, 8)]}
COLA_ARRIBA = {13: [(3, 4)], 14: [(3, 4)], 15: [(3, 4)], 16: [(4, 5)],
               17: [(5, 6)], 18: [(6, 7)], 19: [(7, 8)]}

# --- cuerpo -------------------------------------------------------------------
def cuerpo(patas):
    filas = []
    for r in range(15, 24):
        if r == 15:
            filas.append(fila((10, 20)))
        elif r == 16:
            filas.append(fila((10, 11), (19, 20)))   # collar: hueco al centro
        elif r <= 21:
            filas.append(fila((9, 21)))
        else:
            filas.append(fila(*patas))
    return filas

PATAS_JUNTAS = [(9, 12), (18, 21)]
PATAS_A      = [(9, 12), (17, 20)]
PATAS_B      = [(10, 13), (18, 21)]

# --- brazo levantado del saludo ----------------------------------------------
def brazo(fila_pata):
    """fila_pata = fila superior de la manita; el antebrazo baja hasta el hombro."""
    m = {fila_pata: [(26, 30)], fila_pata + 1: [(26, 30)]}
    for r in range(fila_pata + 2, 17):
        m[r] = [(27, 29)]
    m[16] = [(21, 29)]      # hombro: conecta el brazo con el cuerpo
    return m

def pose(patas, cola, extra=None):
    base = CABEZA + cuerpo(patas)
    return unir(base, cola, extra or {})

POSES = {
    'SIT':    pose(PATAS_JUNTAS, COLA_ABAJO),
    'WALK_A': pose(PATAS_A, COLA_ABAJO),
    'WALK_B': pose(PATAS_B, COLA_ARRIBA),
    'WAVE_A': pose(PATAS_JUNTAS, COLA_ARRIBA, brazo(3)),
    'WAVE_B': pose(PATAS_JUNTAS, COLA_ARRIBA, brazo(6)),
}

# --- ojos: 4x4 por expresion --------------------------------------------------
OJOS = {
    'NORMAL':   (['####', '####', '####', '####'], ['####', '####', '####', '####']),
    'FELIZ':    (['....', '.##.', '#..#', '....'], ['....', '.##.', '#..#', '....']),
    'SORPRESA': (['....', '.##.', '.##.', '....'], ['....', '.##.', '.##.', '....']),
    'DORMIDO':  (['....', '....', '####', '....'], ['....', '....', '####', '....']),
    'ENOJADO':  (['#...', '.#..', '..#.', '...#'], ['...#', '..#.', '.#..', '#...']),
    'AMOR':     (['#..#', '####', '.##.', '....'], ['#..#', '####', '.##.', '....']),
}
EYE_X_L, EYE_X_R, EYE_Y = 10, 17, 6

def estampar(filas, expr):
    izq, der = OJOS[expr]
    out = [list(f) for f in filas]
    for r in range(4):
        for c in range(4):
            if izq[r][c] == '#':
                out[EYE_Y + r][EYE_X_L + c] = '#'
            if der[r][c] == '#':
                out[EYE_Y + r][EYE_X_R + c] = '#'
    return [''.join(f) for f in out]

def mostrar(filas, titulo):
    print('\n' + titulo)
    for f in filas:
        print('  ' + f.replace('#', '█').replace('.', ' '))

# --- accesorios: mismo arte que dibujar_accesorio() en bitcat.c ----------------
# Cada entrada es una lista de estampas (dx, dy, filas) relativas al sprite.
ACC_PARAGUAS = ['..##..', '.####.', '######']
ACC_LENTES   = ['#####..#####', '############', '.####..####.', '..###..###..']
ACC_RAYAS    = ['###', '', '', '###', '', '', '###']
ACC_Z        = ['###', '.#.', '###']

ACCESORIOS = {
    'NINGUNO':  [],
    'PARAGUAS': [(25, 0, ACC_PARAGUAS)],
    'LENTES':   [(9, 6, ACC_LENTES)],
    'FRIO':     [(1, 6, ACC_RAYAS), (27, 6, ACC_RAYAS)],
    'ZZZ':      [(26, 4, ACC_Z), (28, 0, ACC_Z)],
}

# El tiritar se sale de la caja del sprite, asi que la vista previa lleva margen.
MARGEN = 4

def con_accesorio(filas, acc):
    """Devuelve el sprite con margen lateral y el accesorio encima."""
    out = [['.'] * (W + 2 * MARGEN) for _ in range(H)]
    for r in range(H):
        for c in range(W):
            out[r][MARGEN + c] = filas[r][c]
    for dx, dy, estampa in ACCESORIOS[acc]:
        for r, fila in enumerate(estampa):
            for c, ch in enumerate(fila):
                if ch != '#':
                    continue
                y, x = dy + r, MARGEN + dx + c
                if 0 <= y < H and 0 <= x < W + 2 * MARGEN:
                    out[y][x] = '#'
    return [''.join(f) for f in out]

ORDEN_POSES = ['SIT', 'WALK_A', 'WALK_B', 'WAVE_A', 'WAVE_B']
ORDEN_EXPR  = ['NORMAL', 'FELIZ', 'SORPRESA', 'DORMIDO', 'ENOJADO', 'AMOR']

def emitir_c():
    out = []
    out.append('static const char *BITCAT[BITCAT_POSES][BITCAT_H] = {')
    for p in ORDEN_POSES:
        out.append('    [BITCAT_%s] = {' % p)
        for f in POSES[p]:
            out.append('        "%s",' % f)
        out.append('    },')
    out.append('};')
    out.append('')
    out.append('static const char *OJOS[BITCAT_EXPR_COUNT][2][4] = {')
    for e in ORDEN_EXPR:
        izq, der = OJOS[e]
        out.append('    [BITCAT_%s] = {' % e)
        out.append('        {%s},' % ', '.join('"%s"' % r for r in izq))
        out.append('        {%s},' % ', '.join('"%s"' % r for r in der))
        out.append('    },')
    out.append('};')
    return '\n'.join(out)

def emitir_js():
    """Para la pagina web: poses ya estampadas con su expresion."""
    def bloque(nombre, pares):
        out = ['const %s = [[' % nombre]
        for i, (p, e) in enumerate(pares):
            for f in estampar(POSES[p], e):
                out.append('  "%s",' % f)
            out.append('], [' if i == 0 else ']];')
        return '\n'.join(out)
    return (bloque('GATO_CAMINA', [('WALK_A', 'NORMAL'), ('WALK_B', 'NORMAL')])
            + '\n\n'
            + bloque('GATO_SALUDA', [('WAVE_A', 'FELIZ'), ('WAVE_B', 'FELIZ')]))

if __name__ == '__main__':
    for p in ORDEN_POSES:
        assert len(POSES[p]) == H, (p, len(POSES[p]))
        assert all(len(f) == W for f in POSES[p]), p
    if '--c' in sys.argv:
        print(emitir_c())
    elif '--js' in sys.argv:
        print(emitir_js())
    else:
        for p in ORDEN_POSES:
            mostrar(estampar(POSES[p], 'NORMAL'), p)
        for e in ORDEN_EXPR:
            mostrar(estampar(POSES['SIT'], e)[3:15], 'CARA ' + e)
        # Cada accesorio con la pose y expresion con que lo usa el firmware.
        for acc, pose, expr in [('PARAGUAS', 'WAVE_A', 'NORMAL'),
                                ('LENTES',   'SIT',    'FELIZ'),
                                ('FRIO',     'SIT',    'ENOJADO'),
                                ('ZZZ',      'SIT',    'DORMIDO')]:
            mostrar(con_accesorio(estampar(POSES[pose], expr), acc),
                    'ACC %s (%s / %s)' % (acc, pose, expr))
