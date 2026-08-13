# -*- coding: utf-8 -*-
"""Compara anchos de sombrilla sobre la pantalla real."""
import sys, re, io
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

W, H = 128, 32
CAT_X, CAT_Y = (W - 31) // 2, 8
PATA_COL = 28                      # centro de la patita levantada
PATA_X = CAT_X + PATA_COL          # 76
CABEZA = (CAT_X + 6, CAT_X + 24)   # 54..72

src = io.open(r'C:\esp\oled_i2c\main\bitcat.c', encoding='utf-8', errors='replace').read()
m = re.search(r'\[BITCAT_WAVE_A\] = \{(.*?)\n    \}', src, re.S)
WAVE_A = re.findall(r'"([.#]*)"', m.group(1))

def toldo(ancho, alto=4):
    """Arco: cada fila un poco mas ancha, y el borde de abajo dentado como las
       varillas, que es lo que lo distingue de una loma."""
    c = ancho // 2
    filas = ['.' * c + '#' + '.' * (ancho - c - 1)]          # remate
    for r in range(alto):
        hw = round((r + 1) / alto * c)
        filas.append('.' * (c - hw) + '#' * (2 * hw + 1) + '.' * (c - hw))
    borde = ''.join('#' if (i - c) % 4 else '.' for i in range(ancho))
    filas.append(borde)
    return [f[:ancho].ljust(ancho, '.') for f in filas]

def pinta(ancho):
    g = [['.'] * W for _ in range(H)]
    def est(x, y, filas):
        for r, f in enumerate(filas):
            for cc, ch in enumerate(f):
                if ch == '#' and 0 <= x + cc < W and 0 <= y + r < H:
                    g[y + r][x + cc] = '#'
    est(CAT_X, CAT_Y, WAVE_A)
    t = toldo(ancho)
    x0 = PATA_X - ancho // 2
    est(x0, 0, t)
    est(PATA_X, len(t), ['#'] * (CAT_Y + 3 - len(t)))   # mango hasta la patita
    for r in range(7):
        for cc in list(range(0, 30)) + list(range(105, W)):
            if g[r][cc] == '.': g[r][cc] = ':'
    print(f"\n--- toldo de {ancho} px: pantalla x {x0}..{x0+ancho-1} "
          f"| cubre cabeza({CABEZA[0]}..{CABEZA[1]}): "
          f"{max(0, min(x0+ancho-1, CABEZA[1]) - max(x0, CABEZA[0]) + 1)} de 19 px"
          f" | choca texto: {x0 < 30 or x0+ancho-1 >= 105}")
    for r, f in enumerate(g):
        print(f'{r:3d} |' + ''.join(f) + '|')

for a in (19, 25, 31):
    pinta(a)
