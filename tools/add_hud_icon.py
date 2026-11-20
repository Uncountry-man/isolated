#!/usr/bin/env python3
# Acrescenta UM ícone ao atlas assets/icons.e0i sem refazer os outros (o make_hud_icons.py precisa do repo inteiro do game-icons).
# Uso: python3 -I tools/add_hud_icon.py <arquivo.svg> <índice HUD>   (índice = posição em ICONS de make_hud_icons.py; atlas = 12 + índice)
import sys, os, struct, importlib.util
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, 'tools', 'make_hud_icons.py')).read().replace('\nmain()', '\n')
ns = {'__file__': os.path.join(ROOT, 'tools', 'make_hud_icons.py')}; exec(compile(src, 'mhi', 'exec'), ns)
svg, idx = sys.argv[1], int(sys.argv[2])
ATLAS, CELL, COLS, BASE = ns['ATLAS'], ns['CELL'], ns['COLS'], ns['BASE_ROWS']
d = bytearray(open(ATLAS, 'rb').read()); w, h = struct.unpack('<HH', d[4:8]); px = bytearray(d[8:])
rows_needed = BASE + idx // COLS + 1
if rows_needed * CELL > h:
    px += bytearray((rows_needed * CELL - h) * w * 4); h = rows_needed * CELL
img = np.frombuffer(px, np.uint8).reshape(h, w, 4).copy()
r, c = divmod(idx, COLS)
img[(BASE + r) * CELL:(BASE + r + 1) * CELL, c * CELL:(c + 1) * CELL] = ns['raster'](svg)
open(ATLAS, 'wb').write(b'E0I1' + struct.pack('<HH', w, h) + img.tobytes())
print('icone', idx, '-> atlas', 12 + idx, 'atlas', w, h)
