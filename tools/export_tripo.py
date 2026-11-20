#!/usr/bin/env python3
# PIRATAS "TRIPO" (personagens do jogo: PIRATA HOMEM e PIRATA MULHER). Substituem os piratas low-poly do kit poly.pizza.
# Entrada: import assets/tripo/a.glb (MULHER) e b.glb (HOMEM): scans de IA com ~2 milhões de triângulos, 3 texturas PBR, SEM ossos, pose A.
# O resultado fica PRESO AO ESQUELETO DO Y BOT (assets/ybot.e0a: 65 ossos): animações, ferramenta na mão (osso 34), itens no corpo, carregar,
# cápsulas de acerto, TAG e câmera continuam IGUAIS.
#
# Dois estágios:
#   A. tools/tripo_bake.py (dentro do Blender): solda o scan, decima para 3 LODs (12000 / 4500 / 1600 triângulos), desenha as UVs de cada LOD e
#      "assa" a cor do scan na malha baixa (texturas 1024 / 512 / 256). Saída em build/tripo/out/<m|f>_lod<k>.npz/.png.
#   B. este script (numpy + PIL + scipy): para cada LOD, deforma NA PRÓPRIA POSE A para as juntas do Y Bot, copia os pesos de pele do vértice mais
#      próximo do Y Bot (que também foi posto em pose A), e DESFAZ a pose com a inversa da mistura de matrizes do skinning -> malha em pose T de
#      repouso. Quando a animação abaixa os braços a malha volta exatamente à forma modelada (sem ombros quebrados).
#   Grava assets/pirate_<m|f>.e0m/.e0i (LOD0), _lod1.e0m/.e0i, _lod2.e0m/.e0i (cada LOD com a sua textura; formato E0M3 = E0M2 + UV).
# Uso: python3 tools/export_tripo.py [--no-bake] [--no-rig] [--preview pasta]      (fora do sandbox: chama o Blender Flatpak)
#   C. tools/tripo_rig.py (Blender): pesos de pele por Automatic Weights no armature real do `import assets/Y Bot.fbx` + poses de teste (substitui os pesos do estágio B).
import json, os, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'import assets', 'tripo')
OUT = os.path.join(ROOT, 'assets')
WORK = os.path.join(ROOT, 'build', 'tripo', 'out')

# Juntas do Y Bot no espaço do jogo (m): ver assets/ybot.e0a (invBind).
YB = dict(ankle=0.10, knee=0.53, crotch=0.87, shoulder=1.425, neck=1.525, chin=1.585, brow=1.715,
          sh_x=0.19, elbow_x=0.462, wrist_x=0.738, tip_x=0.95, leg_x=0.093, back_z=0.04)
HAND_R, HAND_L, HEAD = 34, 10, 5
ARM = dict(elbow=0.274, wrist=0.550, tip=0.775)   # Y Bot: distância da junta do ombro (0.188, 1.436, 0.083) ao cotovelo, pulso e ponta dos dedos
SHOULDER = np.array([0.188, 1.436, 0.083])

# Os scans medem ~0,98 de altura: K leva ao tamanho de trabalho (~2 m) em que as constantes de conform_a foram afinadas.
K = 2.0


def uniform_y(chin_model):
    """tabela de alturas SEM distorção: escala única que leva o queixo do scan ao queixo do Y Bot (antes cada segmento era esticado/comprimido para casar
    juntas medidas a olho: tronco -16%, coxa -16%, braços +25%). Juntas do Y Bot em unidades do scan = altura do Y Bot / s."""
    s = YB['chin'] / (chin_model * K)
    return [(0.0, 0.0)] + [(YB[k] / s, YB[k]) for k in ('ankle', 'knee', 'crotch')] + [(1.436 / s, 1.436)] + [(YB[k] / s, YB[k]) for k in ('neck', 'chin', 'brow')], s


