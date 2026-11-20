#!/usr/bin/env python3
# PIRATAS -> engine: grava malhas, texturas e o ESQUELETO PRÓPRIO de cada um a partir do rig feito no Blender (tools/rig_pirates.py).
#   python3 tools/export_rig.py [--blender]      (--blender roda antes o rig no Blender Flatpak: fora do sandbox)
# Saída (assets/): pirate_<m|f>[_lod1|_lod2].e0m (E0M3, vértices NA POSE A do scan = pose de bind, pesos do Blender),
#                  pirate_<m|f>[_lod1|_lod2].e0i (texturas assadas por tools/tripo_bake.py), pirate_<m|f>.e0k (esqueleto, ver Skeleton em anim.h).
# A engine usa a MESMA conta das prévias do Blender (pirate_skel.pose_globals): o que se viu lá é o que roda no jogo.
import os, struct, subprocess, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pirate_skel as ps

RIG = os.path.join(ROOT, 'build', 'rig', 'bl')
OUT = os.path.join(ROOT, 'assets')
# ajustes das peças presas ao corpo (espaço do mundo, em repouso): costas mais grossas que as do Y Bot (camisa, bandoleira, cabelo)
BACK = {'m': 0.042, 'f': 0.06}


def write_e0m(path, P, N, B, W, UV, I):
    nv = len(P)
    v = np.zeros(nv, dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('b', 'u1', 4), ('w', 'u1', 4), ('uv', '<f4', 2)])
    v['p'] = P; v['n'] = N; v['b'] = B; v['w'] = W; v['uv'] = UV
    with open(path, 'wb') as f:
        f.write(b'E0M3'); f.write(struct.pack('<I', 1))
        f.write(struct.pack('<3fII', 1.0, 1.0, 1.0, nv, I.size))
        f.write(v.tobytes()); f.write(I.astype('<u4').tobytes())


def fill_holes(img):
    """texels que o bake não acertou (pretos puros) ganham a cor do texel válido mais próximo (sem preto sangrando nos mipmaps)."""
    from scipy.ndimage import distance_transform_edt
    a = np.asarray(img.convert('RGB')).copy()
    hole = a.max(2) < 6
    if hole.any() and not hole.all():
        _, (iy, ix) = distance_transform_edt(hole, return_indices=True)
        a = a[iy, ix]
    return Image.fromarray(a)


def write_e0i(path, png):
    rgba = fill_holes(Image.open(png)).convert('RGBA')
    with open(path, 'wb') as f:
        f.write(b'E0I1'); f.write(struct.pack('<HH', *rgba.size)); f.write(rgba.tobytes())


def colmaj(M): return M.T.astype('<f4').tobytes()


def fist_pose():
    src = open(os.path.join(ROOT, 'engine', 'src', 'fist_pose.h')).read()
    rows = [l.strip().strip('{},') for l in src.splitlines() if l.strip().startswith('{-') or l.strip().startswith('{0') or l.strip().startswith('{-0')]
    vals = [[float(x.rstrip('f')) for x in r.split(',') if x.strip()] for r in rows]
    vals = [v for v in vals if len(v) == 7]
    assert len(vals) == 20, len(vals)
    return np.array(vals)


def finger_centroid(par, T, Q, first, last, hand):
    """centro das juntas dos dedos no espaço local da mão (pose local T/Q por osso)."""
    G = {hand: np.eye(4)}; pts = []
    for i in range(first, last + 1):
        L = np.eye(4); L[:3, :3] = ps.qmat(Q[i]); L[:3, 3] = T[i]
        G[i] = G[par[i]] @ L; pts.append(G[i][:3, 3])
    return np.mean(pts, 0)


def skeleton(key):
    P = ps.load_lod(key, 0)[0]
    sk = ps.build(key, P)
    par, GY = sk['par'], sk['GY']
    invY = np.linalg.inv(GY)
    rt, rq = sk['rest']
    A = [np.eye(4) for _ in range(65)]
    # mão direita: pegada no centro do punho FECHADO do pirata (a mão dele é menor que a do Y Bot)
    fp = fist_pose(); Qf = rq.copy(); Tf_y = rt.copy(); Tf_p = sk['T'].copy()
    Qf[35:55] = fp[:, 3:7]; Tf_y[35:55] = fp[:, 0:3]
    d = finger_centroid(par, Tf_p, Qf, 35, 54, 34) - finger_centroid(par, Tf_y, Qf, 35, 54, 34)
    A[34][:3, 3] = d
    d = finger_centroid(par, sk['T'], rq, 11, 30, 10) - finger_centroid(par, rt, rq, 11, 30, 10)
    A[10][:3, 3] = d
    # costas (itens guardados, corpo carregado) e topo da cabeça (TAG)
    top_p = P[:, 1].max() - sk['J'][5, 1]; top_y = 0.204
    for b, dw in ((3, np.array([0, 0, BACK[key]])), (5, np.array([0, top_p - top_y, 0]))):
        A[b][:3, 3] = GY[b][:3, :3].T @ dw
    att = [A[i] @ invY[i] for i in range(65)]
    blob = bytearray(b'E0K1') + struct.pack('<I', 65)
    for i in range(65): blob += colmaj(np.linalg.inv(sk['B'][i]))
    blob += sk['T'].astype('<f4').tobytes()
    blob += sk['rootRest'].astype('<f4').tobytes() + sk['ybotRoot'].astype('<f4').tobytes() + struct.pack('<f', sk['hipS'])
    for i in range(65): blob += colmaj(att[i])
    open(os.path.join(OUT, 'pirate_%s.e0k' % key), 'wb').write(bytes(blob))
    print('%s: esqueleto  quadril %.3f m (x%.3f)  mao x%.2f  pegada %s cm' % (key, sk['J'][0, 1], sk['hipS'], sk['sh'], np.round(A[34][:3, 3] * 100, 1)))
    return sk


if __name__ == '__main__':
    if '--blender' in sys.argv:
        subprocess.run(['flatpak', 'run', '--filesystem=host', 'org.blender.Blender', '-b', '-P', os.path.join(ROOT, 'tools', 'rig_pirates.py'),
                        '--', ROOT, RIG, '--save'], check=True)
    for key in 'mf':
        skeleton(key)
        r = np.load(os.path.join(RIG, '%s_rig.npz' % key))
        for lod in range(3):
            P, N, UV, I = ps.load_lod(key, lod)
            name = 'pirate_%s%s' % (key, '' if lod == 0 else '_lod%d' % lod)
            write_e0m(os.path.join(OUT, name + '.e0m'), P, N, r['B%d' % lod], r['Q%d' % lod], UV, I.reshape(-1))
            write_e0i(os.path.join(OUT, name + '.e0i'), os.path.join(ps.WORK, '%s_lod%d.png' % (key, lod)))
            print('  %s: %d vertices, %d triangulos' % (name, len(P), len(I)))
