# ESQUELETO PRÓPRIO dos piratas (numpy puro; usado por tools/rig_pirates.py dentro do Blender e pelas prévias no PC).
#
# Os piratas NÃO são mais deformados para o corpo do Y Bot. Cada um tem o seu esqueleto de 65 ossos (mesmos nomes/ordem do Mixamo = ybot.e0a)
# com as juntas DENTRO do corpo dele, na pose A do scan (a malha fica exatamente como foi modelada). A orientação de repouso de cada osso é a do Y Bot
# girada pelo menor giro que leva a direção do osso do Y Bot à direção do osso do pirata (braço em pose A, pernas abertas etc.). Assim as rotações
# locais dos clipes Mixamo (feitos para o Y Bot) valem sem conversão: só a translação local de cada osso (comprimento) é a do pirata e a do quadril
# é escalada pela altura do quadril (ver Skeleton em engine/src/anim.h).
#
# Unidades: as juntas abaixo (LM) estão no espaço do SCAN (build/tripo/out/<m|f>_lod<k>.npz: Y para cima, frente +Z, ~0,98 de altura; o lado
# DIREITO do personagem fica em x NEGATIVO). Espaço da ENGINE: metros, Y para cima, frente -Z, direita +X  ->  e = S * (-x, y, -z).
import os, struct
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'build', 'tripo', 'out')

# Marcos medidos nas vistas de frente/lado do scan (build/rig/z_<m|f>_*.png). Lado DIREITO (x negativo no scan); o esquerdo é o espelho.
# y das juntas da coluna vem de 'hips'..'head' na proporção do Y Bot. z de cada junta: centro da seção do corpo naquele ponto (zsec).
LM = {
    'm': dict(S=1.86, hips=0.515, head=0.862, top=0.978,
              clav=(-0.030, 0.785), arm=(-0.106, 0.762), wrist=(-0.311, 0.590), tip=(-0.356, 0.523), elbow_f=0.56,
              upleg=(-0.055, 0.495), knee=(-0.075, 0.258), ankle=(-0.085, 0.058)),
    'f': dict(S=1.83, hips=0.535, head=0.858, top=0.985,
              clav=(-0.028, 0.782), arm=(-0.096, 0.764), wrist=(-0.278, 0.594), tip=(-0.309, 0.521), elbow_f=0.56,
              upleg=(-0.060, 0.515), knee=(-0.098, 0.260), ankle=(-0.118, 0.058)),
}

NAMES = ['Hips', 'Spine', 'Spine1', 'Spine2', 'Neck', 'Head', 'HeadTop_End']
for side in ('Left', 'Right'):
    NAMES += [side + 'Shoulder', side + 'Arm', side + 'ForeArm', side + 'Hand']
    for f in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        NAMES += ['%sHand%s%d' % (side, f, k) for k in range(1, 5)]
for side in ('Left', 'Right'):
    NAMES += [side + 'UpLeg', side + 'Leg', side + 'Foot', side + 'ToeBase', side + 'Toe_End']
assert len(NAMES) == 65


def load_e0a(path=os.path.join(ROOT, 'assets', 'ybot.e0a')):
    """ybot.e0a: pais, juntas de repouso G (4x4, espaço da engine), pose de repouso local e clipes (t, q xyzw)."""
    b = open(path, 'rb').read(); nb, nc = struct.unpack('<II', b[4:12]); o = 12
    par = np.frombuffer(b, np.int8, nb, o).astype(int); o += (nb + 3) & ~3
    inv = np.frombuffer(b, '<f4', nb * 16, o).reshape(nb, 4, 4).transpose(0, 2, 1).astype(np.float64); o += nb * 64

    def pose(o, n):
        a = np.frombuffer(b, np.uint8, n * 20, o).reshape(n, 20)
        t = a[:, :12].copy().view('<f4').reshape(n, 3).astype(np.float64)
        q = a[:, 12:].copy().view('<i2').reshape(n, 4) / 32767.0
        return t, q / np.linalg.norm(q, axis=1, keepdims=True)
    rest = pose(o, nb); o += nb * 20; clips = []
    for _ in range(nc):
        name = b[o:o + 16].split(b'\0')[0].decode(); fps, nf, lp = struct.unpack('<fII', b[o + 16:o + 28]); o += 28
        t, q = pose(o, nf * nb); o += nf * nb * 20
        clips.append(dict(name=name, fps=fps, loop=lp, t=t.reshape(nf, nb, 3), q=q.reshape(nf, nb, 4)))
    return par, np.linalg.inv(inv), rest, clips


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def rot_between(a, b):
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b); v = np.cross(a, b); c = float(a @ b)
    if c < -0.9999: raise ValueError('direções opostas')
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K / (1 + c)