def arm_cfg(pivot_x, pivot_y_model, tip, s, angle, c):
    """braço: o do scan é MAIS CURTO que o do Y Bot (13% no homem, 23% na mulher) e o esqueleto não muda. `c` = fração do braço do Y Bot em que a ponta
    da mão do scan cai (0,90 = esticar só ~4-10%, junta do pulso ~5 cm antes da do esqueleto: melhor que esticar tudo)."""
    px, py = pivot_x * K, pivot_y_model * K
    L = float(np.hypot(tip[0] * K - px, tip[1] * K - py))     # comprimento natural do braço no scan (unidades de trabalho)
    return dict(pivot=(px, py), angle=angle, elbow=0.354 * L, wrist=0.71 * L, tip=L, c=c)


_ym, _sm = uniform_y(0.835)
_yf, _sf = uniform_y(0.830)
MODELS = {
    'm': dict(file='b.glb', label='HOMEM', y=_ym,
              arm=arm_cfg(YB['sh_x'] / _sm / K, SHOULDER[1] / _sm / K, (0.369, 0.527), _sm, 40.3, 1.0),
              leg_shift=0.17, depth_k=0.94, depth_c=0.0),
    'f': dict(file='a.glb', label='MULHER', y=_yf,
              arm=arm_cfg(YB['sh_x'] / _sf / K, SHOULDER[1] / _sf / K, (0.310, 0.524), _sf, 47.8, 1.0),
              leg_shift=0.17, depth_k=0.94, depth_c=0.0, hair_fix=True),
}


def load_ybot():
    b = open(os.path.join(ROOT, 'import assets', 'ybot_ref', 'ybot.e0m'), 'rb').read()   # molde dos pesos (o Y Bot não vai mais no APK)
    assert b[:4] == b'E0M2'
    nsub = struct.unpack('<I', b[4:8])[0]; o = 8
    P, B, W = [], [], []
    for _ in range(nsub):
        nv, ni = struct.unpack('<II', b[o + 12:o + 20]); o += 20
        v = np.frombuffer(b, np.uint8, nv * 32, o).reshape(nv, 32)
        P.append(v[:, :12].copy().view(np.float32).reshape(nv, 3).astype(np.float64)); B.append(v[:, 24:28].astype(np.int64)); W.append(v[:, 28:32].astype(np.float64) / 255.0)
        o += nv * 32 + ni * 4
    P = np.concatenate(P); B = np.concatenate(B); W = np.concatenate(W)
    full = np.zeros((len(P), 65))
    for k in range(4): np.add.at(full, (np.arange(len(P)), B[:, k]), W[:, k])
    return P, full


def pw(x, table):
    xs = np.array([t[0] for t in table]); ys = np.array([t[1] for t in table])
    out = np.interp(x, xs, ys)
    k1 = (ys[-1] - ys[-2]) / (xs[-1] - xs[-2])   # além da última ponta: continua com a inclinação do último trecho
    return np.where(x > xs[-1], ys[-1] + (x - xs[-1]) * k1, out)


