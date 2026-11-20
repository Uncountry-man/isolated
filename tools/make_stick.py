#!/usr/bin/env python3
# Gera a malha do GRAVETO COLETÁVEL (kPropStick em engine/src/props_data.h). A malha vinda do scan (34 tris) saiu da decimação do Blender como uma FITA achatada
# (sem volume: parecia "quebrada", fina como um fio). Esta versão é um galho de verdade: tronco de 6 lados afilado e levemente curvo, nó, ponta cortada,
# um galhinho e a extremidade partida. Eixo longo = +X (-0,30..+0,30 m), apoiado em y = 0 (o assentamento no terreno em terrain_renderer.cpp não muda).
# Uso: python3 tools/make_stick.py   (reescreve o bloco kPropStickVerts/kPropStick de props_data.h)
import math, os, re
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.path.join(ROOT, 'engine', 'src', 'props_data.h')
tris = []   # (v0, v1, v2, cor)
def sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def norm(a):
    l = math.sqrt(sum(c*c for c in a)) or 1.0
    return (a[0]/l, a[1]/l, a[2]/l)
def ring(c, axis, r, sides, phase=0.0):
    ref = (0, 1, 0) if abs(axis[1]) < 0.9 else (1, 0, 0)
    u = norm(cross(axis, ref)); v = cross(axis, u)
    return [(c[0] + r*(u[0]*math.cos(phase + 2*math.pi*i/sides) + v[0]*math.sin(phase + 2*math.pi*i/sides)),
             c[1] + r*(u[1]*math.cos(phase + 2*math.pi*i/sides) + v[1]*math.sin(phase + 2*math.pi*i/sides)),
             c[2] + r*(u[2]*math.cos(phase + 2*math.pi*i/sides) + v[2]*math.sin(phase + 2*math.pi*i/sides))) for i in range(sides)]
def tube(pts, radii, sides, col, cap_start=True, tip=True):
    rings = []
    for i, p in enumerate(pts):
        a = norm(sub(pts[min(i+1, len(pts)-1)], pts[max(i-1, 0)]))
        rings.append(ring(p, a, radii[i], sides, 0.3))
    for i in range(len(pts)-1):
        shade = 0.82 + 0.28 * ((i * 37) % 5) / 4.0
        c = tuple(int(min(255, k*shade)) for k in col)
        for s in range(sides):
            a, b, c2, d = rings[i][s], rings[i][(s+1) % sides], rings[i+1][(s+1) % sides], rings[i+1][s]
            tris.append((a, b, c2, c)); tris.append((a, c2, d, c))
    if cap_start:
        ctr = pts[0]; lc = tuple(int(k*0.6) for k in col)
        for s in range(sides): tris.append((ctr, rings[0][(s+1) % sides], rings[0][s], lc))
    if tip:
        a = norm(sub(pts[-1], pts[-2])); apex = tuple(pts[-1][k] + a[k]*radii[-1]*1.4 for k in range(3)); c = tuple(int(k*0.9) for k in col)
        for s in range(sides): tris.append((rings[-1][s], rings[-1][(s+1) % sides], apex, c))
BARK = (96, 66, 38)
# tronco: 5 pontos, levemente curvo em y/z, afilando para +X
n = 6; pts = []; rad = []
for i in range(n):
    t = i / (n-1)
    x = -0.30 + 0.60*t
    r = 0.0175*(1.0 - 0.42*t) * (1.22 if abs(t-0.4) < 0.1 else 1.0)    # nó em t≈0.4
    y = r*0.95 + 0.004*math.sin(math.pi*t)
    z = 0.007*math.sin(2*math.pi*t + 0.6)
    pts.append((x, y, z)); rad.append(r)
tube(pts, rad, 6, BARK)
# galhinho que sai do nó para o lado e para cima (extremidade quebrada)
o = pts[2]
d = norm((0.55, 0.38, 0.75)); tw = [(o[0] + d[0]*s, o[1] + d[1]*s*0.6 + 0.004, o[2] + d[2]*s) for s in (0.0, 0.05, 0.10, 0.145)]
tube(tw, [0.0085, 0.0075, 0.0062, 0.0050], 5, (112, 80, 48))
# toquinho de um segundo galho curto para trás
o2 = pts[1]; d2 = norm((-0.35, 0.3, -0.85)); tw2 = [(o2[0] + d2[0]*s, o2[1] + d2[1]*s*0.5 + 0.003, o2[2] + d2[2]*s) for s in (0.0, 0.04, 0.075)]
tube(tw2, [0.0072, 0.0062, 0.0050], 5, (84, 58, 33))
# ---- escreve ----
def vert(p, nrm, col):
    return '    {%.4ff, %.4ff, %.4ff, %d, %d, %d, 0, %d, %d, %d, 255},' % (p[0], p[1], p[2], round(nrm[0]*127), round(nrm[1]*127), round(nrm[2]*127), col[0], col[1], col[2])
lines = []
for (a, b, c, col) in tris:
    nrm = norm(cross(sub(b, a), sub(c, a)))
    # normal para FORA do eixo (centro do triângulo acima do chão): garante virada para cima/lado
    for p in (a, b, c): lines.append(vert(p, nrm, col))
cnt = len(lines)
src = open(P, encoding='utf-8').read()
m = re.search(r'static const PropVertex kPropStickVerts\[\d+\] = \{.*?\n\};', src, re.S)
assert m, 'bloco kPropStickVerts não encontrado'
src = src[:m.start()] + 'static const PropVertex kPropStickVerts[%d] = {\n%s\n};' % (cnt, '\n'.join(lines)) + src[m.end():]
src = re.sub(r'static const PropModel kPropStick = \{ kPropStickVerts, \d+ \};', 'static const PropModel kPropStick = { kPropStickVerts, %d };' % cnt, src)
open(P, 'w', encoding='utf-8').write(src)
print('graveto: %d triângulos (%d vértices)' % (len(tris), cnt))
