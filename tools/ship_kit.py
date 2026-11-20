#!/usr/bin/env python3
# Peças e ferramentas comuns a tools/export_galleon.py e tools/export_boat.py: lê o navio original (build/ship_raw.e0s), separa em peças por componente conexa
# e monta a malha nova. Cada exportador define o mapeamento com configure() e chama finish() no fim.
# Espaço do navio = o de ship.h: a = -z (proa), b = x (estibordo), y (linha d'água em 0). Alfa do vértice: 255 normal, 252 = brilho de ouro (kPropMain), 254 = vela/corda.
import struct, subprocess
import numpy as np

SRC = 'build/ship_raw.e0s'
F = dF = None
SB = SY = 1.0
Y = lambda y: y * SY       # altura: linear, a não ser que o exportador troque
dY = lambda y: SY + 0 * y


def configure(f, df, sb, sy, yfn=None, dyfn=None):
    global F, dF, SB, SY, Y, dY
    F, dF, SB, SY = f, df, sb, sy
    if yfn: Y, dY = yfn, dyfn


# ---- leitura e separação em peças ----
d = open(SRC, 'rb').read()
nv, ni = struct.unpack('<II', d[4:12])
v = np.frombuffer(d, dtype=np.dtype([('p', '<3f4'), ('n', 'i1', 4), ('c', 'u1', 4)]), count=nv, offset=12)
I = np.frombuffer(d, dtype='<u4', count=ni, offset=12 + nv * 20).reshape(-1, 3).astype(np.int64)
P0 = v['p'].astype(np.float64); N0 = v['n'][:, :3].astype(np.float64) / 127.0; C0 = v['c'][:, :3].astype(np.float64)
_, pid = np.unique(np.round(P0 * 200).astype(np.int64), axis=0, return_inverse=True); pid = pid.reshape(-1)
par = np.arange(pid.max() + 1)
def find(x):
    while par[x] != x:
        par[x] = par[par[x]]; x = par[x]
    return x
for t in pid[I]:
    r = find(t[0]); par[find(t[1])] = r; par[find(t[2])] = r
root = np.array([find(x) for x in range(len(par))])
comp = root[pid[I[:, 0]]]
parts = []
for cid in np.unique(comp):
    tris = I[comp == cid]; q = P0[tris.reshape(-1)]
    parts.append(dict(tris=tris, n=len(tris), a0=-q[:, 2].max(), a1=-q[:, 2].min(), b0=q[:, 0].min(), b1=q[:, 0].max(), y0=q[:, 1].min(), y1=q[:, 1].max()))
def pick(fn): return [p for p in parts if fn(p)]
hull = pick(lambda p: p['n'] > 8000)[0]
main_mast = pick(lambda p: p['n'] > 6000 and p['n'] < 8000)
main_yards = pick(lambda p: p['n'] == 892 and 1.3 < p['a0'] < 1.5)
main_sail = pick(lambda p: p['n'] == 72)
rear = pick(lambda p: p['a0'] > -6.3 and p['a1'] < -3.3 and p['y1'] > 7.5)   # mastro, vergas, gávea e vela de ré
bow_rig = pick(lambda p: p['a1'] > 13.0 or p['n'] == 18)                     # gurupés, verga, flâmula, bujarrona
cannons = pick(lambda p: p['n'] == 796)
carriages = pick(lambda p: p['n'] == 518)
windows = pick(lambda p: p['n'] == 608)

# ---- saída ----
OP, ON, OC, OA, OI, OS = [], [], [], [], [], []
base = [0]
SUB = [0]   # 1 = shipbake subdivide esta peça (só casco e tábuas: mastros e cordas têm arestas longas e nenhum ganho)
def emit(p, n, c, a, tris):
    p = np.asarray(p, dtype=np.float64)
    OP.append(p); ON.append(np.asarray(n, dtype=np.float64)); OC.append(np.broadcast_to(np.asarray(c, dtype=np.float64), p.shape).copy())
    OA.append(np.full(len(p), a) if np.isscalar(a) else np.asarray(a)); OS.append(np.full(len(p), SUB[0])); OI.append(np.asarray(tris, dtype=np.int64) + base[0]); base[0] += len(p)

def to_e(a, b, y):   # (a, b, y) -> (x, y, z) da engine
    return np.stack([b, y, -a], axis=-1)

def take(part):      # vértices da peça, reindexados
    idx, inv = np.unique(part['tris'], return_inverse=True)
    return P0[idx], N0[idx], C0[idx], inv.reshape(-1, 3)

def recolor(c, fn):
    return np.array([fn(tuple(int(x) for x in q)) for q in c], dtype=np.float64)