def smooth(x, a, b): t = np.clip((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t)


def centerline(coord, val, mask, lo, hi, step):
    """curva (média de `val` por faixa de `coord`) entre lo e hi, só com os vértices de `mask`; devolve f(coord) interpolada (extremos constantes)."""
    edges = np.arange(lo, hi + step, step); xs, ys = [], []
    for a, b in zip(edges[:-1], edges[1:]):
        m = mask & (coord >= a) & (coord < b)
        if m.sum() >= 3: xs.append((a + b) / 2); ys.append(val[m].mean())
    if not xs: return lambda c: np.zeros_like(c)
    return lambda c: np.interp(c, xs, ys)


def conform_a(P, cfg):
    """P no espaço do modelo (m; X lateral, Y altura, Z frente=+), NA POSE A -> espaço do jogo (direita = +X, frente = -Z), TAMBÉM na pose A,
    com as juntas nas posições do Y Bot (braços abaixados em `angle` graus em volta da junta do ombro).
    O braço e a perna do scan NÃO são centrados no eixo do esqueleto (o braço fica ~13 cm à frente e curva; a perna 3-6 cm fora): a cada ponto do eixo
    a seção é recentrada sobre o eixo do Y Bot, senão cotovelo/joelho giram em torno de um ponto fora da malha ("braço quebrado")."""
    lat = np.abs(P[:, 0]); sgn = np.sign(P[:, 0]); y = P[:, 1]; dep = P[:, 2]
    ar = cfg['arm']; th = np.radians(ar['angle'])
    d = np.array([np.cos(th), -np.sin(th)]); nrm = np.array([np.sin(th), np.cos(th)])
    # corpo
    Yb = pw(y, cfg['y'])
    Lb = pw(lat, [(0.0, 0.0), (ar['pivot'][0], SHOULDER[0])])
    below = np.clip(1.0 - Yb / YB['crotch'], 0, 1)
    Lb = Lb - np.minimum(Lb * 0.80, cfg['leg_shift'] * below) * (Lb < 0.5)
    Zb = -(dep - cfg['depth_c']) * cfg['depth_k'] + YB['back_z']
    # perna: centro z de cada altura -> z das juntas (quadril, joelho, tornozelo)
    leg = (Yb < YB['crotch']) & (Yb > YB['ankle']) & (lat > 0.04 * K)
    zc = centerline(Yb, Zb, leg, YB['ankle'], YB['crotch'], 0.08)
    zj = np.interp(Yb, [YB['ankle'], YB['knee'], YB['crotch']], [0.048, 0.027, 0.022])
    lw = smooth(Yb, YB['ankle'], YB['ankle'] + 0.12) * (1 - smooth(Yb, YB['crotch'] - 0.12, YB['crotch'])) * (lat > 0.03 * K)
    Zb = Zb + (zj - zc(Yb)) * lw
    # braço: coordenadas ao longo do eixo (t) e perpendicular (s)
    rx = lat - ar['pivot'][0]; ry = y - ar['pivot'][1]
    t = rx * d[0] + ry * d[1]; sp = rx * nrm[0] + ry * nrm[1]
    w = smooth(t, -0.03, 0.09) * (sp > -0.13) * (lat > ar['pivot'][0] - 0.07)
    core = (w > 0.95) & (t > 0.10)
    spc = centerline(t, sp, core, 0.10, ar['tip'], 0.06); dpc = centerline(t, dep, core, 0.10, ar['tip'], 0.06)
    ta = pw(t, [(0.0, 0.0), (ar['elbow'], ARM['elbow'] * ar.get('c', 1.0)), (ar['wrist'], ARM['wrist'] * ar.get('c', 1.0)), (ar['tip'], ARM['tip'] * ar.get('c', 1.0))])
    ta = np.where(t < 0, t * cfg['depth_k'], ta)
    spa = sp - spc(t) * w                                  # centro da seção do braço sobre o eixo
    La = SHOULDER[0] + d[0] * ta + nrm[0] * spa * cfg['depth_k']
    Ya = SHOULDER[1] + d[1] * ta + nrm[1] * spa * cfg['depth_k']
    Za = -(dep - dpc(t) * w) * cfg['depth_k'] + SHOULDER[2]
    L = Lb * (1 - w) + La * w; Y = Yb * (1 - w) + Ya * w; Z = Zb * (1 - w) + Za * w
    return np.stack([-sgn * L, Y, Z], 1)   # giro de 180 graus em Y: a frente +Z do modelo vira -Z do jogo e o X troca de sinal


def arm_mats(angle_deg):
    """matrizes 4x4 que levam a cadeia do braço direito (+X) e do esquerdo (-X) da pose T para a pose A (giro em Z em volta da junta do ombro)."""
    out = []
    for side in (1, -1):
        a = -side * np.radians(angle_deg)
        R = np.eye(4); R[0, 0] = np.cos(a); R[0, 1] = -np.sin(a); R[1, 0] = np.sin(a); R[1, 1] = np.cos(a)
        j = SHOULDER * [side, 1, 1]
        T1 = np.eye(4); T1[:3, 3] = j; T0 = np.eye(4); T0[:3, 3] = -j
        out.append(T1 @ R @ T0)
    return out   # [direito (ossos 32..54), esquerdo (8..30)]


def blend_mats(W, mats):
    wr = W[:, 32:55].sum(1); wl = W[:, 8:31].sum(1); w0 = 1.0 - wr - wl
    return w0[:, None, None] * np.eye(4)[None] + wr[:, None, None] * mats[0][None] + wl[:, None, None] * mats[1][None]


def vertex_normals(P, I):
    fn = np.cross(P[I[:, 1]] - P[I[:, 0]], P[I[:, 2]] - P[I[:, 0]])   # área como peso; malha soldada = normais suaves
    N = np.zeros_like(P)
    for k in range(3): np.add.at(N, I[:, k], fn)
    return N / np.maximum(1e-9, np.linalg.norm(N, axis=1, keepdims=True))


def skin_weights(P, I, yP, yW, pre=None):
    tree = cKDTree(yP)
    d, idx = tree.query(P, k=4)
    wk = 1.0 / np.maximum(d, 1e-4) ** 2; wk /= wk.sum(1, keepdims=True)
    W = (yW[idx] * wk[:, :, None]).sum(1)
    if pre is not None: W = pre(W)    # pesos do braço e dos dedos (analíticos), antes da suavização
    key = np.round(P * 5000).astype(np.int64); _, inv = np.unique(key, axis=0, return_inverse=True); inv = inv.reshape(-1)
    nu = inv.max() + 1
    Wu = np.zeros((nu, 65)); cnt = np.zeros(nu); np.add.at(Wu, inv, W); np.add.at(cnt, inv, 1); Wu /= cnt[:, None]
    e = np.concatenate([inv[I[:, [0, 1]]], inv[I[:, [1, 2]]], inv[I[:, [2, 0]]]])
    for _ in range(2):
        acc = Wu.copy(); n = np.ones(nu)
        np.add.at(acc, e[:, 0], Wu[e[:, 1]]); np.add.at(acc, e[:, 1], Wu[e[:, 0]])
        np.add.at(n, e[:, 0], 1); np.add.at(n, e[:, 1], 1)
        Wu = acc / n[:, None]
    W = Wu[inv]
    head = smooth(P[:, 1], 1.585, 1.64)   # do queixo para cima: só o osso da cabeça (rosto não amassa)
    Wh = np.zeros_like(W); Wh[:, HEAD] = 1.0
    return W * (1 - head[:, None]) + Wh * head[:, None]


FINGERS = {1: dict(hand=34, chains=[(35, 36, 37, 38), (39, 40, 41, 42), (43, 44, 45, 46), (47, 48, 49, 50), (51, 52, 53, 54)], upper=32, fore=33),
           -1: dict(hand=10, chains=[(11, 12, 13, 14), (15, 16, 17, 18), (19, 20, 21, 22), (23, 24, 25, 26), (27, 28, 29, 30)], upper=8, fore=9)}


def arm_finger_weights(PA, angle, joints, mats):
    """pesos ANALÍTICOS do braço (a posição ao longo do eixo ombro->cotovelo->pulso decide, não o vértice do Y Bot mais próximo, que nas mangas largas
    caía no torso e quebrava o cotovelo) e dos DEDOS (cada vértice da mão vai para o segmento de dedo mais próximo do Y Bot; a palma fica na mão).
    Devolve pre(W) -> W."""
    th = np.radians(angle); d = np.array([np.cos(th), -np.sin(th)]); nrm = np.array([np.sin(th), np.cos(th)])
    E, Wr = ARM['elbow'], ARM['wrist']
    def pre(W):
        W = W.copy()
        for si, side in enumerate((1, -1)):
            F = FINGERS[side]; sx = side * PA[:, 0]
            r = np.stack([sx - SHOULDER[0], PA[:, 1] - SHOULDER[1]], 1); sa = r @ d; sp = r @ nrm
            b = smooth(sa, 0.03, 0.09) * (1 - smooth(np.abs(sp), 0.09, 0.13)) * smooth(sx, 0.20, 0.26)
            tE = smooth(sa, E - 0.05, E + 0.05); tW = smooth(sa, Wr - 0.04, Wr + 0.03)
            new = np.zeros_like(W)
            new[:, F['upper']] = 1 - tE; new[:, F['fore']] = tE * (1 - tW)
            hv = np.where(tW > 0.01)[0]
            if len(hv):
                J = (mats[si] @ np.c_[joints, np.ones(len(joints))].T).T[:, :3]   # juntas do Y Bot na pose A
                segs = []   # (a, b, osso, é a 1a falange)
                for ch in F['chains']:
                    for k in range(3): segs.append((J[ch[k]], J[ch[k + 1]], ch[k], k == 0))
                A = np.array([x[0] for x in segs]); B = np.array([x[1] for x in segs]); bone = np.array([x[2] for x in segs]); first = np.array([x[3] for x in segs])
                Pt = PA[hv][:, None, :]; ab = (B - A)[None]; u = np.clip(((Pt - A[None]) * ab).sum(2) / np.maximum((ab * ab).sum(2), 1e-9), 0, 1)
                dist = np.linalg.norm(Pt - (A[None] + u[:, :, None] * ab), axis=2)
                kk = dist.argmin(1); dm = dist[np.arange(len(hv)), kk]
                fing = 1 - smooth(dm, 0.020, 0.040)                  # perto de um dedo: segue o dedo; longe (palma): segue a mão
                fing = np.where(first[kk] & (u[np.arange(len(hv)), kk] < 0.02), fing * 0.0, fing)   # antes do 1o nó: palma
                new[hv, F['hand']] += tW[hv] * (1 - fing)
                new[hv, bone[kk]] += tW[hv] * fing
                new[hv, F['fore']] += 0.0
            W = (1 - b)[:, None] * W + b[:, None] * new
        return W
    return pre


def quantize(W):
    top = np.argsort(-W, axis=1)[:, :4]
    w4 = np.take_along_axis(W, top, 1); w4 /= w4.sum(1, keepdims=True)
    q = np.floor(w4 * 255 + 0.5).astype(np.int64)
    q[:, 0] += 255 - q.sum(1)
    top[q == 0] = 0
    return top.astype(np.uint8), q.astype(np.uint8)



def weld_normals(P, I):
    """normais suaves por posição (a malha tem vértices duplicados nas costuras de UV; sem isso cada costura apareceria como uma linha de luz)."""
    key = np.round(P * 20000).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True); inv = inv.reshape(-1)
    Pw = np.zeros((inv.max() + 1, 3)); Pw[inv] = P
    Nw = vertex_normals(Pw, inv[I])
    return Nw[inv]


