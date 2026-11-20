#!/usr/bin/env python3
# PIRATAS COM O RIG FEITO PELO USUÁRIO -> engine.
#   1) bake dos LODs/texturas no Blender (fora do sandbox; caminho SEM espaços, ex. o link /home/neto/isolink):
#        flatpak run --filesystem=host org.blender.Blender -b -P /home/neto/isolink/tools/tripo_bake.py -- GLB /home/neto/isolink/build/glbrig m|f|g|c 20000,4500,1600 1024,512,256
#   2) python3 tools/export_glb_rig.py [--bake] [--only g,c]   (--bake roda o passo 1 sozinho; --only: só esses modelos)
# Entrada: GLBs com a malha, o esqueleto de 65 ossos "mixamorig:*" e os PESOS já feitos pelo usuário (Downloads/tripo_convert_*.glb).
# NADA é recalculado: as juntas vêm dos ossos do GLB, os pesos do LOD0 são os do GLB (LOD1/LOD2 herdam pela superfície mais próxima).
# A engine continua igual: cada pirata tem o SEU esqueleto (pirate_<m|f>.e0k, ver Skeleton em anim.h) e as animações do ybot.e0a valem para ele
# (a orientação de repouso de cada osso é a do Y Bot girada pelo menor giro até a direção do osso do pirata: pirate_skel.py, mesma conta de antes;
# a orientação/roll dos ossos do Blender não importa).
# Saída (assets/): pirate_<m|f>[_lod1|_lod2].e0m (E0M3), .e0i (textura de cada LOD) e pirate_<m|f>.e0k.
import json, os, struct, subprocess, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pirate_skel as ps
import export_rig as er

OUT = os.path.join(ROOT, 'assets')
BAKE = os.path.join(ROOT, 'build', 'glbrig')
# Originais (do usuário, Tripo, rig feito por ele) em "import assets/tripo/new/". Chave = sufixo dos arquivos (assets/pirate_<chave>*).
# Modelo novo: GLB aqui + chave em GLB/SCALE/BACK + entrada em kCharModels/kCharFiles/kSkel (renderer.cpp) e skinTable (skins.h).
_SRC = os.path.join(ROOT, 'import assets', 'tripo', 'new')
GLB = {'m': os.path.join(_SRC, 'pirata_homem.glb'),       # 0 TOMÁS VIEIRA
       'f': os.path.join(_SRC, 'pirata_mulher.glb'),      # 1 ISABELA DUARTE
       'g': os.path.join(_SRC, 'pirata_gordo.glb'),       # 2 pirata gordo (comum)
       'c': os.path.join(_SRC, 'pirata_corsaria.glb')}    # 3 corsária de casaco e tricórnio (rara)
SCALE = {'m': 1.86, 'f': 1.84, 'g': 1.84, 'c': 1.82}      # GLB (~0,98 de altura) -> metros da engine
F = np.array([-1.0, 1.0, -1.0])     # glTF (frente +Z, esquerda +X) -> engine (frente -Z, direita +X)
BACK = {'m': 0.042, 'f': 0.06, 'g': 0.09, 'c': 0.06}
# --only g,c : processa só essas chaves (os assets dos outros não mudam)
KEYS = sys.argv[sys.argv.index('--only') + 1].split(',') if '--only' in sys.argv else list(GLB)


def read_glb(path):
    d = open(path, 'rb').read()
    l = struct.unpack('<I', d[12:16])[0]; j = json.loads(d[20:20 + l])
    o = 20 + l; bl = struct.unpack('<I', d[o:o + 4])[0]
    return j, d[o + 8:o + 8 + bl]


def accessor(j, b, i):
    a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    dt = {5126: '<f4', 5123: '<u2', 5125: '<u4', 5121: 'u1'}[a['componentType']]
    return np.frombuffer(b, dt, a['count'] * n, bv.get('byteOffset', 0) + a.get('byteOffset', 0)).reshape(a['count'], n)


