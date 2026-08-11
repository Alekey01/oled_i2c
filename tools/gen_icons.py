# -*- coding: utf-8 -*-
"""
Genera los iconos PNG de la PWA a partir del mismo sprite de BitCat que usan el
firmware y la pagina, para que no se desincronicen. Escribe PNG a mano (zlib +
struct) para no depender de Pillow.

    python tools/gen_icons.py
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_bitcat import POSES, estampar, W, H  # noqa: E402

SALIDA = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'webapp')

FONDO = (0x14, 0x13, 0x0e)   # --fg del tema oscuro
GATO  = (0x6f, 0xa8, 0xe0)   # --acc del tema oscuro, sobre fondo oscuro

SPRITE = estampar(POSES['SIT'], 'NORMAL')


def png(ancho, alto, pixeles):
    """pixeles: bytearray RGB de ancho*alto*3."""
    filas = bytearray()
    for y in range(alto):
        filas.append(0)                                  # filtro None
        inicio = y * ancho * 3
        filas += pixeles[inicio:inicio + ancho * 3]

    def trozo(tipo, datos):
        c = tipo + datos
        return struct.pack('>I', len(datos)) + c + struct.pack('>I', zlib.crc32(c))

    return (b'\x89PNG\r\n\x1a\n'
            + trozo(b'IHDR', struct.pack('>IIBBBBB', ancho, alto, 8, 2, 0, 0, 0))
            + trozo(b'IDAT', zlib.compress(bytes(filas), 9))
            + trozo(b'IEND', b''))


def render(lado, ocupacion):
    """ocupacion: fraccion del lado que ocupa el sprite. Las variantes maskable
    piden margen porque Android recorta el icono a la forma del launcher."""
    escala = max(1, int(lado * ocupacion) // W)
    sw, sh = W * escala, H * escala
    x0, y0 = (lado - sw) // 2, (lado - sh) // 2

    px = bytearray(FONDO * (lado * lado))
    for r in range(H):
        for c in range(W):
            if SPRITE[r][c] != '#':
                continue
            for dy in range(escala):
                y = y0 + r * escala + dy
                base = (y * lado + x0 + c * escala) * 3
                px[base:base + escala * 3] = bytes(GATO) * escala
    return png(lado, lado, px)


ICONOS = [
    ('icon-192.png',          192, 0.80),
    ('icon-512.png',          512, 0.80),
    ('icon-maskable-512.png', 512, 0.50),   # zona segura: circulo central del 80%
    ('apple-touch-icon.png',  180, 0.72),
]

if __name__ == '__main__':
    for nombre, lado, ocupacion in ICONOS:
        ruta = os.path.normpath(os.path.join(SALIDA, nombre))
        with open(ruta, 'wb') as f:
            f.write(render(lado, ocupacion))
        print('%-24s %4dx%-4d %6d bytes' % (nombre, lado, lado, os.path.getsize(ruta)))