def fill_holes(img):
    """texels que o bake não acertou (pretos puros: dedos finos, brincos, fora das ilhas) ganham a cor do texel válido mais próximo (sem preto sangrando nos mipmaps)."""
    from scipy.ndimage import distance_transform_edt
    a = np.asarray(img.convert('RGB')).copy()
    hole = a.max(2) < 6
    if hole.any() and not hole.all():
        _, (iy, ix) = distance_transform_edt(hole, return_indices=True)
        a = a[iy, ix]
    return Image.fromarray(a)


def write_e0m(path, P, N, B, W, UV, I):
    nv = len(P)
    v = np.zeros(nv, dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('b', 'u1', 4), ('w', 'u1', 4), ('uv', '<f4', 2)])
    v['p'] = P; v['n'] = N; v['b'] = B; v['w'] = W; v['uv'] = UV
    with open(path, 'wb') as f:
        f.write(b'E0M3'); f.write(struct.pack('<I', 1))
        f.write(struct.pack('<3fII', 1.0, 1.0, 1.0, nv, I.size))
        f.write(v.tobytes()); f.write(I.astype('<u4').tobytes())


def write_e0i(path, img):
    rgba = img.convert('RGBA')
    with open(path, 'wb') as f:
        f.write(b'E0I1'); f.write(struct.pack('<HH', *rgba.size)); f.write(rgba.tobytes())