def load_rig(key):
    """juntas (glTF, 65x3 na ordem de ps.NAMES) e os pesos por vértice do GLB (posição, ossos na ordem de ps.NAMES, pesos)."""
    j, b = read_glb(GLB[key]); N = j['nodes']; sk = j['skins'][0]
    par = {c: i for i, n in enumerate(N) for c in n.get('children', [])}

    def local(n):
        if 'matrix' in n: return np.array(n['matrix']).reshape(4, 4).T
        M = np.eye(4); M[:3, :3] = ps.qmat(n.get('rotation', [0, 0, 0, 1])) * np.array(n.get('scale', [1, 1, 1]))[None, :]
        M[:3, 3] = n.get('translation', [0, 0, 0]); return M
    cache = {}

    def glob(i):
        if i not in cache: cache[i] = (glob(par[i]) @ local(N[i])) if i in par else local(N[i])
        return cache[i]
    idx = {N[n]['name'].replace('mixamorig:', ''): n for n in sk['joints']}
    assert set(idx) == set(ps.NAMES), set(ps.NAMES) ^ set(idx)
    J = np.array([glob(idx[nm])[:3, 3] for nm in ps.NAMES])
    remap = np.array([ps.NAMES.index(N[n]['name'].replace('mixamorig:', '')) for n in sk['joints']])   # junta do skin -> osso da engine
    p = j['meshes'][0]['primitives'][0]; A = p['attributes']
    P = accessor(j, b, A['POSITION']).astype(np.float64)
    JT = remap[accessor(j, b, A['JOINTS_0']).astype(int)]; W = accessor(j, b, A['WEIGHTS_0']).astype(np.float64)
    W /= np.maximum(W.sum(1, keepdims=True), 1e-9)
    if key in CLEAN_ARMS:
        I = accessor(j, b, p['indices']).reshape(-1, 3).astype(int)
        JT, W = clean_arm_weights(P, I, JT, W, J)
    return J, P, JT, W


# Modelos em que as mãos encostam na roupa na pose de modelagem: o rig pôs um pouco de peso de BRAÇO/MÃO em pedaços da saia (corsária) e
# da faixa da cintura (gordo), que eram puxados junto com as mãos nas animações. Limpeza: o peso de braço (ossos 8..30 e 32..54) só fica nos
# vértices LIGADOS PELA MALHA (soldada por posição) ao braço/antebraço de verdade SEM PASSAR PELA MÃO (a mão encosta na roupa e ligava
# a saia ao braço); o pano com a mão como osso principal mas longe dos dedos (> HAND_R) também sai; os outros perdem o peso de braço e copiam os pesos do vértice "sem braço" mais próximo. A manga (peso de braço ~1) continua no braço.
CLEAN_ARMS = {'g', 'c'}
ARM_BONES = np.array(list(range(8, 31)) + list(range(32, 55)))


HAND_BONES = np.array(list(range(10, 31)) + list(range(34, 55)))
UPPER_BONES = np.array([7, 8, 9, 31, 32, 33])
HAND_R = 0.04       # a mão: até ~5,5 cm (unidades do GLB) do eixo pulso -> ponta de cada dedo
SKIRT_R = 0.05      # pano a mais de ~9 cm do eixo da perna (unidades do GLB) é saia/faixa solta
SKIRT_HIP = 0.6     # até 60% do peso de perna dele vai para o quadril


def clean_arm_weights(P, I, JT, W, J):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    from scipy.spatial import cKDTree
    JT = JT.copy(); W = W.copy()
    _, wid = np.unique(np.round(P * 1e5).astype(np.int64), axis=0, return_inverse=True); wid = wid.reshape(-1)
    isarm = np.isin(JT, ARM_BONES) & (W > 0)
    armw = (W * isarm).sum(1)
    dom = JT[np.arange(len(P)), W.argmax(1)]                       # osso de maior peso
    hand = np.isin(dom, HAND_BONES)                                # mão/dedos: é por ela que a roupa encosta no braço
    n = wid.max() + 1
    selw = np.zeros(n, bool); selw[wid[(armw > 0) & ~hand]] = True
    e = wid[np.concatenate([I[:, [0, 1]], I[:, [1, 2]], I[:, [2, 0]]])]
    m = selw[e[:, 0]] & selw[e[:, 1]]
    _, lab = connected_components(coo_matrix((np.ones(m.sum()), (e[m, 0], e[m, 1])), shape=(n, n)), directed=False)
    core = np.unique(lab[wid[(armw > 0.5) & np.isin(dom, UPPER_BONES)]])   # braço/antebraço/manga de verdade
    ok = np.isin(lab[wid], core) & selw[wid]
    # a mão em si continua; peso de braço fora do braço (sem passar pela mão) sai
    y = P[:, 1]
    bad = (armw > 0) & ~ok & ~hand
    # MÃO de verdade (protegida): perto do eixo pulso -> ponta de cada um dos 5 dedos
    def sd(X, a, b):
        ab = b - a; t = np.clip(((X - a) @ ab) / max(ab @ ab, 1e-12), 0, 1)
        return np.linalg.norm(X - (a + t[:, None] * ab), axis=1)
    dh = np.full(len(P), 1e9)
    for wr, tips in ((10, (14, 18, 22, 26, 30)), (34, (38, 42, 46, 50, 54))):
        for tp in tips: dh = np.minimum(dh, sd(P, J[wr], J[tp]))
    inhand = dh < HAND_R
    bad |= (armw > 0) & hand & ~inhand          # pano com a MÃO como osso principal mas longe dos dedos (saia encostada na mão)
    clean = armw == 0
    _, ix = cKDTree(P[clean]).query(P[bad])
    src = np.flatnonzero(clean)[ix]
    JT[bad] = JT[src]; W[bad] = W[src]
    print('  pesos de braço removidos de %d vértices soltos (saia/faixa)' % bad.sum())
    # MANGA com peso de PERNA (a barra da manga encosta na coxa): vértice de braço (peso de braço >= 0,5) não segue perna
    armw = (W * np.isin(JT, ARM_BONES)).sum(1)
    legm = JT >= 55
    sl = (armw >= 0.5) & (W * legm).sum(1).astype(bool)
    W[sl] = np.where(legm[sl], 0.0, W[sl]); W[sl] /= W[sl].sum(1, keepdims=True)
    print('  peso de perna tirado de %d vértices da manga' % sl.sum())
    # SAIA: o pano longe da perna segue metade a coxa e metade o quadril (antes abria em "asas" quando a perna subia)
    def seg_d(X, a, b):
        ab = b - a; t = np.clip(((X - a) @ ab) / max(ab @ ab, 1e-12), 0, 1)
        return np.linalg.norm(X - (a + t[:, None] * ab), axis=1)
    dleg = np.minimum(np.minimum(seg_d(P, J[55], J[56]), seg_d(P, J[56], J[57])), np.minimum(seg_d(P, J[60], J[61]), seg_d(P, J[61], J[62])))
    legw = (W * legm).sum(1)
    sk = (legw > 0) & (armw < 0.5) & (dleg > SKIRT_R) & (P[:, 1] > J[56, 1])   # só da altura do joelho para cima (botas e canelas ficam como estão)
    k = np.clip((dleg[sk] - SKIRT_R) / SKIRT_R, 0, 1) * SKIRT_HIP   # quanto mais longe da perna, mais segue o quadril
    for v, kk in zip(np.flatnonzero(sk), k):
        moved = 0.0
        for c in range(4):
            if JT[v, c] >= 55: moved += W[v, c] * kk; W[v, c] *= 1 - kk
        c0 = np.flatnonzero(JT[v] == 0)
        if len(c0): W[v, c0[0]] += moved
        else:
            c = int(W[v].argmin()); W[v, c] = 0; JT[v, c] = 0; W[v, c] = moved; W[v] /= W[v].sum()
    print('  saia: %d vértices passam a seguir em parte o quadril' % sk.sum())
    return JT, W


