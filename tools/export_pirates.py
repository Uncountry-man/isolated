#!/usr/bin/env python3
# PIRATAS SEMI-REALISTAS (personagens do jogo; substituíram o Y Bot).
# Corpo e rosto: "Cowboy" e "Cowgirl" do mastjie (poly.pizza, CC0): proporções reais, músculos, 5 dedos e rosto esculpido (nariz, lábios, orelhas),
# ~4,3 mil triângulos, pose A (braços abaixados ~51 graus), textura-paleta de 4 cores. Ficam em `import assets/pirate_kit/`. O resultado fica PRESO AO
# ESQUELETO DO Y BOT (assets/ybot.e0a): animações, ferramenta na mão, tag, cápsulas de acerto... não mudam. SEM CHAPÉU (pedido do usuário).
#
# O que o script faz (numpy + PIL + scipy; o Blender só decima o LOD2):
#   1. lê o GLB, solda os vértices e classifica cada triângulo pela cor da paleta original;
#   2. APAGA o chapéu de cowboy e FECHA o crânio com uma calota (testa na cor da pele + cabelo), costurada na borda do corte;
#   3. LOD0: subdivisão de LOOP (4x triângulos, superfície suavizada) -> ~17 mil triângulos; LOD1: resolução original;
#   4. deforma NA PRÓPRIA POSE A para as juntas do Y Bot COM OS BRAÇOS NA MESMA POSE A (corpo: mapas lineares por partes na altura/lateral, pernas
#      endireitadas; braços: mapa ao longo do eixo do braço: ombro, cotovelo, pulso, ponta dos dedos);
#   5. PESOS DE PELE: põe a malha do Y Bot (import assets/ybot_ref/ybot.e0m) na mesma pose A (gira a cadeia do braço em volta da junta do ombro),
#      copia os pesos do vértice mais próximo, soma os dedos na mão (mão rígida), cabeça rígida;
#   6. DESFAZ A POSE com esses mesmos pesos (inversa da mistura de matrizes do skinning) -> malha na pose T de repouso do esqueleto. Assim, quando a
#      animação abaixa os braços, a malha volta EXATAMENTE à forma em que foi modelada (antes os braços eram levantados por um giro aproximado que não
#      batia com os pesos: o ombro "quebrava");
#   7. normais suaves, REPINTA por região (pele, camisa, colete, faixa, calça, botas, fivela, barba, cabelo) e cria OLHOS e SOBRANCELHAS;
#   8. grava assets/pirate_<m|f>.e0m (LOD0), _lod1 (original), _lod2 (Blender: 50% do LOD1) + assets/pirate_<m|f>.e0i (paleta 64x64).
# Uso: python3 tools/export_pirates.py [--preview pasta] [--no-lods]      (fora do sandbox por causa do Blender)
import io, json, os, struct, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'import assets', 'pirate_kit')
OUT = os.path.join(ROOT, 'assets')
WORK = os.path.join(ROOT, 'build', 'pirates')

# Juntas do Y Bot no espaço do jogo (m): ver assets/ybot.e0a (invBind).
YB = dict(ankle=0.10, knee=0.53, crotch=0.87, shoulder=1.425, neck=1.525, chin=1.585, brow=1.715,
          sh_x=0.19, elbow_x=0.462, wrist_x=0.738, tip_x=0.95, leg_x=0.093, back_z=0.04)
HAND_R, HAND_L, HEAD = 34, 10, 5

# Cores novas (sRGB 0..255)
C = dict(skin_m=(214, 164, 128), skin_f=(232, 186, 150), shirt=(232, 224, 205), blouse=(242, 238, 228), navy=(40, 54, 96), wine=(138, 30, 40),
         sash=(170, 36, 42), pants_m=(60, 50, 46), pants_f=(52, 58, 86), boots=(34, 28, 26), gold=(214, 168, 62), glove=(72, 46, 32),
         hair_f=(128, 52, 26), beard=(58, 40, 30), hat_m=(30, 28, 32), hat_f=(74, 46, 32), eye_w=(238, 234, 226), iris_m=(70, 48, 30), iris_f=(60, 92, 78),
         brow=(50, 34, 26), lips=(176, 96, 92), hair_m=(62, 42, 30))
