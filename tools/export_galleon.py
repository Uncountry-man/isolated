#!/usr/bin/env python3
# GALEÃO: segundo navio do jogo, DERIVADO do navio atual (sem Blender): python3 tools/export_galleon.py  (depois de tools/export_ship.py, usa build/ship_raw.e0s)
# Separa o modelo em peças (casco, mastros, vergas, velas, canhões...), estica o meio do casco, aumenta tudo, duplica um mastro e acrescenta geometria própria:
# faixas pintadas e frisos dourados que seguem a curva do casco, portinholas com canhões, tábuas do convés, popa com janelas acesas, lanternas, carranca, cordame.
# Espaço do navio = o de ship.h: a = -z (proa), b = x (estibordo), y (linha d'água em 0). As medidas walkable daqui ficam em shared/ship.h (kShipDefs[1]): mudou F/SB/SY, mude lá.
# Alfa do vértice: 255 normal, 252 = brilho de ouro (kPropMain), 254 = vela (shipbake não subdivide nem escurece).
import sys
import numpy as np
sys.path.insert(0, 'tools')
from ship_kit import *
import ship_kit as kit

RAW = 'build/galleon_raw.e0s'
OUT = sys.argv[1] if len(sys.argv) > 1 else 'assets/galleon.e0s'
S, SB, SY = 1.3, 1.35, 1.3      # escala das pontas (a), boca (b) e altura (y)
MID0, MID1, NM0, NM1 = -4.0, 5.0, -11.0, 11.0   # trecho do meio do casco [-4, 5] vira [-11, 11]


def F(a):
    a = np.asarray(a, dtype=np.float64)
    return np.where(a < MID0, S * (a - MID0) + NM0, np.where(a > MID1, S * (a - MID1) + NM1, NM0 + (a - MID0) * (NM1 - NM0) / (MID1 - MID0)))


def dF(a):
    a = np.asarray(a, dtype=np.float64)
    return np.where((a < MID0) | (a > MID1), S, (NM1 - NM0) / (MID1 - MID0))


kit.configure(F, dF, SB, SY)

# ---- paleta ----
WOOD_HULL = (104, 60, 34); WOOD_LIGHT = (150, 104, 62); WOOD_DECK = (172, 132, 88); WOOD_DECK2 = (156, 116, 76); SEAM = (118, 84, 54)
NAVY = (26, 42, 78); NAVY2 = (34, 54, 96); GOLD = (232, 182, 72); IRON = (44, 44, 50); SAIL = (240, 228, 200); CRIMSON = (156, 30, 36); GLOW = (255, 196, 96); BLACK = (18, 18, 22)

def hull_color(c):
    if c[0] <= 70 and abs(c[0] - c[2]) < 10: return (30, 26, 28)        # faixa da linha d'água
    return WOOD_HULL if c[0] > 120 else (78, 44, 24)
def mast_color(c):
    if c[0] == c[2]: return IRON if c[0] < 100 else GOLD                  # cintas do mastro
    return (122, 80, 44) if c[0] > 120 else (84, 54, 28)
def sail_color(c):
    return CRIMSON if c[0] < 100 else SAIL

# ---- peças do navio antigo ----
SUB[0] = 1
stretched(hull, hull_color)
SUB[0] = 0
kit.measure_hull()
MAST_MAIN, MAST_FORE, MAST_MIZZEN = 0.2, 11.2, -11.3
OLD_MAIN, OLD_REAR = 1.7, -5.3
for part in main_mast:
    rigid(part, OLD_MAIN, MAST_MAIN, sa=1.45, sb=1.6, sy=1.62, b_from=-0.1, b_to=-0.135, colorfn=mast_color)
for part in main_yards:
    rigid(part, OLD_MAIN, MAST_MAIN, sa=1.45, sb=2.3, sy=1.62, b_from=-0.1, b_to=-0.135, colorfn=mast_color)
for part in main_sail:
    rigid(part, OLD_MAIN, MAST_MAIN, sa=1.9, sb=2.3, sy=1.62, b_from=-0.1, b_to=-0.135, colorfn=sail_color, alpha=254)
for a_to, sy in ((MAST_FORE, 1.75), (MAST_MIZZEN, 1.5)):
    for part in rear:
        sail = part['n'] == 96
        wide = part['n'] != 2684 and part['n'] != 828       # vergas e vela abrem mais que o mastro
        rigid(part, OLD_REAR, a_to, sa=1.7 if sail else 1.35, sb=2.15 if wide else 1.5, sy=sy, b_from=-0.1, b_to=-0.135, colorfn=sail_color if sail else mast_color, alpha=254 if sail else 255)
for part in bow_rig:
    sail = part['n'] in (49, 18)
    stretched(part, sail_color if sail else mast_color, 254 if sail else 255)
for part in windows:
    stretched(part, lambda c: GLOW if c[0] < 100 else GOLD, 252)
for part in cannons + carriages:
    ca = 0.5 * (part['a0'] + part['a1']); cb = 0.5 * (part['b0'] + part['b1'])
    rigid(part, ca, float(F(ca)), b_from=cb, b_to=cb * SB, y_to=SY * 0.0, colorfn=(lambda c: IRON if c[0] < 100 else GOLD) if part['n'] == 796 else (lambda c: (92, 56, 30)))