def stretched(part, colorfn=None, alpha=255):
    p, n, c, t = take(part)
    a = -p[:, 2]
    q = to_e(F(a), p[:, 0] * SB, Y(p[:, 1]))
    m = np.c_[n[:, 0] / SB, n[:, 1] / dY(p[:, 1]), n[:, 2] / dF(a)]
    m /= np.maximum(np.linalg.norm(m, axis=1), 1e-9)[:, None]
    if alpha == 254: m = sail_normals(m)
    emit(q, m, recolor(c, colorfn) if colorfn else c, alpha, t)

def sail_normals(m):   # plano fino visto dos 2 lados: a normal aponta para cima, com um pouco da curvatura (o sol ilumina a vela de qualquer lado)
    q = m * np.where(m[:, 2:3] > 0, -1.0, 1.0) * 0.5 + np.array([0.0, 1.0, 0.0])
    return q / np.linalg.norm(q, axis=1)[:, None]

def rigid(part, a_from, a_to, sa=1.3, sb=1.3, sy=1.3, b_from=0.0, b_to=0.0, y_from=0.0, y_to=0.0, colorfn=None, alpha=255):
    p, n, c, t = take(part)
    a = (-p[:, 2] - a_from) * sa + a_to; b = (p[:, 0] - b_from) * sb + b_to; y = (p[:, 1] - y_from) * sy + y_to
    m = np.c_[n[:, 0] / sb, n[:, 1] / sy, n[:, 2] / sa]
    m /= np.maximum(np.linalg.norm(m, axis=1), 1e-9)[:, None]
    if alpha == 254: m = sail_normals(m)
    emit(to_e(a, b, y), m, recolor(c, colorfn) if colorfn else c, alpha, t)

def quad(p0, p1, p2, p3, col, alpha=255):
    p = np.array([p0, p1, p2, p3], dtype=np.float64)
    nn = np.cross(p[1] - p[0], p[3] - p[0]); nn /= max(np.linalg.norm(nn), 1e-9)
    emit(p, np.tile(nn, (4, 1)), col, alpha, [[0, 1, 2], [0, 2, 3]])

