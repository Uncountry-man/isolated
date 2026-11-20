#!/usr/bin/env python3
# Fatia a folha "import assets/icones/inventory_icons.png" (4 colunas x 3 linhas de ícones de inventário) em ícones recortados (fundo
# removido por detecção de bordas + preenchimento a partir das bordas) e gera assets/icons.e0i = atlas RGBA8 4x3 de 128x128 (header 'E0I1' + w u16 + h u16 + pixels).
# Ordem (linha a linha): ADAGA, MACHADINHA, PICARETA, TOCHA / COCO, FOLHA, PEDRA, GRAVETO / DIAMANTE, OURO, MADEIRA, CORDA.
# Uso: python3 tools/make_icons.py [--preview saida.png]
import sys, os, struct
import numpy as np, cv2
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'import assets', 'icones', 'inventory_icons.png')
COLS, ROWS, CELL = 4, 3, 128
img = cv2.imread(SRC, cv2.IMREAD_COLOR)
H, W = img.shape[:2]
# Fundo = região lisa (sem bordas) ligada às bordas da folha; os ícones têm contorno escuro nítido, o fundo é um degradê borrado.
edges = cv2.dilate(cv2.Canny(cv2.GaussianBlur(img, (3, 3), 0), 30, 90), np.ones((5, 5), np.uint8))
fill = np.where(edges == 0, 255, 0).astype(np.uint8); ff = np.zeros((H + 2, W + 2), np.uint8)
for (x, y) in [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1), (W // 2, 0), (W // 2, H - 1), (0, H // 2), (W - 1, H // 2)]:
    if fill[y, x] == 255: cv2.floodFill(fill, ff, (x, y), 128)
fg = np.where(fill == 128, 0, 255).astype(np.uint8)
fg = cv2.erode(cv2.morphologyEx(fg, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8)), np.ones((5, 5), np.uint8))
n, lab, st, cen = cv2.connectedComponentsWithStats(fg)
comps = [i for i in range(1, n) if st[i, cv2.CC_STAT_AREA] > 5000]
assert len(comps) == COLS * ROWS, 'esperava %d ícones, achei %d' % (COLS * ROWS, len(comps))
atlas = np.zeros((ROWS * CELL, COLS * CELL, 4), np.uint8)
for i in comps:
    c = min(COLS - 1, int(cen[i][0] / (W / COLS))); r = min(ROWS - 1, int(cen[i][1] / (H / ROWS)))
    x, y, w, h = st[i, 0], st[i, 1], st[i, 2], st[i, 3]; pad = 4
    x0, y0, x1, y1 = max(0, x - pad), max(0, y - pad), min(W, x + w + pad), min(H, y + h + pad)
    t = img[y0:y1, x0:x1]; a = cv2.dilate(np.where(lab[y0:y1, x0:x1] == i, 255, 0).astype(np.uint8), np.ones((5, 5), np.uint8))
    a = cv2.GaussianBlur(a, (3, 3), 0)
    sc = (CELL - 8) / max(t.shape[0], t.shape[1])
    nw, nh = max(1, round(t.shape[1] * sc)), max(1, round(t.shape[0] * sc))
    t = cv2.resize(t, (nw, nh), interpolation=cv2.INTER_AREA); a = cv2.resize(a, (nw, nh), interpolation=cv2.INTER_AREA)
    # sangra a cor das bordas para os pixels transparentes (evita halo com filtro linear)
    tt = t.copy(); known = (a > 200).astype(np.uint8)
    for _ in range(6):
        dil = cv2.dilate(tt * known[..., None], np.ones((3, 3), np.uint8)); cnt = cv2.dilate(known, np.ones((3, 3), np.uint8))
        tt = np.where(known[..., None] > 0, tt, dil); known = cnt
    ox, oy = (CELL - nw) // 2, (CELL - nh) // 2
    cell = atlas[r * CELL:(r + 1) * CELL, c * CELL:(c + 1) * CELL]
    cell[oy:oy + nh, ox:ox + nw, :3] = tt[..., ::-1]; cell[oy:oy + nh, ox:ox + nw, 3] = a
out = os.path.join(ROOT, 'assets', 'icons.e0i')
with open(out, 'wb') as f:
    f.write(b'E0I1'); f.write(struct.pack('<HH', atlas.shape[1], atlas.shape[0])); f.write(atlas.tobytes())
print('ESCRITO', out, atlas.shape)
if '--preview' in sys.argv:
    p = sys.argv[sys.argv.index('--preview') + 1]
    chk = np.indices(atlas.shape[:2]).sum(0) // 8 % 2 * 60 + 80
    comp = (atlas[..., :3][..., ::-1] * (atlas[..., 3:] / 255.0) + chk[..., None] * (1 - atlas[..., 3:] / 255.0)).astype(np.uint8)
    cv2.imwrite(p, cv2.resize(comp, None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST))