def build_from_joints(J):
    """o mesmo que ps.build(), mas com as juntas dadas (engine): quadros de repouso B, translação local T, escala do quadril."""
    par, GY, rest, clips = ps.load_e0a()
    JY = GY[:, :3, 3]; OY = GY[:, :3, :3]
    mir = np.array([-1.0, 1.0, 1.0])
    dY = JY[46] - JY[34]; dP = J[46] - J[34]
    Rh = ps.rot_between(dY, dP); sh = np.linalg.norm(dP) / np.linalg.norm(dY)
    child = {1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 7: 8, 8: 9, 9: 10, 31: 32, 32: 33, 33: 34, 55: 56, 56: 57, 57: 58, 58: 59, 60: 61, 61: 62, 62: 63, 63: 64}
    Rb = [np.eye(3)] * 65
    for i in range(65):
        if i in child: Rb[i] = ps.rot_between(JY[child[i]] - JY[i], J[child[i]] - J[i])
        elif i == 34: Rb[i] = Rh
        elif i == 10: Rb[i] = np.diag(mir) @ Rh @ np.diag(mir)
        elif par[i] >= 0: Rb[i] = Rb[par[i]]
    B = np.zeros((65, 4, 4))
    for i in range(65):
        B[i, :3, :3] = Rb[i] @ OY[i]; B[i, :3, 3] = J[i]; B[i, 3, 3] = 1
    T = np.zeros((65, 3))
    for i in range(1, 65): T[i] = B[par[i], :3, :3].T @ (J[i] - J[par[i]])
    return dict(J=J, B=B, T=T, par=par, hipS=J[0, 1] / rest[0][0, 1], rootRest=J[0].copy(), ybotRoot=rest[0][0].copy(), sh=sh, GY=GY, clips=clips, rest=rest,
                tilt=[np.degrees(np.arccos(np.clip((np.trace(Rb[i]) - 1) / 2, -1, 1))) for i in range(65)])