strake(0.62, 2.02, NAVY, rows=3)
strake(0.52, 0.64, GOLD, off=0.05, alpha=252, rows=1)
strake(2.00, 2.12, GOLD, off=0.05, alpha=252, rows=1)
strake(2.50, 2.58, GOLD, off=0.04, alpha=252, rows=1, a0=-25.5, a1=-16.5)
strake(3.10, 3.18, GOLD, off=0.04, alpha=252, rows=1, a0=-25.5, a1=-16.5)

# ---- portinholas e canhões da bateria baixa ----
GUN_Y = 1.32
for a in np.arange(-14.0, 14.6, 3.5):
    for s in (-1, 1):
        b = side_b(a, GUN_Y, s)
        if b is None: continue
        o = s * 0.06
        hw, hh = 0.42, 0.36
        box(to_e(a, b + o, GUN_Y), [0.02, 0, 0], [0, hh, 0], [0, 0, hw], BLACK)
        for dy in (-1, 1):
            box(to_e(a, b + o * 1.5, GUN_Y + dy * (hh + 0.03)), [0.04, 0, 0], [0, 0.04, 0], [0, 0, hw + 0.07], GOLD, 252)
        for dz in (-1, 1):
            box(to_e(a + dz * (hw + 0.03), b + o * 1.5, GUN_Y), [0.04, 0, 0], [0, hh, 0], [0, 0, 0.04], GOLD, 252)
        cyl(to_e(a, b - s * 0.1, GUN_Y), to_e(a, b + s * 0.95, GUN_Y), 0.17, 0.13, IRON, sides=8)
        cyl(to_e(a, b + s * 0.85, GUN_Y), to_e(a, b + s * 1.0, GUN_Y), 0.17, 0.17, IRON, sides=8)

# ---- tábuas do convés e do toldo ----
DECK_Y, TER_Y = 1.316, 4.405
PLK = (WOOD_DECK, WOOD_DECK2, (164, 122, 80))
SUB[0] = 1
planks(-15.9, 12.0, -3.9, 3.75, DECK_Y, PLK, SEAM)
planks(-20.5, -18.9, -4.15, 3.95, TER_Y, PLK, SEAM)
planks(-18.9, -16.1, -2.1, 1.85, TER_Y, PLK, SEAM)
SUB[0] = 0

# ---- popa: galeria de janelas acesas sobre a parede lisa ----
def panel(bc, yc, hb, hy, col, off, alpha=255, depth=0.02):
    a = stern_a(bc, yc)
    if a is None: return None
    box(to_e(a - off, bc, yc), [hb, 0, 0], [0, hy, 0], [0, 0, depth], col, alpha)
    return a

def window(bc, yc, hb, hy):
    if panel(bc, yc, hb, hy, GLOW, 0.05, 252) is None: return
    a = stern_a(bc, yc) - 0.09
    for dy in (-1, 1): box(to_e(a, bc, yc + dy * hy), [hb + 0.05, 0, 0], [0, 0.05, 0], [0, 0, 0.05], GOLD, 252)
    for db in (-1, 0, 1): box(to_e(a, bc + db * hb, yc), [0.035 if db == 0 else 0.05, 0, 0], [0, hy, 0], [0, 0, 0.05], GOLD, 252)
    box(to_e(a, bc, yc), [hb, 0, 0], [0, 0.025, 0], [0, 0, 0.04], GOLD, 252)

panel(0.0, 6.2, 3.95, 1.25, NAVY, 0.03)
panel(0.0, 3.35, 3.5, 0.72, NAVY2, 0.03)
for bc in (-3.1, -1.55, 0.0, 1.55, 3.1): window(bc, 6.2, 0.56, 0.86)
for bc in (-2.5, -0.85, 0.85, 2.5): window(bc, 3.35, 0.46, 0.5)
for yc, hy in ((7.5, 0.06), (4.92, 0.06), (4.1, 0.05), (2.6, 0.05)):
    panel(0.0, yc, 3.98, hy, GOLD, 0.08, 252, 0.05)
a_st = stern_a(0.0, 6.2)
for b in np.arange(-3.8, 3.81, 0.48):        # balaústres da varanda
    cyl(to_e(a_st - 0.55, b, 4.95), to_e(a_st - 0.55, b, 5.42), 0.035, 0.035, GOLD, sides=5, alpha=252, caps=False)
