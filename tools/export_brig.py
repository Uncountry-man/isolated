#!/usr/bin/env python3
# BERGANTIM: quarto navio do jogo, navio pirata de guerra (rápido, 2 mastros com pano redondo, velas NEGRAS, casco preto com faixa vermelha e frisos dourados,
# bandeira de caveira no topo do mastro grande, 12 canhões). Derivado do navio original como o galeão e a chalupa: python3 tools/export_brig.py
# (depois de tools/export_ship.py, usa build/ship_raw.e0s). O mapeamento F/SB/SY daqui é o de brigA/kBrigB em shared/ship.h (kShipDefs[ShipBrig]): mudou um, mude o outro.
import sys
import numpy as np
sys.path.insert(0, 'tools')
from ship_kit import *
import ship_kit as kit

RAW = 'build/brig_raw.e0s'
OUT = sys.argv[1] if len(sys.argv) > 1 else 'assets/brig.e0s'
S, SB, SY = 1.0, 0.95, 1.0       # pontas iguais ao navio original, boca mais estreita (casco esguio = rápido)
MID0, MID1, NM0, NM1 = -4.0, 5.0, -7.0, 7.0   # o meio do casco [-4, 5] vira [-7, 7]: 5 m mais comprido


def F(a):
    a = np.asarray(a, dtype=np.float64)
    return np.where(a < MID0, S * (a - MID0) + NM0, np.where(a > MID1, S * (a - MID1) + NM1, NM0 + (a - MID0) * (NM1 - NM0) / (MID1 - MID0)))
def dF(a):
    a = np.asarray(a, dtype=np.float64)
    return np.where((a < MID0) | (a > MID1), S, (NM1 - NM0) / (MID1 - MID0))
kit.configure(F, dF, SB, SY)

# ---- paleta: preto, vermelho-sangue e ouro velho ----
HULL = (44, 38, 36); HULL_LOW = (28, 24, 24); RED = (128, 22, 22); RED2 = (104, 16, 18); GOLD = (214, 166, 70); IRON = (40, 40, 46)
MAST = (78, 54, 36); MAST2 = (58, 40, 26); SAIL = (52, 48, 48); SAIL2 = (40, 36, 36); BONE = (232, 226, 210); GLOW = (255, 186, 90); BLACK = (16, 16, 18)
PLK = ((118, 88, 60), (104, 76, 50), (112, 82, 56)); SEAM = (62, 44, 30)

def hull_color(c):
    if c[0] <= 70 and abs(c[0] - c[2]) < 10: return (88, 22, 20)        # faixa da linha d'água: vermelho escuro (zarcão)
    return HULL if c[0] > 120 else HULL_LOW
def mast_color(c):
    if c[0] == c[2]: return IRON if c[0] < 100 else GOLD
    return MAST if c[0] > 120 else MAST2
def sail_color(c):
    return SAIL2 if c[0] < 100 else SAIL

# ---- casco esticado ----
SUB[0] = 1
stretched(hull, hull_color)
SUB[0] = 0
kit.measure_hull()

# ---- 2 mastros de pano redondo (o mastro grande do original, duas vezes); o mastro de ré do original sai ----
OLD_MAIN = 1.29                     # centro do pau do mastro grande (medido na malha: a 0,99..1,59)
MAST_FORE, MAST_MAIN, MAST_B = 5.6, -4.2, -0.1 * SB
for a_to, sy, sw in ((MAST_FORE, 1.04, 0.95), (MAST_MAIN, 1.2, 1.08)):
    for part in main_mast:
        rigid(part, OLD_MAIN, a_to, sa=0.95, sb=0.95, sy=sy, b_from=-0.1, b_to=MAST_B, colorfn=mast_color)
    for part in main_yards:
        rigid(part, OLD_MAIN, a_to, sa=0.95, sb=sw, sy=sy, b_from=-0.1, b_to=MAST_B, colorfn=mast_color)
    for part in main_sail:
        rigid(part, OLD_MAIN, a_to, sa=1.15, sb=sw, sy=sy, b_from=-0.1, b_to=MAST_B, colorfn=sail_color, alpha=254)
for part in bow_rig:
    sail = part['n'] in (49, 18)
    stretched(part, sail_color if sail else mast_color, 254 if sail else 255)
for part in windows:
    stretched(part, lambda c: GLOW if c[0] < 100 else GOLD, 252)