PALETTE = list(C.keys())

ARM = dict(elbow=0.274, wrist=0.550, tip=0.775)   # Y Bot: distância da junta do ombro (0.188, 1.436, 0.083) ao cotovelo, pulso e ponta dos dedos
SHOULDER = np.array([0.188, 1.436, 0.083])

MODELS = {
    'm': dict(file='mj_cowboy.glb', hat_cut=lambda cls, cx, cy, cz: cy > 2.005, top=2.125,
              y=[(0.0, 0.0), (0.10, YB['ankle']), (0.57, YB['knee']), (1.02, YB['crotch']), (1.71, 1.436), (1.80, YB['neck']), (1.88, YB['chin']), (2.03, YB['brow'])],
              arm=dict(pivot=(0.23, 1.71), angle=50.8, elbow=0.24, wrist=0.47, tip=0.74),
              leg_shift=0.150, depth_k=0.85, depth_c=0.03, eye_y=1.668, eye_x=0.031, iris='iris_m', hair='hair_m'),
    'f': dict(file='mj_cowgirl.glb', top=1.985,   # chapéu marrom + a parte de baixo da aba (creme, mais larga que a cabeça)
              hat_cut=lambda cls, cx, cy, cz: ((cls == 'brown') & (cy > 1.84)) | (cy > 1.905) | ((cy > 1.885) & (np.hypot(cx, cz - 0.03) > 0.105)),
              y=[(0.0, 0.0), (0.08, YB['ankle']), (0.51, YB['knee']), (0.96, YB['crotch']), (1.58, 1.436), (1.68, YB['neck']), (1.745, YB['chin']), (1.90, YB['brow'])],
              arm=dict(pivot=(0.185, 1.58), angle=52.6, elbow=0.205, wrist=0.415, tip=0.635),
              leg_shift=0.100, depth_k=0.93, depth_c=0.02, eye_y=1.668, eye_x=0.030, iris='iris_f', hair='hair_f'),
}