def fit(key, cfg, d, yP, yW, tex, joints):
    """malha baixa (espaço do scan) -> (P, N, B, W, UV, I) no espaço do jogo, em pose T de repouso, presa ao esqueleto do Y Bot."""
    P0 = d['P'] * K; I = d['I']
    PA = conform_a(P0, cfg)
    mats = arm_mats(cfg['arm']['angle'])
    yPA = np.einsum('nij,nj->ni', blend_mats(yW, mats), np.c_[yP, np.ones(len(yP))])[:, :3]   # o Y Bot na mesma pose A
    Wc = skin_weights(PA, I, yPA, yW, arm_finger_weights(PA, cfg['arm']['angle'], joints, mats))
    # cabelo comprido / correias: vértices ESCUROS acima da cintura nunca seguem ombro/braço (o cabelo da mulher virava um "bolo" no ombro quando o tronco
    # girava); o peso dos ossos 7..54 vai para a coluna (osso 3)
    th, tw = tex.shape[:2]
    col = tex[np.clip((d['UV'][:, 1] * th).astype(int), 0, th - 1), np.clip((d['UV'][:, 0] * tw).astype(int), 0, tw - 1)].astype(float)
    dark = (col @ [0.30, 0.59, 0.11] < 80) & (PA[:, 1] > 1.25) & (np.abs(PA[:, 0]) < 0.30) & cfg.get('hair_fix', False)   # só a mulher (cabelo longo); no resto pegava texels escuros do braço (espetos)
    Wc[dark, 3] += Wc[dark][:, 7:55].sum(1); Wc[dark, 7:55] = 0
    B, W = quantize(Wc)
    Wq = np.zeros((len(PA), 65))
    for k in range(4): np.add.at(Wq, (np.arange(len(PA)), B[:, k].astype(np.int64)), W[:, k] / 255.0)
    Minv = np.linalg.inv(blend_mats(Wq, mats))
    P = np.einsum('nij,nj->ni', Minv, np.c_[PA, np.ones(len(PA))])[:, :3]
    return P, weld_normals(P, I), B, W, d['UV'], I