def load_lod(key, lod):
    """malha do scan (pose A) já no espaço da ENGINE: P, N, UV, I."""
    d = np.load(os.path.join(WORK, '%s_lod%d.npz' % (key, lod)))
    S = LM[key]['S']; F = np.array([-1.0, 1.0, -1.0])
    return d['P'] * F * S, d['N'] * F, d['UV'], d['I']


def zsec(P, x, y, r=0.035, back=0.0):
    """z (engine) do centro da seção do corpo em volta de (x, y) (engine); back = fração da espessura para TRÁS (+z)."""
    m = (np.abs(P[:, 0] - x) < r) & (np.abs(P[:, 1] - y) < r)
    if m.sum() < 4: return zsec(P, x, y, r * 1.6, back)
    z0, z1 = P[m, 2].min(), P[m, 2].max()
    return 0.5 * (z0 + z1) + back * (z1 - z0)


def build(key, P=None):
    """juntas J (65x3), quadros de repouso B (65x4x4: rotação + junta) e dados para a engine, no espaço da engine."""
    L = LM[key]; S = L['S']
    if P is None: P = load_lod(key, 0)[0]
    par, GY, rest, clips = load_e0a()
    JY = GY[:, :3, 3]; OY = GY[:, :3, :3]
    e = lambda xy: np.array([-xy[0] * S, xy[1] * S])            # marco do lado DIREITO (scan) -> engine (x positivo)
    J = np.zeros((65, 3))
    # coluna: alturas na proporção do Y Bot entre o quadril (0.998) e a cabeça (1.601)
    hy, hd = L['hips'] * S, L['head'] * S
    for i in range(6):
        y = hy + (JY[i, 1] - JY[0, 1]) / (JY[5, 1] - JY[0, 1]) * (hd - hy)
        J[i] = [0.0, y, zsec(P, 0.0, y, 0.05, 0.12 if 1 <= i <= 3 else 0.0)]
    # pescoço e cabeça: medidos pela FRENTE (garganta/queixo), não pelo centro da seção (o cabelo atrás puxava as juntas para a nuca)
    def front(y, r=0.025):
        m = (np.abs(P[:, 1] - y) < r) & (np.abs(P[:, 0]) < 0.035)
        return P[m, 2].min()
    J[4, 2] = front(J[4, 1]) + 0.060
    J[5, 2] = front(J[5, 1]) + 0.095
    J[6] = [0.0, L['top'] * S, J[5, 2]]
    # braços (índices do lado direito: 31 ombro, 32 braço, 33 antebraço, 34 mão; esquerdo = espelho 7..10)
    c, a, w, t = e(L['clav']), e(L['arm']), e(L['wrist']), e(L['tip'])
    el = a + L['elbow_f'] * (w - a)
    R = {}
    for idx, xy, r in ((31, c, 0.04), (32, a, 0.04), (33, el, 0.03), (34, w, 0.022)):
        J[idx] = [xy[0], xy[1], zsec(P, xy[0], xy[1], r)]
    # ombro: profundidade do PRÓPRIO braço (seção 7 cm para dentro do braço, longe do tronco), não a do tronco
    ad = (el - a) / np.linalg.norm(el - a); q = a + 0.10 * ad
    J[32, 2] = zsec(P, q[0], q[1], 0.02)
    J[31, 2] = 0.5 * (J[31, 2] + J[32, 2])
    tip3 = np.array([t[0], t[1], zsec(P, t[0], t[1], 0.02)])
    # pernas (60 coxa, 61 joelho, 62 tornozelo, 63 base dos dedos, 64 ponta)
    for idx, nm, r in ((60, 'upleg', 0.04), (61, 'knee', 0.035), (62, 'ankle', 0.03)):
        xy = e(L[nm]); J[idx] = [xy[0], xy[1], zsec(P, xy[0], xy[1], r)]
    foot = P[(P[:, 1] < 0.05 * S) & (np.abs(P[:, 0] - J[62, 0]) < 0.07 * S)]
    zf, zb = foot[:, 2].min(), foot[:, 2].max()                 # ponta (frente = -z) e calcanhar
    flen = zb - zf
    J[62, 2] = zb - 0.27 * flen
    J[63] = [J[62, 0], 0.028 * S, zf + 0.30 * flen]
    J[64] = [J[62, 0], 0.025 * S, zf + 0.04 * flen]
    # mão: dedos do Y Bot girados pela direção pulso->ponta e escalados pelo tamanho da mão
    dY = JY[46] - JY[34]; dP = tip3 - J[34]
    Rh = rot_between(dY, dP); sh = np.linalg.norm(dP) / np.linalg.norm(dY)
    for i in range(35, 55): J[i] = J[34] + sh * Rh @ (JY[i] - JY[34])
    # lado esquerdo = espelho
    mir = np.array([-1.0, 1.0, 1.0])
    for i in range(31, 55): J[i - 24] = J[i] * mir
    for i in range(60, 65): J[i - 5] = J[i] * mir
    # orientação: Y Bot girado pelo menor giro que leva a direção do osso do Y Bot à do pirata
    child = {1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 7: 8, 8: 9, 9: 10, 31: 32, 32: 33, 33: 34, 55: 56, 56: 57, 57: 58, 58: 59, 60: 61, 61: 62, 62: 63, 63: 64}
    Rb = [np.eye(3)] * 65
    for i in range(65):
        if i in child:
            Rb[i] = rot_between(JY[child[i]] - JY[i], J[child[i]] - J[i])
        elif i == 34: Rb[i] = Rh
        elif i == 10: Rb[i] = np.diag(mir) @ Rh @ np.diag(mir)
        elif par[i] >= 0: Rb[i] = Rb[par[i]]
    B = np.zeros((65, 4, 4))
    for i in range(65):
        B[i, :3, :3] = Rb[i] @ OY[i]; B[i, :3, 3] = J[i]; B[i, 3, 3] = 1
    T = np.zeros((65, 3))                                         # translação local de cada osso (no quadro do pai)
    for i in range(1, 65):
        p = par[i]; T[i] = B[p, :3, :3].T @ (J[i] - J[p])
    hipS = J[0, 1] / rest[0][0, 1]
    return dict(J=J, B=B, T=T, par=par, hipS=hipS, rootRest=J[0].copy(), ybotRoot=rest[0][0].copy(), sh=sh, GY=GY, clips=clips, rest=rest)


def pose_globals(sk, t, q):
    """quadros globais (65x4x4) do pirata para uma pose local do clipe (t, q do Y Bot) - MESMA conta que a engine (AnimBank::computeSkinning)."""
    par = sk['par']; G = np.zeros((65, 4, 4))
    for i in range(65):
        L = np.eye(4); L[:3, :3] = qmat(q[i])
        L[:3, 3] = sk['rootRest'] + sk['hipS'] * (t[0] - sk['ybotRoot']) if i == 0 else sk['T'][i]
        G[i] = L if par[i] < 0 else G[par[i]] @ L
    return G


def skin_mats(sk, G):
    return np.einsum('bij,bjk->bik', G, np.linalg.inv(sk['B']))
