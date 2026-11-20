#!/usr/bin/env python3
# Converte o navio pirata (GLB do Sketchfab) em assets/ship.e0s para a engine. Roda só com numpy + PIL (sem Blender):
#   python3 tools/export_ship.py [/caminho/pirate_ship.glb] [saida.e0s]
# Depois de converter, chama build/shipbake (compilado por build_server.sh), que corrige as normais invertidas do GLB e grava a oclusão de ambiente.
#
# O GLB usa uma TEXTURA-PALETA 8x8 (poucas cores chapadas): cada vértice recebe a cor do texel do seu UV (cor de vértice, zero textura em runtime).
# Eixos: o modelo tem proa em +X, "para cima" em +Y e direita em +Z (mão direita). A engine usa frente = -Z, direita = +X:
#   x_e = Z_m,  y_e = (Y_m - WATERLINE) * SCALE,  z_e = -X_m * SCALE   (rotação própria de 90° em torno de Y: o sentido dos triângulos se mantém)
# => no espaço local do navio: +a (proa) = -Z, +b (estibordo/direita) = +X, linha d'água em y = 0, convés em y = kShipDeckY (shared/ship.h).
# Formato E0S1: "E0S1" | nv u32 | ni u32 | nv x {f32 x,y,z; i8 nx,ny,nz,0; u8 r,g,b,a} (20 B) | ni x u32 índices.
import io, json, struct, subprocess, sys
import numpy as np
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else '/home/neto/Downloads/pirate_ship.glb'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'assets/ship.e0s'
SCALE = 0.8          # 41 m do modelo -> ~33 m
WATERLINE = 3.9      # y do modelo onde fica a faixa escura da linha d'água (vira y = 0)

f = open(SRC, 'rb').read()
jl = struct.unpack('<I', f[12:16])[0]
j = json.loads(f[20:20 + jl])
binb = f[20 + jl + 8:]


def acc(i):
    a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
    dt = {5126: np.float32, 5123: np.uint16, 5125: np.uint32}[a['componentType']]
    o = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    return np.frombuffer(binb, dtype=dt, count=a['count'] * n, offset=o).reshape(a['count'], n)


def mat(n):
    return np.array(n['matrix']).reshape(4, 4).T if 'matrix' in n else np.eye(4)


W = mat(j['nodes'][0]) @ mat(j['nodes'][1]) @ mat(j['nodes'][3])   # Sketchfab_model * fbx * Cube
im = j['images'][0]; bv = j['bufferViews'][im['bufferView']]
tex = np.array(Image.open(io.BytesIO(binb[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']])).convert('RGB'))
th, tw = tex.shape[:2]

P, N, C, I = [], [], [], []
base = 0
for mi in (0, 1):
    p = j['meshes'][mi]['primitives'][0]
    pos = acc(p['attributes']['POSITION']).astype(np.float64); nor = acc(p['attributes']['NORMAL']).astype(np.float64)
    uv = acc(p['attributes']['TEXCOORD_0']).astype(np.float64); idx = acc(p['indices']).astype(np.int64).reshape(-1, 3)
    ph = (np.c_[pos, np.ones(len(pos))] @ W.T)[:, :3]
    nn = nor @ np.linalg.inv(W[:3, :3]).T
    nn /= np.maximum(np.linalg.norm(nn, axis=1), 1e-9)[:, None]
    u = np.clip(np.floor((uv[:, 0] % 1.0) * tw).astype(int), 0, tw - 1)
    v = np.clip(np.floor((uv[:, 1] % 1.0) * th).astype(int), 0, th - 1)
    P.append(ph); N.append(nn); C.append(tex[v, u]); I.append(idx + base); base += len(pos)
P = np.vstack(P); N = np.vstack(N); C = np.vstack(C); I = np.vstack(I)

# a roda do leme do modelo (em x -11,0..-10,4; y 8,5..10,0; z ~0) é tirada: a engine desenha um timão que GIRA com o leme no mesmo lugar (Renderer::buildWheel)
cen = P[I].mean(1)
keep = ~((cen[:, 0] > -11.15) & (cen[:, 0] < -10.3) & (cen[:, 1] > 8.45) & (cen[:, 1] < 10.15) & (np.abs(cen[:, 2]) < 0.95))
print('triangulos da roda removidos:', int((~keep).sum()))
I = I[keep]

# modelo -> engine
R = np.array([[0, 0, 1], [0, 1, 0], [-1, 0, 0]], dtype=np.float64)   # (Xm,Ym,Zm) -> (Zm, Ym, -Xm)
Pe = P @ R.T
Pe[:, 1] -= WATERLINE
Pe *= SCALE
Ne = N @ R.T

# solda vértices idênticos (posição, normal, cor)
q = np.c_[np.round(Pe * 1000).astype(np.int64), np.round(Ne * 100).astype(np.int64), C.astype(np.int64)]
_, first, inv = np.unique(q, axis=0, return_index=True, return_inverse=True)
inv = inv.reshape(-1)
Pe, Ne, C = Pe[first], Ne[first], C[first]
I = inv[I]
# descarta triângulos degenerados
a, b, c = I[:, 0], I[:, 1], I[:, 2]
I = I[(a != b) & (b != c) & (a != c)]
nv, ni = len(Pe), I.size
print('vertices', nv, 'triangulos', ni // 3, 'bbox', Pe.min(0).round(2), Pe.max(0).round(2))

RAW = 'build/ship_raw.e0s'
with open(RAW, 'wb') as o:
    o.write(b'E0S1'); o.write(struct.pack('<II', nv, ni))
    nb = np.clip(np.round(Ne * 127), -127, 127).astype(np.int8)
    for k in range(nv):
        o.write(struct.pack('<3f4b4B', Pe[k, 0], Pe[k, 1], Pe[k, 2], nb[k, 0], nb[k, 1], nb[k, 2], 0, C[k, 0], C[k, 1], C[k, 2], 255))
    o.write(I.astype('<u4').tobytes())
subprocess.run(['build/shipbake', RAW, OUT], check=True)
print('ESCRITO', OUT)