def preview(path, P, I, UV, B, W, joints, tex):
    th, tw = tex.shape[:2]
    c = UV[I].mean(1)
    col = tex[np.clip((c[:, 1] * th).astype(int), 0, th - 1), np.clip((c[:, 0] * tw).astype(int), 0, tw - 1)].astype(float)
    fn = np.cross(P[I[:, 1]] - P[I[:, 0]], P[I[:, 2]] - P[I[:, 0]]); fn /= np.maximum(1e-12, np.linalg.norm(fn, axis=1, keepdims=True))
    S = 1000
    def view(ax, sgn, zoom, cy, W_, mode=0, cx=0.0):
        im = Image.new('RGB', (W_, S), (40, 40, 48)); dr = ImageDraw.Draw(im); sc = S / 2.15 * zoom
        dax = 2 if ax == 0 else 0
        L = np.zeros(3); L[dax] = -sgn; L[1] = 0.5; L[ax] = 0.35; L /= np.linalg.norm(L)
        sh = 0.55 + 0.45 * np.clip(fn @ L, 0, 1)
        for t in np.argsort(P[I][:, :, dax].mean(1) * sgn)[::-1]:
            if fn[t][dax] * sgn > 0: continue
            if mode == 1:
                bone = int(B[I[t][0]][np.argmax(W[I[t][0]])]); cc = ((bone * 67) % 200 + 55, (bone * 131) % 200 + 55, (bone * 29) % 200 + 55)
            else: cc = tuple(int(v) for v in np.clip(col[t] * sh[t], 0, 255))
            dr.polygon([(W_ / 2 + (P[k, ax] - cx) * sc * (sgn if ax == 0 else -sgn), S / 2 - (P[k, 1] - cy) * sc) for k in I[t]], fill=cc)
        if zoom < 2:
            for jp in joints:
                x = W_ / 2 + jp[ax] * sc * (sgn if ax == 0 else -sgn); y = S / 2 - (jp[1] - cy) * sc
                dr.ellipse([x - 3, y - 3, x + 3, y + 3], outline=(0, 255, 0))
        return im
    views = [view(0, 1, 1.0, 1.02, 1000), view(2, 1, 1.0, 1.02, 520), view(0, -1, 1.0, 1.02, 1000), view(0, 1, 1.0, 1.02, 1000, 1),
             view(0, 1, 5.0, 1.70, 700), view(2, 1, 5.0, 1.70, 700, cx=-0.03)]
    out = Image.new('RGB', (sum(v.width for v in views), S)); x = 0
    for v in views: out.paste(v, (x, 0)); x += v.width
    out.save(path)


