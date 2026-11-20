#!/usr/bin/env python3
# CHALUPA: terceiro navio do jogo, pequeno e sem canhões (só leme e baú), derivado do navio original: python3 tools/export_boat.py  (depois de tools/export_ship.py)
# O mapeamento F/SB/Y daqui é o de boatA/kBoatB/kBoatY em shared/ship.h (kShipDefs[ShipBoat]): mudou um, mude o outro. Ferramentas em tools/ship_kit.py.
import sys
import numpy as np
sys.path.insert(0, 'tools')
from ship_kit import *
import ship_kit as kit

RAW = 'build/boat_raw.e0s'
OUT = sys.argv[1] if len(sys.argv) > 1 else 'assets/boat.e0s'
S, SB, SY = 0.62, 0.7, 0.62      # escala das pontas (a), boca (b) e altura (y)
MID0, MID1, NM0, NM1 = -4.0, 5.0, -2.2, 2.2   # o meio do casco [-4, 5] encolhe para [-2,2; 2,2]
Y_CUT, Y_TOP = 3.39, 0.38        # acima do toldo a popa encolhe mais: no barco pequeno ela não pode virar uma torre


def F(a):
    a = np.asarray(a, dtype=np.float64)
    return np.where(a < MID0, S * (a - MID0) + NM0, np.where(a > MID1, S * (a - MID1) + NM1, NM0 + (a - MID0) * (NM1 - NM0) / (MID1 - MID0)))
def dF(a):
    a = np.asarray(a, dtype=np.float64)
    return np.where((a < MID0) | (a > MID1), S, (NM1 - NM0) / (MID1 - MID0))
def Yf(y):
    y = np.asarray(y, dtype=np.float64)
    return np.where(y > Y_CUT, Y_CUT * SY + (y - Y_CUT) * Y_TOP, y * SY)
def dYf(y):
    return np.where(np.asarray(y) > Y_CUT, Y_TOP, SY)
kit.configure(F, dF, SB, SY, Yf, dYf)

HULL = (176, 138, 96); HULL_LOW = (118, 84, 54); TEAL = (28, 104, 108); CREAM = (234, 226, 204); MAST = (150, 108, 66); IRON = (44, 44, 50); SAIL = (242, 236, 218); SAIL2 = (34, 122, 124)
GLOW = (255, 200, 110); BRASS = (214, 170, 84)
PLK = ((190, 152, 108), (174, 136, 94), (182, 144, 100)); SEAM = (128, 96, 64)

def hull_color(c):
    if c[0] <= 70 and abs(c[0] - c[2]) < 10: return (36, 40, 44)
    return HULL if c[0] > 120 else HULL_LOW
def mast_color(c):
    if c[0] == c[2]: return IRON if c[0] < 100 else BRASS
    return MAST if c[0] > 120 else (112, 78, 44)
def sail_color(c):
    return SAIL if c[0] < 100 else SAIL2

SUB[0] = 1
stretched(hull, hull_color)
SUB[0] = 0
kit.measure_hull()

MAST_A, MID_B = 0.7, -0.07
OLD_MAIN = 1.7
for part in main_mast:
    rigid(part, OLD_MAIN, MAST_A, sa=0.6, sb=0.6, sy=0.72, b_from=-0.1, b_to=MID_B, colorfn=mast_color)
for part in main_yards:
    rigid(part, OLD_MAIN, MAST_A, sa=0.6, sb=0.68, sy=0.72, b_from=-0.1, b_to=MID_B, colorfn=mast_color)
for part in main_sail:
    rigid(part, OLD_MAIN, MAST_A, sa=0.85, sb=0.68, sy=0.72, b_from=-0.1, b_to=MID_B, colorfn=sail_color, alpha=254)
for part in bow_rig:
    if part['n'] in (892, 49): continue       # verga e vela do gurupés: só o gurupés e a bujarrona
    sail = part['n'] == 18
    stretched(part, sail_color if sail else mast_color, 254 if sail else 255)
for part in windows:
    stretched(part, lambda c: GLOW if c[0] < 100 else BRASS, 252)

strake(0.36, 0.92, TEAL, a0=-7.0, a1=6.5, step=0.6, rows=2)
strake(0.92, 1.00, CREAM, off=0.045, a0=-7.0, a1=6.5, step=0.6, rows=1)
strake(0.30, 0.36, CREAM, off=0.045, a0=-7.0, a1=6.5, step=0.6, rows=1)

DECK_Y, TER_Y = 0.6275, 2.102
SUB[0] = 1
planks(-4.55, 3.1, -2.15, 2.05, DECK_Y, PLK, SEAM, w=0.2)
planks(-6.45, -5.95, -2.2, 2.05, TER_Y, PLK, SEAM, w=0.2)
planks(-5.95, -4.8, -1.05, 0.95, TER_Y, PLK, SEAM, w=0.2)
SUB[0] = 0

# popa: 3 janelas e a lanterna
def window(bc, yc, hb, hy):
    a = stern_a(bc, yc)
    if a is None: return
    box(to_e(a - 0.04, bc, yc), [hb, 0, 0], [0, hy, 0], [0, 0, 0.02], GLOW, 252)
    for dy in (-1, 1): box(to_e(a - 0.07, bc, yc + dy * hy), [hb + 0.03, 0, 0], [0, 0.03, 0], [0, 0, 0.03], BRASS, 252)
    for db in (-1, 0, 1): box(to_e(a - 0.07, bc + db * hb, yc), [0.025, 0, 0], [0, hy, 0], [0, 0, 0.03], BRASS, 252)
a_st = stern_a(0.0, 2.0)
if a_st is not None:
    for bc in (-1.0, 0.0, 1.0): window(bc, 2.0, 0.3, 0.26)
    box(to_e(a_st - 0.03, 0.0, 1.0), [1.9, 0, 0], [0, 0.26, 0], [0, 0, 0.02], TEAL)
    x, z = 0.0, -(a_st + 0.3)
    top = float(Yf(7.0))
    cyl([x, top, z], [x, top + 0.42, z], 0.03, 0.03, IRON, sides=5, caps=False)
    lathe((x, z), [(top + 0.42, 0.08), (top + 0.5, 0.15), (top + 0.76, 0.15), (top + 0.84, 0.05)], GLOW, sides=6, alpha=252)
    lathe((x, z), [(top + 0.84, 0.05), (top + 0.96, 0.015)], BRASS, sides=6, alpha=252)

# cordame: 3 ovéns por bordo e estais para a proa e a popa
top = 7.9
for s_ in (-1, 1):
    for k in range(3):
        a = MAST_A - 0.9 + k * 0.45
        b = side_b(a, 1.0, s_)
        if b is not None: rope(to_e(MAST_A, MID_B, top), to_e(a, b, 1.15), 0.016)
rope(to_e(MAST_A, MID_B, 9.2), to_e(9.2, -0.07, 3.0), 0.018)
if a_st is not None: rope(to_e(MAST_A, MID_B, 9.2), to_e(a_st + 0.4, MID_B, float(Yf(7.0))), 0.018)

kit.finish(RAW, OUT, 0.7)