box(to_e(a_st - 0.3, 0.0, 4.92), [3.98, 0, 0], [0, 0.05, 0], [0, 0, 0.32], (120, 78, 44))
box(to_e(a_st - 0.55, 0.0, 5.44), [3.98, 0, 0], [0, 0.035, 0], [0, 0, 0.05], GOLD, 252)
for s_ in (-1, 1): box(to_e(a_st - 0.3, s_ * 3.98, 5.2), [0.04, 0, 0], [0, 0.28, 0], [0, 0, 0.3], GOLD, 252)
ct = to_e(a_st - 0.14, 0.0, 8.05)            # brasão no topo
box(ct, [0.62, 0, 0], [0, 0.42, 0], [0, 0, 0.05], GOLD, 252)
box(ct + np.array([0, 0, 0.07]), [0.44, 0, 0], [0, 0.28, 0], [0, 0, 0.02], CRIMSON)
for b in (-3.6, 0.0, 3.6):                   # lanternas
    x, z = b, -(a_st + 0.25)
    cyl([x, 9.0, z], [x, 9.65, z], 0.05, 0.05, IRON, sides=5, caps=False)
    lathe((x, z), [(9.65, 0.14), (9.8, 0.27), (10.25, 0.27), (10.4, 0.1)], GLOW, sides=6, alpha=252)
    lathe((x, z), [(10.4, 0.1), (10.62, 0.03)], GOLD, sides=6, alpha=252)
    for k in range(4):
        q = k * np.pi / 2 + np.pi / 4
        cyl([x + 0.27 * np.cos(q), 9.8, z + 0.27 * np.sin(q)], [x + 0.27 * np.cos(q), 10.25, z + 0.27 * np.sin(q)], 0.018, 0.018, GOLD, sides=3, alpha=252, caps=False)

# ---- proa: carranca dourada sob o gurupés ----
BOW_A, BOW_B = 19.1, -0.135
curve = [(18.6, 1.75, 0.20), (19.5, 2.0, 0.22), (20.3, 2.55, 0.2), (20.9, 3.2, 0.17), (21.25, 3.85, 0.15)]
for (a0, y0, r0), (a1, y1, r1) in zip(curve, curve[1:]):
    cyl(to_e(a0, BOW_B, y0), to_e(a1, BOW_B, y1), r0, r1, GOLD, sides=6, alpha=252, caps=False)
hx, hy = 21.35, 4.15                          # cabeça, pescoço e crista
for y0, r0, y1, r1 in ((3.82, 0.15, 3.98, 0.2), (3.98, 0.2, 4.2, 0.24), (4.2, 0.24, 4.42, 0.18), (4.42, 0.18, 4.52, 0.04)):
    cyl(to_e(hx, BOW_B, y0), to_e(hx + 0.1, BOW_B, y1), r0, r1, GOLD, sides=6, alpha=252, caps=False)
cyl(to_e(hx + 0.12, BOW_B, 4.2), to_e(hx + 0.58, BOW_B, 4.12), 0.13, 0.05, GOLD, sides=5, alpha=252)
for s_ in (-1, 1):                            # asas coladas ao casco
    for k, (ln, dy) in enumerate(((1.9, 0.0), (1.5, -0.3), (1.05, -0.58))):
        cyl(to_e(20.2, BOW_B + s_ * 0.16, 2.72 + dy * 0.5), to_e(20.2 - ln, BOW_B + s_ * (0.5 + ln * 0.42), 3.05 + dy), 0.07, 0.015, GOLD, sides=4, alpha=252, caps=False)

# ---- cordame e flâmulas ----
def beam(a): return (side_b(a, 2.1, 1) or 3.8)
for am, top in ((MAST_MAIN, 21.0), (MAST_FORE, 15.5), (MAST_MIZZEN, 14.0)):
    for s_ in (-1, 1):
        for k in range(5):
            a = am - 1.6 + k * 0.62
            b = side_b(a, 2.1, s_)
            if b is None: continue
            rope(to_e(am, -0.135, top), to_e(a, b, 2.3))
    box(to_e(am, -0.135, 1.6), [0.08, 0, 0], [0, 0.3, 0], [0, 0, 0.08], IRON)
rope(to_e(MAST_MAIN, -0.135, 23.5), to_e(MAST_FORE, -0.135, 19.4), 0.03)
rope(to_e(MAST_MIZZEN, -0.135, 16.5), to_e(MAST_MAIN, -0.135, 21.5), 0.03)
rope(to_e(MAST_FORE, -0.135, 18.8), to_e(25.8, -0.2, 5.6), 0.03)
rope(to_e(MAST_MIZZEN, -0.135, 15.8), to_e(a_st + 0.8, -0.135, 9.0), 0.03)
def pennant(a, y, ln, col):
    b0 = -0.135
    p = [to_e(a, b0, y + 0.45), to_e(a - ln * 0.6, b0 + 0.25, y + 0.33), to_e(a - ln, b0 - 0.1, y + 0.2), to_e(a, b0, y - 0.05)]
    nn = np.array([0.3, 1.0, 0.0]); nn /= np.linalg.norm(nn)
    emit(p, np.tile(nn, (4, 1)), col, 254, [[0, 1, 3], [1, 2, 3]])
pennant(MAST_FORE, 20.0, 3.4, CRIMSON)
pennant(MAST_MIZZEN, 17.2, 3.0, CRIMSON)
for am, y1 in ((MAST_FORE, 20.5), (MAST_MIZZEN, 17.6)):
    cyl(to_e(am, -0.135, y1 - 2.2), to_e(am, -0.135, y1), 0.07, 0.03, (84, 54, 28), sides=5, caps=False)

kit.finish(RAW, OUT, 1.2)