def quat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def load_glb(path):
    """-> lista de partes (nome do nó, P, N, I, cor por triângulo em sRGB 0..255), já com as transformações dos nós."""
    d = open(path, 'rb').read()
    n = struct.unpack('<I', d[12:16])[0]
    j = json.loads(d[20:20 + n]); bin_ = d[20 + n + 8:]
    def acc(i):
        a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        ct = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}[a['componentType']]
        nc = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        isz = np.dtype(ct).itemsize * nc; st = bv.get('byteStride', 0) or isz
        raw = np.frombuffer(bin_, np.uint8, st * (a['count'] - 1) + isz, off)
        idx = (np.arange(a['count'])[:, None] * st + np.arange(isz)[None, :]).ravel()
        return raw[idx].view(ct).reshape(a['count'], nc)
    imgs = []
    for im in j.get('images', []):
        bv = j['bufferViews'][im['bufferView']]
        imgs.append(np.asarray(Image.open(io.BytesIO(bin_[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']])).convert('RGB')))
    parts = []
    def walk(ni, M):
        nd = j['nodes'][ni]
        if 'matrix' in nd: L = np.array(nd['matrix']).reshape(4, 4).T
        else:
            T = np.eye(4); T[:3, 3] = nd.get('translation', [0, 0, 0]); R = np.eye(4); R[:3, :3] = quat(nd.get('rotation', [0, 0, 0, 1]))
            L = T @ R @ np.diag(list(nd.get('scale', [1, 1, 1])) + [1])
        G = M @ L
        if 'mesh' in nd:
            me = j['meshes'][nd['mesh']]
            for p in me['primitives']:
                P = acc(p['attributes']['POSITION']).astype(np.float64); P = (np.c_[P, np.ones(len(P))] @ G.T)[:, :3]
                N = acc(p['attributes']['NORMAL']).astype(np.float64) @ np.linalg.inv(G[:3, :3])   # normal: inversa transposta
                I = acc(p['indices']).astype(np.int64).reshape(-1, 3)
                pbr = j['materials'][p.get('material', 0)].get('pbrMetallicRoughness', {})
                if 'baseColorTexture' in pbr and imgs:
                    t = imgs[j['textures'][pbr['baseColorTexture']['index']]['source']]; th, tw = t.shape[:2]
                    uv = acc(p['attributes']['TEXCOORD_0']).astype(np.float64)[I].mean(1)
                    col = t[np.clip((uv[:, 1] % 1 * th).astype(int), 0, th - 1), np.clip((uv[:, 0] % 1 * tw).astype(int), 0, tw - 1)].astype(np.float64)
                else:
                    col = np.tile(255 * np.power(np.array(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3]), 1 / 2.2), (len(I), 1))
                parts.append((me['name'], P, N, I, col))
        for c in nd.get('children', []): walk(c, G)
    for r in j['scenes'][j.get('scene', 0)]['nodes']: walk(r, np.eye(4))
    return parts


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


def weld(P, I):
    key = np.round(P * 20000).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True); inv = inv.reshape(-1)
    F = inv[I]
    ok = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    return P[first], F[ok], ok


def close_head(P, I, cls, top_y):
    """fecha o buraco deixado pelo chapéu: calota costurada na MAIOR borda aberta acima do pescoço (anéis que sobem até o topo da cabeça).
    Os 2 primeiros anéis da frente ficam na cor da pele (testa); o resto é cabelo."""
    e = np.sort(np.concatenate([I[:, [0, 1]], I[:, [1, 2]], I[:, [2, 0]]]), 1)
    ue, cnt = np.unique(e, axis=0, return_counts=True)
    be = ue[cnt == 1]
    be = be[P[be].mean(1)[:, 1] > top_y - 0.35]
    adj = {}
    for a, b in be: adj.setdefault(int(a), []).append(int(b)); adj.setdefault(int(b), []).append(int(a))
    loops = []; seen = set()
    for st in adj:
        if st in seen: continue
        loop = [st]; seen.add(st); prev = None; cur = st
        while True:
            nxt = [v for v in adj[cur] if v != prev and v not in seen]
            if not nxt: break
            prev, cur = cur, nxt[0]; loop.append(cur); seen.add(cur)
        loops.append(loop)
    loop = max(loops, key=len)
    L = P[loop]; c = L.mean(0)
    # sentido: a calota precisa olhar para fora (para cima): ordena o anel no sentido em que a normal do leque aponta para +Y
    area = np.cross(L - c, np.roll(L, -1, 0) - c)[:, 1].sum()
    if area < 0: loop = loop[::-1]; L = P[loop]   # no plano XZ, +Y para cima pede giro negativo (z x x): normal = (b-a) x (c-a)
    n = len(loop); R = 5
    newP = [P]; rings = [np.array(loop)]; base = len(P)
    for r in range(1, R):
        ph = r / R * np.pi / 2
        ring = np.stack([c[0] + (L[:, 0] - c[0]) * np.cos(ph) ** 0.8, L[:, 1] + (top_y - L[:, 1]) * np.sin(ph), c[2] + (L[:, 2] - c[2]) * np.cos(ph) ** 0.8], 1)
        newP.append(ring); rings.append(np.arange(base, base + n)); base += n
    apex = base; newP.append(np.array([[c[0], top_y, c[2]]]))
    tris = []; tcls = []
    for r in range(R - 1):
        a, b = rings[r], rings[r + 1]
        for k in range(n):
            k2 = (k + 1) % n
            front = L[k, 2] > c[2] + 0.02 and r < 2
            for t in ((a[k], b[k], a[k2]), (a[k2], b[k], b[k2])): tris.append(t); tcls.append('forehead' if front else 'hair')
    for k in range(n): tris.append((rings[-1][k], apex, rings[-1][(k + 1) % n])); tcls.append('hair')
    T = np.array(tris)
    # confere o sentido pelo primeiro triângulo (normal para fora do centro da cabeça)
    PP = np.concatenate(newP)
    nrm = np.cross(PP[T[:, 1]] - PP[T[:, 0]], PP[T[:, 2]] - PP[T[:, 0]]); out = (PP[T].mean(1) - [c[0], c[1] - 0.05, c[2]])
    if (np.einsum('ij,ij->i', nrm, out) < 0).mean() > 0.5: T = T[:, [0, 2, 1]]
    return PP, np.concatenate([I, T]), np.concatenate([cls, np.array(tcls, dtype=object)])


def loop_subdivide(P, I, cls):
    """1 nível de subdivisão de Loop (malha soldada): cada triângulo vira 4 (herdam a cor); bordas abertas/arestas com mais de 2 faces não suavizam."""
    nv = len(P)
    e = np.concatenate([I[:, [0, 1]], I[:, [1, 2]], I[:, [2, 0]]]); opp = np.concatenate([I[:, 2], I[:, 0], I[:, 1]])
    es = np.sort(e, 1)
    ue, inv, cnt = np.unique(es, axis=0, return_inverse=True, return_counts=True); inv = inv.reshape(-1)
    osum = np.zeros((len(ue), 3)); np.add.at(osum, inv, P[opp])
    mid = (P[ue[:, 0]] + P[ue[:, 1]]) * 0.5
    interior = cnt == 2
    ep = np.where(interior[:, None], 0.375 * (P[ue[:, 0]] + P[ue[:, 1]]) + 0.125 * osum, mid)
    # vértices antigos
    nb = np.zeros(nv); nsum = np.zeros((nv, 3)); bcount = np.zeros(nv); bsum = np.zeros((nv, 3))
    for a, b in ((0, 1), (1, 0)):
        np.add.at(nb, ue[:, a], 1); np.add.at(nsum, ue[:, a], P[ue[:, b]])
        np.add.at(bcount, ue[~interior][:, a], 1); np.add.at(bsum, ue[~interior][:, a], P[ue[~interior][:, b]])
    n = np.maximum(nb, 3)
    beta = (0.625 - (0.375 + 0.25 * np.cos(2 * np.pi / n)) ** 2) / n
    vin = (1 - nb * beta)[:, None] * P + beta[:, None] * nsum
    vb = 0.75 * P + 0.125 * bsum
    newP = np.where((bcount == 0)[:, None], vin, np.where((bcount == 2)[:, None], vb, P))
    m = inv.reshape(3, -1).T + nv   # ponto novo das arestas (0-1, 1-2, 2-0) de cada triângulo
    T = np.concatenate([np.stack([I[:, 0], m[:, 0], m[:, 2]], 1), np.stack([I[:, 1], m[:, 1], m[:, 0]], 1),
                        np.stack([I[:, 2], m[:, 2], m[:, 1]], 1), np.stack([m[:, 0], m[:, 1], m[:, 2]], 1)])
    return np.concatenate([newP, ep]), T, np.concatenate([cls] * 4)


def conform_a(P, cfg):
    """P no espaço do modelo (m; X lateral, Y altura, Z frente=+), NA POSE A -> espaço do jogo (direita = +X, frente = -Z), TAMBÉM na pose A,
    com as juntas nas posições do Y Bot (braços abaixados em `angle` graus em volta da junta do ombro)."""
    lat = np.abs(P[:, 0]); sgn = np.sign(P[:, 0]); y = P[:, 1]; dep = P[:, 2]
    ar = cfg['arm']; th = np.radians(ar['angle'])
    d = np.array([np.cos(th), -np.sin(th)]); nrm = np.array([np.sin(th), np.cos(th)])
    # corpo
    Yb = pw(y, cfg['y'])
    Lb = pw(lat, [(0.0, 0.0), (ar['pivot'][0], SHOULDER[0])])
    below = np.clip(1.0 - Yb / YB['crotch'], 0, 1)
    Lb = Lb - np.minimum(Lb * 0.80, cfg['leg_shift'] * below) * (Lb < 0.5)
    # braço: coordenadas ao longo do eixo (t) e perpendicular (s)
    rx = lat - ar['pivot'][0]; ry = y - ar['pivot'][1]
    t = rx * d[0] + ry * d[1]; sp = rx * nrm[0] + ry * nrm[1]
    ta = pw(t, [(0.0, 0.0), (ar['elbow'], ARM['elbow']), (ar['wrist'], ARM['wrist']), (ar['tip'], ARM['tip'])])
    ta = np.where(t < 0, t * cfg['depth_k'], ta)
    La = SHOULDER[0] + d[0] * ta + nrm[0] * sp * cfg['depth_k']
    Ya = SHOULDER[1] + d[1] * ta + nrm[1] * sp * cfg['depth_k']
    w = smooth(t, -0.03, 0.09) * (sp > -0.13) * (lat > ar['pivot'][0] - 0.07)
    L = Lb * (1 - w) + La * w; Y = Yb * (1 - w) + Ya * w
    Z = -(dep - cfg['depth_c']) * cfg['depth_k'] + YB['back_z']
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


def skin_weights(P, I, yP, yW):
    tree = cKDTree(yP)
    d, idx = tree.query(P, k=4)
    wk = 1.0 / np.maximum(d, 1e-4) ** 2; wk /= wk.sum(1, keepdims=True)
    W = (yW[idx] * wk[:, :, None]).sum(1)
    W[:, HAND_R] += W[:, 35:55].sum(1); W[:, 35:55] = 0
    W[:, HAND_L] += W[:, 11:31].sum(1); W[:, 11:31] = 0
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


def quantize(W):
    top = np.argsort(-W, axis=1)[:, :4]
    w4 = np.take_along_axis(W, top, 1); w4 /= w4.sum(1, keepdims=True)
    q = np.floor(w4 * 255 + 0.5).astype(np.int64)
    q[:, 0] += 255 - q.sum(1)
    top[q == 0] = 0
    return top.astype(np.uint8), q.astype(np.uint8)


def classify(col):
    ref = {'cream': (255, 216, 176), 'brown': (80, 56, 40), 'red': (104, 0, 0), 'green': (0, 48, 0)}
    names = list(ref); R = np.array([ref[k] for k in names], float)
    return np.array(names)[np.argmin(((col[:, None, :] - R[None]) ** 2).sum(2), 1)]


def recolor(key, cfg, cls, cen):
    """cor nova de cada triângulo: cls = cor original (ou 'hair'/'forehead' da calota), cen = centro no espaço do jogo (pose T)."""
    X = np.abs(cen[:, 0]); Y = cen[:, 1]; Z = cen[:, 2]
    out = np.full(len(cls), 'pants_m' if key == 'm' else 'pants_f', dtype=object)
    cream = cls == 'cream'; brown = cls == 'brown'
    skin = 'skin_' + key
    if key == 'm':
        out[cream] = 'navy'                                   # o colete vira colete azul-marinho
        out[cream & (X > 0.24)] = 'shirt'                     # punhos
        out[cream & (Y > 1.535)] = skin
        out[brown & (Y > 1.0)] = 'shirt'                      # gola e mangas: camisa de linho
        out[brown & (X > 0.745)] = skin                       # as luvas viram mãos
        out[brown & (Y <= 0.44)] = 'boots'
        out[brown & (Y > 0.955) & (Y < 1.045) & (X < 0.30)] = 'sash'   # faixa vermelha na cintura
        out[cls == 'red'] = 'gold'
        jaw = cream & (Y > 1.578) & (Y < 1.632) & (Z < 0.055)  # barba curta: do queixo até abaixo do nariz, na frente e nos lados
        out[jaw & ~((X < 0.021) & (Y > 1.612) & (Y < 1.628) & (Z < -0.03))] = 'beard'
    else:
        out[cream] = 'blouse'
        out[cream & (Y > 1.535)] = skin
        out[brown & (Y > 1.0)] = 'wine'                       # gola e gravata viram lenço cor de vinho
        out[brown & (X > 0.70)] = 'glove'
        out[brown & (Y <= 0.47)] = 'boots'
        out[cls == 'green'] = 'sash'
        out[cls == 'red'] = 'gold'
        out[(cls == 'red') & (Y > 1.5)] = 'hair_f'            # o coque (vermelho-escuro no original)
    out[cls == 'hair'] = cfg['hair']
    out[cls == 'forehead'] = skin
    return out


def ray_front(P, I, x, y):
    """z do primeiro ponto da malha visto de frente (-Z) na linha (x, y); None se não acerta."""
    a, b, c = P[I[:, 0]], P[I[:, 1]], P[I[:, 2]]
    def e(p, q, x, y): return (q[:, 0] - p[:, 0]) * (y - p[:, 1]) - (q[:, 1] - p[:, 1]) * (x - p[:, 0])
    w0, w1, w2 = e(b, c, x, y), e(c, a, x, y), e(a, b, x, y)
    area = w0 + w1 + w2
    hit = ((w0 >= 0) & (w1 >= 0) & (w2 >= 0) | (w0 <= 0) & (w1 <= 0) & (w2 <= 0)) & (np.abs(area) > 1e-12)
    if not hit.any(): return None
    z = (w0[hit] * a[hit, 2] + w1[hit] * b[hit, 2] + w2[hit] * c[hit, 2]) / area[hit]
    return z.min()


def face_parts(P, I, cfg):
    """olhos (esclera + íris) e sobrancelhas: pequenos leques de triângulos colados na superfície do rosto. -> lista (P, I, cor)."""
    out = []
    def disc(cx, cy, rx, ry, color, lift, n=8, tilt=0.0):
        ang = np.arange(n) * 2 * np.pi / n
        px = cx + np.cos(ang) * rx * np.cos(tilt) - np.sin(ang) * ry * np.sin(tilt)
        py = cy + np.cos(ang) * rx * np.sin(tilt) + np.sin(ang) * ry * np.cos(tilt)
        pts = [(cx, cy)] + list(zip(px, py))
        zs = [ray_front(P, I, x, y) for x, y in pts]
        if any(z is None for z in zs): return
        V = np.array([(x, y, z - lift) for (x, y), z in zip(pts, zs)])
        F = np.array([(0, 1 + (k + 1) % n, 1 + k) for k in range(n)])   # de frente para -Z
        out.append((V, F, color))
    for s in (-1, 1):
        ex = s * cfg['eye_x']; ey = cfg['eye_y']
        disc(ex, ey, 0.0135, 0.0062, 'eye_w', 0.0015)
        disc(ex + s * 0.001, ey + 0.0004, 0.0052, 0.0054, cfg['iris'], 0.0030)
        disc(ex + s * 0.002, ey + 0.0185, 0.0165, 0.0034, 'brow', 0.0020, tilt=-s * 0.16)
    return out


def assemble(tris):
    """tris: lista de (P, N, B, W, I, cores por triângulo (nomes)) -> vértices únicos por (vértice, cor) com UV da paleta."""
    Ps, Ns, Bs, Ws, UVs, Is = [], [], [], [], [], []; base = 0
    for P, N, B, W, I, cn in tris:
        ci = np.array([PALETTE.index(c) for c in cn])
        key = I * 64 + ci[:, None]
        uniq, inv = np.unique(key.ravel(), return_inverse=True)
        vi = uniq // 64; cc = uniq % 64
        Ps.append(P[vi]); Ns.append(N[vi]); Bs.append(B[vi]); Ws.append(W[vi])
        UVs.append(np.stack([(cc % 8 + 0.5) / 8.0, (cc // 8 + 0.5) / 8.0], 1))
        Is.append(inv.reshape(-1, 3) + base); base += len(vi)
    return np.concatenate(Ps), np.concatenate(Ns), np.concatenate(Bs), np.concatenate(Ws), np.concatenate(UVs), np.concatenate(Is)


def palette_image():
    im = Image.new('RGB', (64, 64), (255, 0, 255)); d = ImageDraw.Draw(im)
    for i, k in enumerate(PALETTE): d.rectangle([i % 8 * 8, i // 8 * 8, i % 8 * 8 + 7, i // 8 * 8 + 7], fill=C[k])
    return im


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


def preview(path, P, I, UV, B, W, joints):
    pal = np.asarray(palette_image())
    col = pal[np.clip((UV[I][:, 0, 1] * 64).astype(int), 0, 63), np.clip((UV[I][:, 0, 0] * 64).astype(int), 0, 63)].astype(float)
    fn = np.cross(P[I[:, 1]] - P[I[:, 0]], P[I[:, 2]] - P[I[:, 0]]); fn /= np.maximum(1e-12, np.linalg.norm(fn, axis=1, keepdims=True))
    S = 1000
    def view(ax, sgn, zoom, cy, W_, mode=0, cx=0.0):
        im = Image.new('RGB', (W_, S), (40, 40, 48)); dr = ImageDraw.Draw(im); sc = S / 2.15 * zoom
        dax = 2 if ax == 0 else 0
        L = np.zeros(3); L[dax] = -sgn; L[1] = 0.5; L[ax] = 0.35; L /= np.linalg.norm(L)
        sh = 0.5 + 0.5 * np.clip(fn @ L, 0, 1)
        for t in np.argsort(P[I][:, :, dax].mean(1) * sgn)[::-1]:
            if fn[t][dax] * sgn > 0: continue
            if mode == 1:
                bone = int(B[I[t][0]][np.argmax(W[I[t][0]])]); c = ((bone * 67) % 200 + 55, (bone * 131) % 200 + 55, (bone * 29) % 200 + 55)
            else: c = tuple(int(v) for v in col[t] * sh[t])
            dr.polygon([(W_ / 2 + (P[k, ax] - cx) * sc * (sgn if ax == 0 else -sgn), S / 2 - (P[k, 1] - cy) * sc) for k in I[t]], fill=c)
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


def build(key, cfg, yP, yW, subdiv):
    (_, P0, _N0, I0, col), = load_glb(os.path.join(SRC, cfg['file']))
    P0 = P0 * 100.0   # o arquivo vem em metros / 100
    cls = classify(col).astype(object)
    Pw, Iw, ok = weld(P0, I0); cls = cls[ok]
    cen0 = Pw[Iw].mean(1)
    keep = ~cfg['hat_cut'](cls, cen0[:, 0], cen0[:, 1], cen0[:, 2])
    Iw = Iw[keep]; cls = cls[keep]
    used = np.unique(Iw); remap = -np.ones(len(Pw), np.int64); remap[used] = np.arange(len(used)); Pw = Pw[used]; Iw = remap[Iw]
    Pw, Iw, cls = close_head(Pw, Iw, cls, cfg['top'])
    if subdiv: Pw, Iw, cls = loop_subdivide(Pw, Iw, cls)
    PA = conform_a(Pw, cfg)                                   # pose A, juntas do Y Bot
    mats = arm_mats(cfg['arm']['angle'])
    yPA = np.einsum('nij,nj->ni', blend_mats(yW, mats), np.c_[yP, np.ones(len(yP))])[:, :3]   # o Y Bot na mesma pose A
    B, W = quantize(skin_weights(PA, Iw, yPA, yW))
    Wq = np.zeros((len(PA), 65))
    for k in range(4): np.add.at(Wq, (np.arange(len(PA)), B[:, k].astype(np.int64)), W[:, k] / 255.0)
    Minv = np.linalg.inv(blend_mats(Wq, mats))                # desfaz a pose com os MESMOS pesos do skinning -> pose T de repouso
    P = np.einsum('nij,nj->ni', Minv, np.c_[PA, np.ones(len(PA))])[:, :3]
    N = vertex_normals(P, Iw)
    cn = recolor(key, cfg, cls, P[Iw].mean(1))
    tris = [(P, N, B, W, Iw, cn)]
    hb = np.zeros((1, 4), np.uint8); hb[0, 0] = HEAD; hw = np.zeros((1, 4), np.uint8); hw[0, 0] = 255
    head_tris = Iw[(P[Iw][:, :, 1].min(1) > 1.56)]
    for V, F, color in face_parts(P, head_tris, cfg):
        Nf = np.tile([0.0, 0.0, -1.0], (len(V), 1))
        tris.append((V, Nf, np.repeat(hb, len(V), 0), np.repeat(hw, len(V), 0), F, np.array([color] * len(F))))
    return assemble(tris)


def recolor_lod(d, cen, cidx):
    """LOD decimado pelo Blender: a cor de cada triângulo volta a ser a do triângulo original mais próximo (o Decimate mistura as UVs da paleta)."""
    P, N, B, W, I = d['P'], d['N'], d['B'], d['W'], d['I']
    _, near = cKDTree(cen).query(P[I].mean(1))
    cn = np.array(PALETTE, dtype=object)[cidx[near]]
    return assemble([(P, N, B, W, I, cn)])


def main():
    os.makedirs(WORK, exist_ok=True)
    prev = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None
    yP, yW = load_ybot()
    b = open(os.path.join(OUT, 'ybot.e0a'), 'rb').read(); nb = struct.unpack('<I', b[4:8])[0]
    inv = np.frombuffer(b, np.float32, nb * 16, 12 + ((nb + 3) & ~3)).reshape(nb, 4, 4)
    joints = np.array([np.linalg.inv(inv[i].T)[:3, 3] for i in range(nb)])
    pal = palette_image()
    orig = {}
    for f in os.listdir(WORK):
        if f.endswith('.npz'): os.remove(os.path.join(WORK, f))
    for key, cfg in MODELS.items():
        name = 'pirate_' + key
        P, N, B, W, UV, I = build(key, cfg, yP, yW, True)       # LOD0: subdividido
        write_e0m(os.path.join(OUT, name + '.e0m'), P, N, B, W, UV, I)
        write_e0i(os.path.join(OUT, name + '.e0i'), pal)
        print('%s: %d vertices, %d triangulos, altura %.2f m' % (name, len(P), len(I), P[:, 1].max()))
        if prev:
            os.makedirs(prev, exist_ok=True)
            preview(os.path.join(prev, name + '.png'), P, I, UV, B, W, joints)
        P, N, B, W, UV, I = build(key, cfg, yP, yW, False)      # LOD1: resolução original
        write_e0m(os.path.join(OUT, name + '_lod1.e0m'), P, N, B, W, UV, I)
        np.savez(os.path.join(WORK, name + '.npz'), P=P, N=N, B=B, W=W, UV=UV, I=I)
        cidx = (np.clip((UV[I][:, 0, 1] * 8).astype(int), 0, 7) * 8 + np.clip((UV[I][:, 0, 0] * 8).astype(int), 0, 7))
        orig[key] = (P[I].mean(1), cidx, (P, N, B, W, UV, I))
        print('  %s_lod1: %d triangulos' % (name, len(I)))
    if '--no-lods' not in sys.argv:
        script = os.path.join(ROOT, 'tools', 'pirate_lods.py')
        r = subprocess.run(['flatpak', 'run', '--filesystem=host', 'org.blender.Blender', '-b', '-P', script, '--', WORK], capture_output=True, text=True)
        print(r.stdout[-600:] if r.returncode else 'Blender: LOD2 decimado')
    for key in MODELS:
        name = 'pirate_' + key
        src = os.path.join(WORK, name + '_lod1.npz')   # o Blender grava 50% (_lod1) e 25% (_lod2) da malha que recebeu (o nosso LOD1): usamos o de 50%
        if os.path.exists(src): P, N, B, W, UV, I = recolor_lod(np.load(src), orig[key][0], orig[key][1])
        else: P, N, B, W, UV, I = orig[key][2]
        write_e0m(os.path.join(OUT, name + '_lod2.e0m'), P, N, B, W, UV, I)
        print('  %s_lod2: %d triangulos%s' % (name, len(I), '' if os.path.exists(src) else ' (copia do LOD1: sem Blender)'))

main()