def write_skeleton(key, sk, P):
    par, GY = sk['par'], sk['GY']; invY = np.linalg.inv(GY); rt, rq = sk['rest']
    A = [np.eye(4) for _ in range(65)]
    fp = er.fist_pose(); Qf = rq.copy(); Tf_y = rt.copy(); Tf_p = sk['T'].copy()
    Qf[35:55] = fp[:, 3:7]; Tf_y[35:55] = fp[:, 0:3]
    A[34][:3, 3] = er.finger_centroid(par, Tf_p, Qf, 35, 54, 34) - er.finger_centroid(par, Tf_y, Qf, 35, 54, 34)
    A[10][:3, 3] = er.finger_centroid(par, sk['T'], rq, 11, 30, 10) - er.finger_centroid(par, rt, rq, 11, 30, 10)
    top_p = P[:, 1].max() - sk['J'][5, 1]; top_y = 0.204
    for b, dw in ((3, np.array([0, 0, BACK[key]])), (5, np.array([0, top_p - top_y, 0]))):
        A[b][:3, 3] = sk['GY'][b][:3, :3].T @ dw
    att = [A[i] @ invY[i] for i in range(65)]
    blob = bytearray(b'E0K1') + struct.pack('<I', 65)
    for i in range(65): blob += er.colmaj(np.linalg.inv(sk['B'][i]))
    blob += sk['T'].astype('<f4').tobytes()
    blob += sk['rootRest'].astype('<f4').tobytes() + sk['ybotRoot'].astype('<f4').tobytes() + struct.pack('<f', sk['hipS'])
    for i in range(65): blob += er.colmaj(att[i])
    open(os.path.join(OUT, 'pirate_%s.e0k' % key), 'wb').write(bytes(blob))


def transfer_weights(P0, JT0, W0, P):
    """pesos do LOD0 (do GLB) para os vértices de outra malha: 4 vértices mais próximos, peso por 1/d^2 (distância ~0 = cópia exata)."""
    from scipy.spatial import cKDTree
    d, ix = cKDTree(P0).query(P, k=4)
    w = 1.0 / np.maximum(d, 1e-5) ** 2
    exact = d[:, 0] < 1e-5
    w[exact] = 0; w[exact, 0] = 1
    w /= w.sum(1, keepdims=True)
    B = np.zeros((len(P), 4), np.uint8); Wt = np.zeros((len(P), 4), np.uint8)
    for v in range(len(P)):
        acc = {}
        for k in range(4):
            for b, ww in zip(JT0[ix[v, k]], W0[ix[v, k]] * w[v, k]):
                if ww > 0: acc[b] = acc.get(b, 0) + ww
        top = sorted(acc.items(), key=lambda t: -t[1])[:4]
        s = sum(t[1] for t in top)
        q = np.array([t[1] / s * 255 for t in top]); r = np.floor(q).astype(int); r[0] += 255 - r.sum()
        for k, (b, _) in enumerate(top): B[v, k] = b; Wt[v, k] = r[k]
    return B, Wt


def main():
    if '--bake' in sys.argv:
        link = '/home/neto/isolink'
        for key in KEYS:
            subprocess.run(['flatpak', 'run', '--filesystem=host', 'org.blender.Blender', '-b', '-P', link + '/tools/tripo_bake.py', '--', GLB[key].replace(ROOT, link),
                            link + '/build/glbrig', key, '20000,4500,1600', '1024,512,256'], check=True)
    for key in KEYS:
        S = SCALE[key]
        Jg, Pg, JTg, Wg = load_rig(key)
        J = Jg * F * S
        sk = build_from_joints(J)
        worst = sorted(((t, ps.NAMES[i]) for i, t in enumerate(sk['tilt'])), reverse=True)[:8]
        print('%s: altura %.2f m  quadril %.3f m (x%.3f)  mao x%.2f  maiores giros de repouso: %s' % (
            key, Pg[:, 1].max() * S, J[0, 1], sk['hipS'], sk['sh'], ', '.join('%s %.0f°' % (n, t) for t, n in worst)))
        Pe = Pg * F * S
        write_skeleton(key, sk, Pe)
        for lod in range(3):
            d = np.load(os.path.join(BAKE, '%s_lod%d.npz' % (key, lod)))
            P = d['P'] * F * S; N = d['N'] * F; UV = d['UV']; I = d['I']
            Bn, Wn = transfer_weights(Pe, JTg, Wg, P)
            name = 'pirate_%s%s' % (key, '' if lod == 0 else '_lod%d' % lod)
            er.write_e0m(os.path.join(OUT, name + '.e0m'), P, N, Bn, Wn, UV, I.reshape(-1))
            er.write_e0i(os.path.join(OUT, name + '.e0i'), os.path.join(BAKE, '%s_lod%d.png' % (key, lod)))
            print('  %s: %d vertices, %d triangulos' % (name, len(P), len(I)))
        np.savez(os.path.join(BAKE, '%s_rig.npz' % key), J=J, B=sk['B'], T=sk['T'], par=sk['par'], hipS=sk['hipS'], rootRest=sk['rootRest'], ybotRoot=sk['ybotRoot'])


if __name__ == '__main__':
    main()
    # RAM/APK: texturas dos piratas viram ETC2 (E0I2); ver tools/compress_textures.py
    import subprocess
    subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'compress_textures.py')], check=True)