def main():
    prev = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None
    if '--no-bake' not in sys.argv:
        for key, cfg in MODELS.items():
            r = subprocess.run(['flatpak', 'run', '--filesystem=host', 'org.blender.Blender', '-b', '-P', os.path.join(ROOT, 'tools', 'tripo_bake.py'), '--',
                                os.path.join(SRC, cfg['file']), WORK, key], capture_output=True, text=True)
            print(cfg['label'], [l for l in r.stdout.splitlines() if l.startswith(('LOD', 'scan'))])
            if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); sys.exit(1)
    yP, yW = load_ybot()
    rig = '--no-rig' not in sys.argv
    b = open(os.path.join(OUT, 'ybot.e0a'), 'rb').read(); nb = struct.unpack('<I', b[4:8])[0]
    inv = np.frombuffer(b, np.float32, nb * 16, 12 + ((nb + 3) & ~3)).reshape(nb, 4, 4)
    joints = np.array([np.linalg.inv(inv[i].T)[:3, 3] for i in range(nb)])
    for key, cfg in MODELS.items():
        for lod in range(3):
            d = np.load(os.path.join(WORK, '%s_lod%d.npz' % (key, lod)))
            img = fill_holes(Image.open(os.path.join(WORK, '%s_lod%d.png' % (key, lod))))
            P, N, B, W, UV, I = fit(key, cfg, d, yP, yW, np.asarray(img), joints)
            name = 'pirate_%s%s' % (key, '' if lod == 0 else '_lod%d' % lod)
            write_e0m(os.path.join(OUT, name + '.e0m'), P, N, B, W, UV, I)
            write_e0i(os.path.join(OUT, name + '.e0i'), img)
            print('%s: %d vertices, %d triangulos, altura %.2f m, textura %dx%d' % (name, len(P), len(I), P[:, 1].max(), img.width, img.height))
            if prev and lod == 0:
                os.makedirs(prev, exist_ok=True)
                preview(os.path.join(prev, name + '.png'), P, I, UV, B, W, joints, np.asarray(img))

    if rig:   # ESTÁGIO C: pesos de pele FEITOS NO BLENDER (Automatic Weights no armature real do Y Bot) no lugar dos calculados aqui; --poses grava as poses de teste
        r = subprocess.run(['flatpak', 'run', '--filesystem=host', 'org.blender.Blender', '-b', '-P', os.path.join(ROOT, 'tools', 'tripo_rig.py'), '--', ROOT,
                            os.path.join(ROOT, 'build', 'tripo', 'rig'), '--write'] + (['--poses'] if prev else []), capture_output=True, text=True)
        print('\n'.join(l for l in r.stdout.splitlines() if l.startswith(('ossos', 'pirate_', 'vertices sem'))))
        if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); sys.exit(1)


main()