for part in cannons + carriages:
    ca = 0.5 * (part['a0'] + part['a1']); cb = 0.5 * (part['b0'] + part['b1'])
    rigid(part, ca, float(F(ca)), sa=1.0, sb=1.0, sy=1.0, b_from=cb, b_to=cb * SB, colorfn=(lambda c: IRON if c[0] < 100 else GOLD) if part['n'] == 796 else (lambda c: (70, 46, 28)))

# ---- faixas pintadas e frisos dourados ----
strake(0.36, 1.06, RED, rows=2, a0=-17.0, a1=14.0, step=0.8)
strake(1.06, 1.14, GOLD, off=0.045, alpha=252, rows=1, a0=-17.0, a1=14.0, step=0.8)
strake(0.29, 0.36, GOLD, off=0.045, alpha=252, rows=1, a0=-17.0, a1=14.0, step=0.8)
strake(1.55, 1.62, GOLD, off=0.04, alpha=252, rows=1, a0=-15.0, a1=10.0, step=0.8)

# ---- portinholas fechadas na faixa vermelha (bateria baixa, só desenho) ----
GUN_Y = 0.72
for a in np.arange(-11.0, 9.1, 2.9):
    for s in (-1, 1):
        b = side_b(a, GUN_Y, s)
        if b is None: continue
        o = s * 0.05
        hw, hh = 0.32, 0.26
        box(to_e(a, b + o, GUN_Y), [0.02, 0, 0], [0, hh, 0], [0, 0, hw], BLACK)
        for dy in (-1, 1): box(to_e(a, b + o * 1.5, GUN_Y + dy * (hh + 0.03)), [0.035, 0, 0], [0, 0.035, 0], [0, 0, hw + 0.06], GOLD, 252)
        for dz in (-1, 1): box(to_e(a + dz * (hw + 0.03), b + o * 1.5, GUN_Y), [0.035, 0, 0], [0, hh, 0], [0, 0, 0.035], GOLD, 252)
        cyl(to_e(a, b - s * 0.1, GUN_Y), to_e(a, b + s * 0.75, GUN_Y), 0.13, 0.1, IRON, sides=8)

# ---- tábuas do convés e do toldo (alturas = as do navio original: SY 1) ----
DECK_Y, TER_Y = 1.0125, 3.39
SUB[0] = 1
planks(-10.7, 7.5, -2.95 * SB, 2.85 * SB, DECK_Y, PLK, SEAM, w=0.26)
planks(-14.1, -13.05, -3.07 * SB, 3.0 * SB, TER_Y, PLK, SEAM, w=0.26)
planks(-13.05, -11.0, -1.5 * SB, 1.42 * SB, TER_Y, PLK, SEAM, w=0.26)
SUB[0] = 0

# ---- popa: janelas acesas, painel vermelho e 2 lanternas ----
def panel(bc, yc, hb, hy, col, off, alpha=255, depth=0.02):
    a = stern_a(bc, yc)
    if a is None: return None
    box(to_e(a - off, bc, yc), [hb, 0, 0], [0, hy, 0], [0, 0, depth], col, alpha)
    return a
def window(bc, yc, hb, hy):
    if panel(bc, yc, hb, hy, GLOW, 0.05, 252) is None: return
    a = stern_a(bc, yc) - 0.08
    for dy in (-1, 1): box(to_e(a, bc, yc + dy * hy), [hb + 0.04, 0, 0], [0, 0.04, 0], [0, 0, 0.04], GOLD, 252)
    for db in (-1, 0, 1): box(to_e(a, bc + db * hb, yc), [0.03, 0, 0], [0, hy, 0], [0, 0, 0.04], GOLD, 252)
panel(0.0, 4.75, 2.6, 0.85, RED2, 0.03)
for bc in (-1.8, -0.6, 0.6, 1.8): window(bc, 4.75, 0.42, 0.6)
for yc in (5.7, 3.85): panel(0.0, yc, 2.65, 0.05, GOLD, 0.07, 252, 0.04)
a_st = stern_a(0.0, 4.75)
if a_st is not None:
    for b in (-2.3, 2.3):
        x, z = b, -(a_st + 0.25)
        cyl([x, 6.4, z], [x, 6.95, z], 0.04, 0.04, IRON, sides=5, caps=False)
        lathe((x, z), [(6.95, 0.11), (7.07, 0.21), (7.45, 0.21), (7.57, 0.08)], GLOW, sides=6, alpha=252)
        lathe((x, z), [(7.57, 0.08), (7.76, 0.025)], GOLD, sides=6, alpha=252)