def box(c, ax, ay, az, col, alpha=255):   # c = centro; ax/ay/az = meios-eixos, tudo em (x, y, z) da engine
    c, ax, ay, az = (np.asarray(q, dtype=np.float64) for q in (c, ax, ay, az))
    k = [c + sx * ax + sy * ay + sz * az for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        quad(k[f[0]], k[f[1]], k[f[2]], k[f[3]], col, alpha)

def cyl(p0, p1, r0, r1, col, sides=8, alpha=255, caps=True):
    p0, p1 = np.asarray(p0, dtype=np.float64), np.asarray(p1, dtype=np.float64)
    w = p1 - p0; w /= max(np.linalg.norm(w), 1e-9)
    u = np.cross(w, [0, 1, 0] if abs(w[1]) < 0.9 else [1, 0, 0]); u /= np.linalg.norm(u); t = np.cross(w, u)
    ang = np.arange(sides) * 2 * np.pi / sides
    ring = np.cos(ang)[:, None] * u + np.sin(ang)[:, None] * t
    pts = np.vstack([p0 + ring * r0, p1 + ring * r1])
    tr = []
    for i in range(sides):
        j = (i + 1) % sides
        tr += [[i, j, sides + j], [i, sides + j, sides + i]]
    emit(pts, np.vstack([ring, ring]), col, alpha, tr)
    if caps:
        for q, r, nn in ((p0, r0, -w), (p1, r1, w)):
            if r > 1e-4:
                emit(np.vstack([q, q + ring * r]), np.tile(nn, (sides + 1, 1)), col, alpha, [[0, 1 + i, 1 + (i + 1) % sides] for i in range(sides)])

def rope(a, b, r=0.022):
    cyl(a, b, r, r, (58, 44, 30), sides=3, caps=False, alpha=254)

def lathe(a0, axis_pts, col, sides=8, alpha=255):   # sólido de revolução em volta de um eixo vertical: axis_pts = [(y, raio)...], a0 = (x, z)
    for (y0, r0), (y1, r1) in zip(axis_pts, axis_pts[1:]):
        cyl([a0[0], y0, a0[1]], [a0[0], y1, a0[1]], r0, r1, col, sides, alpha, caps=False)

# ---- curva do costado: maior |b| do casco em (a, y) ----
TA = TY = TB = den = Pm = Im = None
def measure_hull():   # chamar logo depois de emitir o casco: a partir daqui se mede a curva do costado
    global TA, TY, TB, den, Pm, Im
    Pm = np.vstack(OP); Im = np.vstack(OI)
    T = Pm[Im]; TA, TY, TB = -T[:, :, 2], T[:, :, 1], T[:, :, 0]
    den = (TY[:, 1] - TY[:, 2]) * (TA[:, 0] - TA[:, 2]) + (TA[:, 2] - TA[:, 1]) * (TY[:, 0] - TY[:, 2])
    ok = np.abs(den) > 1e-9
    TA, TY, TB, den = TA[ok], TY[ok], TB[ok], den[ok]
def side_b(a, y, s):
    w0 = ((TY[:, 1] - TY[:, 2]) * (a - TA[:, 2]) + (TA[:, 2] - TA[:, 1]) * (y - TY[:, 2])) / den
    w1 = ((TY[:, 2] - TY[:, 0]) * (a - TA[:, 2]) + (TA[:, 0] - TA[:, 2]) * (y - TY[:, 2])) / den
    w2 = 1 - w0 - w1
    m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
    if not m.any(): return None
    b = (w0 * TB[:, 0] + w1 * TB[:, 1] + w2 * TB[:, 2])[m]
    b = b[b * s > 0]
    return (b * s).max() * s if len(b) else None

def strake(y0, y1, col, off=0.035, a0=-24.0, a1=24.0, step=1.0, alpha=255, rows=2):
    ys = np.linspace(y0, y1, rows + 1)
    for s in (-1, 1):
        prev = None
        for a in np.arange(a0, a1 + 1e-6, step):
            cur = [side_b(a, y, s) for y in ys]
            if prev is not None and all(q is not None for q in cur + prev[1]):
                for k in range(rows):
                    pa = prev[0]
                    quad(to_e(pa, prev[1][k] + s * off, ys[k]), to_e(a, cur[k] + s * off, ys[k]), to_e(a, cur[k + 1] + s * off, ys[k + 1]), to_e(pa, prev[1][k + 1] + s * off, ys[k + 1]), col, alpha)
            prev = (a, cur)

def stern_a(b, y):   # a da superfície da popa em (b, y): raio ao longo de a
    T2 = Pm[Im]; A, Y, B = -T2[:, :, 2], T2[:, :, 1], T2[:, :, 0]
    dn = (Y[:, 1] - Y[:, 2]) * (B[:, 0] - B[:, 2]) + (B[:, 2] - B[:, 1]) * (Y[:, 0] - Y[:, 2])
    k = np.abs(dn) > 1e-9
    A, Y, B, dn = A[k], Y[k], B[k], dn[k]
    w0 = ((Y[:, 1] - Y[:, 2]) * (b - B[:, 2]) + (B[:, 2] - B[:, 1]) * (y - Y[:, 2])) / dn
    w1 = ((Y[:, 2] - Y[:, 0]) * (b - B[:, 2]) + (B[:, 0] - B[:, 2]) * (y - Y[:, 2])) / dn
    w2 = 1 - w0 - w1
    m = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
    return (w0 * A[:, 0] + w1 * A[:, 1] + w2 * A[:, 2])[m].min() if m.any() else None

def planks(a0, a1, b0, b1, y, cols, seam, w=0.30):
    k = 0
    for b in np.arange(b0, b1, w):
        col = cols[0] if k % 2 == 0 else cols[1]
        if k % 3 == 2: col = cols[2]
        quad(to_e(a0, b + 0.012, y + 0.02), to_e(a1, b + 0.012, y + 0.02), to_e(a1, b + w - 0.012, y + 0.02), to_e(a0, b + w - 0.012, y + 0.02), col)
        k += 1
    box(to_e(0.5 * (a0 + a1), 0.5 * (b0 + b1), y + 0.008), [0.5 * (b1 - b0), 0, 0], [0, 0.008, 0], [0, 0, 0.5 * (a1 - a0)], seam)

def finish(raw, out, edge):
    Pa = np.vstack(OP); Ia = np.vstack(OI); Na = np.vstack(ON); Ca = np.vstack(OC); Aa = np.concatenate(OA)
    hdr = struct.pack('<II', len(Pa), Ia.size)
    rec = np.zeros(len(Pa), dtype=np.dtype([('p', '<3f4'), ('n', 'i1', 4), ('c', 'u1', 4)]))
    rec['p'] = Pa
    rec['n'][:, :3] = np.clip(np.round(Na * 127), -127, 127)
    rec['n'][:, 3] = np.concatenate(OS)
    rec['c'][:, :3] = np.clip(Ca, 0, 255); rec['c'][:, 3] = Aa
    with open(raw, 'wb') as o:
        o.write(b'E0S1'); o.write(hdr); o.write(rec.tobytes()); o.write(Ia.astype('<u4').tobytes())
    print('cru:', len(Pa), 'vertices', len(Ia), 'triangulos', 'bbox', Pa.min(0).round(2), Pa.max(0).round(2))
    subprocess.run(['build/shipbake', raw, out, str(edge)], check=True)