# ---- proa: carranca de CAVEIRA dourada na ponta da roda de proa ----
BOW_A, BOW_B = float(F(13.0)) - 0.25, -0.1 * SB
skull = to_e(BOW_A + 0.55, BOW_B, 3.05)
lathe((skull[0], skull[2]), [(2.78, 0.05), (2.86, 0.2), (3.05, 0.27), (3.25, 0.25), (3.38, 0.14), (3.43, 0.02)], BONE, sides=8)
for s_ in (-1, 1): box(to_e(BOW_A + 0.78, BOW_B + s_ * 0.1, 3.1), [0.065, 0, 0], [0, 0.065, 0], [0, 0, 0.03], BLACK)   # olhos
box(to_e(BOW_A + 0.72, BOW_B, 2.88), [0.13, 0, 0], [0, 0.07, 0], [0, 0, 0.1], BONE)                                      # maxilar
cyl(to_e(BOW_A - 0.5, BOW_B, 2.2), to_e(BOW_A + 0.45, BOW_B, 2.86), 0.16, 0.08, GOLD, sides=6, alpha=252, caps=False)

# ---- bandeira de CAVEIRA no topo do mastro grande (tremula para trás) ----
top = 15.79 * 1.2
cyl(to_e(MAST_MAIN, MAST_B, top - 0.6), to_e(MAST_MAIN, MAST_B, top + 1.9), 0.06, 0.035, MAST2, sides=5, caps=False)
def flag_pt(u, v):   # u 0..1 do mastro para trás, v 0..1 de baixo para cima
    return to_e(MAST_MAIN - 0.05 - 2.4 * u, MAST_B + 0.22 * np.sin(u * 5.5) * u, top + 0.2 + 1.55 * v - 0.25 * u * u)
NU = 6
up = np.array([0.25, 1.0, 0.0]); up /= np.linalg.norm(up)
for i in range(NU):
    u0, u1 = i / NU, (i + 1) / NU
    emit([flag_pt(u0, 0), flag_pt(u1, 0), flag_pt(u1, 1), flag_pt(u0, 1)], np.tile(up, (4, 1)), BLACK, 254, [[0, 1, 2], [0, 2, 3]])
def flag_mark(u, v, du, dv, col, off=0.03):   # retângulo na bandeira, dos 2 lados
    for side in (-1, 1):
        q = [flag_pt(u - du, v - dv), flag_pt(u + du, v - dv), flag_pt(u + du, v + dv), flag_pt(u - du, v + dv)]
        q = [p + np.array([side * off, 0, 0]) for p in q]
        emit(q, np.tile(up, (4, 1)), col, 254, [[0, 1, 2], [0, 2, 3]])
flag_mark(0.42, 0.62, 0.075, 0.15, BONE)                 # crânio
flag_mark(0.42, 0.47, 0.045, 0.05, BONE)                 # maxilar
for s_ in (-1, 1):                                       # ossos cruzados (2 tiras em X)
    for k in range(5):
        t = (k - 2) / 2.0
        flag_mark(0.42 + 0.1 * t, 0.27 + s_ * 0.12 * t, 0.03, 0.04, BONE)
for e in (-1, 1): flag_mark(0.42 + e * 0.035, 0.65, 0.022, 0.045, BLACK, off=0.05)   # olhos

# ---- cordame: ovéns dos 2 mastros, estais entre eles, para a proa e para a popa ----
for am, top_r in ((MAST_FORE, 14.3), (MAST_MAIN, 16.6)):
    for s_ in (-1, 1):
        for k in range(4):
            a = am - 1.3 + k * 0.62
            b = side_b(a, 1.6, s_)
            if b is not None: rope(to_e(am, MAST_B, top_r), to_e(a, b, 1.7))
rope(to_e(MAST_MAIN, MAST_B, 17.8), to_e(MAST_FORE, MAST_B, 15.2), 0.026)
rope(to_e(MAST_FORE, MAST_B, 15.6), to_e(float(F(16.6)), -0.12, 4.3), 0.026)
a_st2 = stern_a(0.0, 6.0)
if a_st2 is not None: rope(to_e(MAST_MAIN, MAST_B, 16.8), to_e(a_st2 + 0.6, MAST_B, 6.2), 0.026)

kit.finish(RAW, OUT, 0.8)
