# Exporta o esqueleto (65 ossos) do Y Bot e os clipes de animação dos Packs para o formato binário .e0a (roda no Blender).
# Uso: blender -b -P tools/export_anims.py -- [saida.e0a]
# Formato E0A1 (little-endian, alinhado em 4 bytes):
#   "E0A1" | u32 nBones (65) | u32 nClips
#   int8 parents[nBones] + pad[3]
#   float invBind[nBones * 16] (column-major 4x4, espaço Engine)
#   BonePose restPose[nBones]  (float tx,ty,tz + int16 qx,qy,qz,qw = 20 B)
#   Por clipe (nClips):
#     char name[16] | float fps | u32 nFrames | u32 loop
#     BonePose frames[nFrames * nBones]
import bpy, os, sys, struct, math
from mathutils import Matrix, Vector, Quaternion

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
dst = args[0] if args else os.path.join(ROOT, 'assets', 'ybot.e0a')
ybot_fbx = os.path.join(ROOT, 'import assets', 'Y Bot.fbx')

# Clipes usados atualmente no gameplay:
#   0 IDLE    : parado respirando
#   1 WALK    : andando (sincronizado em fase com RUN)
#   2 RUN     : correndo (sincronizado em fase com WALK)
#   3 JUMP    : impulso inicial do pulo (subida)
#   4 FALL    : no ar / caindo (loop)
#   5 LAND    : amortecimento de pouso no chão
#   6 BATER   : soco 1 (esquerdo)
#   7 BATER2  : soco 2 (gancho direito)
#   8 BATER3  : soco 3 (direto cruzado direito)
#   9 CORTAR  : golpe horizontal em árvore/tora (botão CORTAR)
#  10 MINERAR : golpe vertical em pedra (botão MINERAR)
#  11 COLETAR : abaixar, pegar no chão e levar a mão ao bolso/cintura ao levantar (botão COLETAR)
#  12 BOIAR   : boiando na água / mantendo-se na superfície (Treading Water)
#  13 NADAR   : nadando (ciclo contínuo extraído de Swimming To Edge)
#  14..17     : TMINE, AIDLE, AWALK, ARUN (ferramentas na mão, ver abaixo)
CLIPS = [
    ('IDLE',    '_Packs/Locomotion Pack/idle.fbx',                                   30.0, 1, None),
    ('WALK',    '_Packs/Locomotion Pack/walking.fbx',                                30.0, 1, None),
    ('RUN',     '_Packs/Locomotion Pack/running.fbx',                                30.0, 1, None),
    ('JUMP',    '_Packs/Action Adventure Pack/jumping up.fbx',                       30.0, 0, None),
    ('FALL',    '_Packs/Pro Longbow Pack/fall a loop.fbx',                           30.0, 1, None),
    ('LAND',    '_Packs/Pro Longbow Pack/fall a land to standing idle 01.fbx',       32.0, 0, None),
    ('BATER',   '_Packs/Pro Longbow Pack/standing melee punch.fbx',                  35.0, 0, None),
    ('BATER2',  'Right Hook.fbx',                                                    36.0, 0, None),
    ('BATER3',  'Cross Punch.fbx',                                                   45.0, 0, 'cross_trim'),
    # CORTAR / MINERAR: golpes do Pro Melee Axe Pack (mesmo pack da pegada das ferramentas), aparados p/ começar e terminar na
    # pose de guarda e acelerados (o original tem ~2,4 s).
    ('CORTAR',  '_Packs/Pro Melee Axe Pack/standing melee attack horizontal.fbx',    42.0, 0, 'trim:3:62'),
    ('MINERAR', '_Packs/Pro Melee Axe Pack/standing melee attack downward.fbx',      42.0, 0, 'trim:3:60'),
    ('COLETAR', '_Packs/Farming Pack/pull plant.fbx',                                60.0, 0, 'pickup_pocket'),
    ('BOIAR',   'Treading Water.fbx',                                                30.0, 1, 'swim_idle'),
    ('NADAR',   'Swimming To Edge.fbx',                                              30.0, 1, 'swim_loop'),
    # Ferramentas na mão (ver tools/export_tools.py: a pegada e os dedos vêm do mesmo Pro Melee Axe Pack):
    #  14 TMINE : picareta - ergue com as duas mãos por cima da cabeça e crava no chão à frente (combo attack ver. 2, f62..108)
    #  15 AIDLE : braço direito segurando a ferramenta parado (camada só dos ossos 31..54 sobre PARADO)
    #  16 AWALK : idem andando (camada sobre ANDAR, mesma fase)
    #  17 ARUN  : idem correndo (camada sobre CORRER, mesma fase)
    ('TMINE',   '_Packs/Pro Melee Axe Pack/standing melee combo attack ver. 2.fbx',  34.0, 0, 'trim:62:108'),
    ('AIDLE',   '_Packs/Pro Melee Axe Pack/standing idle.fbx',                       30.0, 1, None),
    ('AWALK',   '_Packs/Pro Melee Axe Pack/standing walk forward.fbx',               30.0, 1, None),
    ('ARUN',    '_Packs/Pro Melee Axe Pack/standing run forward.fbx',                30.0, 1, None),
    ('MORRER',  '_Packs/Pro Longbow Pack/standing death backward 01.fbx',            30.0, 0, None),   # morte: cai desmaiado (fica no último quadro)
    ('RESGATE', 'Administering Cpr.fbx',                                             30.0, 1, None),   # quem SALVA um jogador caído (RCP, em loop)
    ('LEVANTAR','Getting Up.fbx',                                                    30.0, 0, None),   # o jogador salvo levanta do chão
    ('BRACO',   'Hand Raising.fbx',                                                  30.0, 0, 'pose_carrier'),   # POSE: braço direito de quem CARREGA, dobrado segurando a perna
    ('OMBRO',   'Hand Raising.fbx',                                                  30.0, 0, 'pose_carried'),   # POSE: corpo dobrado sobre o ombro direito (já no espaço de quem carrega)
    # BACAMARTE (arma de fogo, tools/export_gun.py): Pro Rifle Pack (todos os clipes seguram o rifle no MESMO lugar em relação às mãos; in place).
    #  23 GIDLE: parado com a arma (pronto, cano para a frente)   24 GAIM: mirando parado (coronha no ombro)
    #  25..28 GWALK/GBACK/GLEFT/GRIGHT: andar mirando em 4 direções (o corpo fica de frente para a mira; mistura por direção)
    #  29 GRUN: correndo com a arma   30 GRELOAD: recarregar (camada do tronco; Reloading.fbx acelerado p/ ~2,2 s)
    ('GIDLE',   '_Packs/Pro Rifle Pack/idle.fbx',                                    30.0, 1, None),
    ('GAIM',    '_Packs/Pro Rifle Pack/idle aiming.fbx',                             30.0, 1, None),
    ('GWALK',   '_Packs/Pro Rifle Pack/walk forward.fbx',                            30.0, 1, None),
    ('GBACK',   '_Packs/Pro Rifle Pack/walk backward.fbx',                           30.0, 1, None),
    ('GLEFT',   '_Packs/Pro Rifle Pack/walk left.fbx',                               30.0, 1, None),
    ('GRIGHT',  '_Packs/Pro Rifle Pack/walk right.fbx',                              30.0, 1, None),
    ('GRUN',    '_Packs/Pro Rifle Pack/run forward.fbx',                             30.0, 1, None),
    ('GRELOAD', 'Reloading.fbx',                                                     50.0, 0, None),
    # 31..36: idles calmos da tela PERSONAGEM (loop sem emenda, virados para a frente do IDLE)
    ('VIDLE1',  'Breathing Idle.fbx',                                                30.0, 1, 'idle_loop'),
    ('VIDLE2',  'Idle.fbx',                                 30.0, 1, 'idle_loop'),
    ('VIDLE3',  '_Packs/Female Locomotion Pack/idle.fbx',                                   30.0, 1, 'idle_loop'),
    ('VIDLE4',  'Weight Shift.fbx',                                   30.0, 1, 'idle_loop'),
    ('VIDLE5',  'Unarmed Idle Looking Ver. 1.fbx',                                   30.0, 1, 'idle_loop'),
    ('VIDLE6',  'Look Around.fbx',                                   30.0, 1, 'idle_loop'),
]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=ybot_fbx)
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
meshes = [o for o in bpy.data.objects if o.type == 'MESH']

arm.data.pose_position = 'REST'
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()

C = Matrix(((-1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
C_inv = C.inverted()

allv = []
for o in meshes:
    eo = o.evaluated_get(dg); me = eo.to_mesh()
    mw = o.matrix_world
    allv += [(C @ (mw @ v.co)) for v in me.vertices]
    eo.to_mesh_clear()

mn = [min(v[i] for v in allv) for i in range(3)]
mx = [max(v[i] for v in allv) for i in range(3)]
off = Vector(((mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2))
T_B2E = Matrix.Translation(-off) @ C

M_arm = arm.matrix_world
M_arm_inv = M_arm.inverted()

bones = list(arm.data.bones)
nb = len(bones)
assert nb == 65, f"Esperado 65 ossos, veio {nb}"
bone_idx = {b.name: i for i, b in enumerate(bones)}
parents = [bone_idx[b.parent.name] if b.parent else -1 for b in bones]
for i, p in enumerate(parents):
    assert p < i, f"Ordem topológica violada no osso {i} (pai {p})"

G_rest = [T_B2E @ M_arm @ b.matrix_local @ M_arm_inv @ C_inv for b in bones]
inv_bind = [g.inverted() for g in G_rest]

def decompose_local(G_list):
    out = []
    for i in range(nb):
        p = parents[i]
        L = G_list[i] if p < 0 else (G_list[p].inverted() @ G_list[i])
        t, q, _ = L.decompose()
        q.normalize()
        out.append((t.copy(), q.copy()))
    return out

rest_local = decompose_local(G_rest)
rest_root_x = rest_local[0][0].x
rest_root_z = rest_local[0][0].z

def pack_pose(pose_tq, prev_q=None):
    raw = bytearray()
    for i, (t, q) in enumerate(pose_tq):
        qn = q.normalized()
        if prev_q is not None and prev_q[i] is not None:
            if prev_q[i].dot(qn) < 0.0:
                qn = -qn
            prev_q[i] = qn
        elif prev_q is not None:
            prev_q[i] = qn
        # Quat no motor: (x, y, z, w) em int16 [-32767, 32767]
        qi = [max(-32767, min(32767, int(round(c * 32767.0)))) for c in (qn.x, qn.y, qn.z, qn.w)]
        raw += struct.pack('<3f4h', t.x, t.y, t.z, qi[0], qi[1], qi[2], qi[3])
    return raw

def eval_frame_matrices(f):
    bpy.context.scene.frame_set(int(f))
    bpy.context.view_layer.update()
    return [T_B2E @ M_arm @ arm.pose.bones[b.name].matrix @ M_arm_inv @ C_inv for b in bones]

def eval_frame(f):
    return decompose_local(eval_frame_matrices(f))

def blend_poses(pa, pb, alpha):
    out = []
    for (ta, qa), (tb, qb) in zip(pa, pb):
        qa_n = qa.normalized()
        qb_n = qb.normalized()
        if qa_n.dot(qb_n) < 0.0:
            qb_n = -qb_n
        out.append((ta.lerp(tb, alpha), qa_n.slerp(qb_n, alpha)))
    return out

def blend_subset(base, target, bone_ids, alpha):
    out = list(base)
    for i in bone_ids:
        ta, qa = base[i]
        tb, qb = target[i]
        qa_n = qa.normalized()
        qb_n = qb.normalized()
        if qa_n.dot(qb_n) < 0.0:
            qb_n = -qb_n
        out[i] = (ta.lerp(tb, alpha), qa_n.slerp(qb_n, alpha))
    return out

def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)

# Extrai a pose de mão no bolso/cintura (Searching Pockets.fbx) para usar ao levantar no COLETAR
before_sp = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', 'Searching Pockets.fbx'))
sp_objs = [o for o in bpy.data.objects if o not in before_sp]
sp_arm = [o for o in sp_objs if o.type == 'ARMATURE'][0]
sp_act = sp_arm.animation_data.action
arm.animation_data.action = sp_act
arm.data.pose_position = 'POSE'
pocket_pose = blend_poses(eval_frame(28), eval_frame(34), 0.5)
for o in sp_objs:
    bpy.data.objects.remove(o, do_unlink=True)
bpy.data.actions.remove(sp_act, do_unlink=True)

body_stand_ids = [0] + list(range(1, 7)) + list(range(55, 65))
left_arm_ids = list(range(7, 31))
right_arm_ids = list(range(31, 55))

# ---- Poses de CARREGAR no ombro (não há clipe no Mixamo): construídas apontando ossos para direções no espaço da engine (+X direita, +Y cima, frente -Z) ----
idle_fingers = {}   # dedos relaxados (quadro do IDLE), preenchido quando o clipe IDLE é exportado
def aim_pose(root_pos, root_rot, aims, arms=None):
    # aims: osso -> (filho, direção): rotação mínima (tronco e pernas). arms: osso -> (filho, direção, polegar): orientação COMPLETA a partir do
    # T-pose (braço em ±X, palma para baixo, polegar para -Z), para o giro do braço/mão ficar anatômico (a rotação mínima deixava a palma invertida).
    arms = arms or {}
    G = [None] * nb
    for i, b in enumerate(bones):
        t, q = idle_fingers.get(i, rest_local[i])
        L = Matrix.Translation(t) @ q.to_matrix().to_4x4()
        G[i] = (Matrix.Translation(root_pos) @ root_rot) if parents[i] < 0 else G[parents[i]] @ L
        key = b.name.split(':')[-1]
        if key in aims:
            child, tgt = aims[key]
            c = bone_idx['mixamorig:' + child]
            cur = (G[i].to_3x3() @ rest_local[c][0]).normalized()
            R = cur.rotation_difference(Vector(tgt).normalized()).to_matrix()
            G[i] = Matrix.Translation(G[i].to_translation()) @ (R @ G[i].to_3x3()).to_4x4()
        if key in arms:
            child, d1, t1 = arms[key]
            c = bone_idx['mixamorig:' + child]
            d0 = (G_rest[c].to_translation() - G_rest[i].to_translation()).normalized()
            t0 = Vector((0, 0, -1)); t0 = (t0 - d0 * t0.dot(d0)).normalized()
            d1 = Vector(d1).normalized(); t1 = Vector(t1); t1 = (t1 - d1 * t1.dot(d1)).normalized()
            B0 = Matrix((d0, t0, d0.cross(t0))).transposed(); B1 = Matrix((d1, t1, d1.cross(t1))).transposed()
            G[i] = Matrix.Translation(G[i].to_translation()) @ (B1 @ B0.transposed() @ G_rest[i].to_3x3()).to_4x4()
    return decompose_local(G)

def flex(d, t, deg):   # dobra o cotovelo: gira (d, t) no plano do braço, levando o antebraço para o lado da dobra (t)
    d = Vector(d).normalized(); t = Vector(t); t = (t - d * t.dot(d)).normalized(); r = math.radians(deg)
    return tuple(d * math.cos(r) + t * math.sin(r)), tuple(-d * math.sin(r) + t * math.cos(r))

def bend(deg, x=0.0):   # 0 = para cima, 90 = para a frente, 180 = para baixo
    r = math.radians(deg)
    return (x, math.cos(r), -math.sin(r))

CARRIED_HIPS = Vector((0.20, 1.60, 0.00))   # quadril sobre o ombro direito de quem carrega
CARRIED_BEND = 100.0                        # pelve deitada de barriga para baixo
def carried_pose():
    return aim_pose(CARRIED_HIPS, Matrix.Rotation(-math.radians(CARRIED_BEND), 4, 'X'), {
        'Spine': ('Spine1', bend(128, -0.22)), 'Spine1': ('Spine2', bend(146, -0.34)), 'Spine2': ('Neck', bend(160, -0.42)),
        'Neck': ('Head', bend(165, -0.25)), 'Head': ('HeadTop_End', bend(172, -0.10)),
        'LeftUpLeg': ('LeftLeg', (-0.02, -0.96, 0.28)), 'RightUpLeg': ('RightLeg', (0.06, -0.96, 0.28)),
        'LeftLeg': ('LeftFoot', (-0.02, -0.84, 0.54)), 'RightLeg': ('RightFoot', (0.04, -0.87, 0.48)),
    }, {   # braços pendurados: palmas para dentro, polegar para longe de quem carrega
        'LeftArm': ('LeftForeArm', (-0.15, -0.95, -0.25), (0, 0, -1)), 'LeftForeArm': ('LeftHand', (-0.02, -0.98, -0.20), (0, 0, -1)), 'LeftHand': ('LeftHandMiddle1', (-0.02, -0.98, -0.10), (0, 0, -1)),
        'RightArm': ('RightForeArm', (0.10, -0.95, -0.20), (0, 0, -1)), 'RightForeArm': ('RightHand', (0.02, -0.98, -0.15), (0, 0, -1)), 'RightHand': ('RightHandMiddle1', (0.02, -0.98, -0.08), (0, 0, -1)),
    })
def carrier_pose():
    # braço direito DOBRADO como na referência: cotovelo para fora na altura do peito, antebraço para CIMA por fora da coxa do carregado,
    # mão na altura da cabeça curvada para dentro, segurando a perna (palma para dentro)
    ad, at = (0.85, -0.45, 0.08), (0.43, 0.90, 0.0)
    fd, ft = (0.0, 1.0, 0.10), (0.0, 0.0, 1.0)
    hd, ht = (-0.45, 0.88, 0.10), (0.0, 0.0, 1.0)
    return aim_pose(G_rest[0].to_translation(), G_rest[0].to_3x3().to_4x4(), {}, {
        'RightArm': ('RightForeArm', ad, at), 'RightForeArm': ('RightHand', fd, ft), 'RightHand': ('RightHandMiddle1', hd, ht),
    })

exported_clips = []
for cname, rel_path, fps, loop, mode in CLIPS:
    full_path = os.path.join(ROOT, 'import assets', rel_path)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=full_path)
    new_objs = [o for o in bpy.data.objects if o not in before]
    new_arm = [o for o in new_objs if o.type == 'ARMATURE'][0]
    act = new_arm.animation_data.action
    arm.animation_data.action = act
    arm.data.pose_position = 'POSE'
    f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])

    frames_tq = []
    if mode in ('pose_carrier', 'pose_carried'):
        p = carried_pose() if mode == 'pose_carried' else carrier_pose()
        frames_tq = [p, p]
    elif mode == 'cross_trim':
        for f in range(8, 53):
            frames_tq.append(eval_frame(f))
    elif mode and mode.startswith('trim:'):
        _, ta, tb = mode.split(':')
        for f in range(int(ta), int(tb) + 1):
            frames_tq.append(eval_frame(f))
    elif mode == 'swim_idle':
        # Boiar na superfície: baixa o corpo (-0.24 m) para que a água (y=1.20 m no modelo quando p.y=-1.20 m)
        # fique na altura do pescoço/ombros, mantendo peito e mãos debaixo d'água e só a cabeça para fora
        for f in range(f0, f1 + 1):
            fr = eval_frame(f)
            t0, q0 = fr[0]
            fr[0] = (Vector((t0.x, t0.y - 0.24, t0.z)), q0)
            frames_tq.append(fr)
    elif mode == 'swim_loop':
        # Ciclo contínuo de braçada de peito (frames 1..41 de Swimming To Edge.fbx):
        # inclina o tronco +18° (quadril/pernas mais fundos na água, ombros sob a superfície)
        # e ergue levemente o pescoço/cabeça (+10°) para manter a cabeça fora d'água na mesma linha do BOIAR
        raw_swim = []
        for f in range(1, 42):
            G = eval_frame_matrices(f)
            p_hips = G[0].translation.copy()
            R_body = Matrix.Translation(p_hips) @ Matrix.Rotation(math.radians(18.0), 4, 'X') @ Matrix.Translation(-p_hips)
            G = [R_body @ m for m in G]
            p_neck = G[4].translation.copy()
            R_neck = Matrix.Translation(p_neck) @ Matrix.Rotation(math.radians(10.0), 4, 'X') @ Matrix.Translation(-p_neck)
            for idx in (4, 5, 6):
                G[idx] = R_neck @ G[idx]
            neck_y = G[4].translation.y
            fr = decompose_local(G)
            t0, q0 = fr[0]
            # Mantém o pescoço/queixo alinhado à superfície e ombros/braços abaixo de 1.20 m
            target_neck = 1.20 + 0.25 * (neck_y - 1.70)
            dy_swim = target_neck - neck_y
            fr[0] = (Vector((rest_root_x, t0.y + dy_swim, rest_root_z)), q0)
            raw_swim.append(fr)
        for i, fr in enumerate(raw_swim):
            if i >= 33:
                w_loop = smoothstep(32.0, 40.0, float(i))
                fr = blend_poses(fr, raw_swim[0], w_loop)
            frames_tq.append(fr)
    elif mode == 'pickup_pocket':
        end_pose = eval_frame(139)
        bottom_pose = eval_frame(76)
        # Abaixa (16..46) e pega no chão (ponte suave entre 46..54 e 68..76)
        for f in range(16, 47):
            p = eval_frame(f)
            w_l = 0.55 * smoothstep(16.0, 38.0, float(f))
            frames_tq.append(blend_subset(p, end_pose, left_arm_ids, w_l))
        for k in range(1, 9):
            pa = eval_frame(46 + k)
            pb = eval_frame(68 + k)
            p = blend_poses(pa, pb, k / 9.0)
            frames_tq.append(blend_subset(p, end_pose, left_arm_ids, 0.55))
        bottom_pose = frames_tq[-1]
        # Ao levantar (76..126): corpo ereto, braço esquerdo relaxa na lateral e mão direita vai ao bolso/cintura
        for f in range(76, 127):
            base = eval_frame(f)
            w_stand = smoothstep(76.0, 106.0, float(f))
            base = blend_subset(base, end_pose, body_stand_ids, w_stand)
            w_left = smoothstep(76.0, 102.0, float(f))
            l_target = blend_poses(bottom_pose, end_pose, w_left)
            base = blend_subset(base, l_target, left_arm_ids, 1.0)
            if f <= 112:
                w_to_pocket = smoothstep(76.0, 94.0, float(f))
                r_target = blend_poses(bottom_pose, pocket_pose, w_to_pocket)
                base = blend_subset(base, r_target, right_arm_ids, 1.0)
            else:
                w_to_end = smoothstep(112.0, 126.0, float(f))
                r_target = blend_poses(pocket_pose, end_pose, w_to_end)
                base = blend_subset(base, r_target, right_arm_ids, 1.0)
            frames_tq.append(base)
    else:
        for f in range(f0, f1 + 1):
            frames_tq.append(eval_frame(f))

    # Pulo/queda/pouso do pack vêm com o corpo girado 20..60 graus para a direita em relação ao IDLE (quadril +39 no pulo, +62 na queda,
    # contra +5 no IDLE): o personagem "gira" ao pular em vez de continuar virado para onde anda. Mede a direção (yaw global) do quadril,
    # do peito (osso 3) e da cabeça (5), com o peito valendo o dobro, e pré-multiplica a rotação da raiz por um giro em Y (em volta do
    # quadril: a posição não muda) que leva essa direção à do IDLE. PULAR e CAIR: giro constante (mantém a dinâmica do clipe, só endireita);
    # POUSAR: parte do giro da queda (continuidade) e vai ao giro que faz o último quadro (já em pé) coincidir com o IDLE.
    def global_yaws(fr):
        g = [None] * nb
        for k in range(nb):
            g[k] = fr[k][1] if parents[k] < 0 else g[parents[k]] @ fr[k][1]
        def yaw(q):
            v = q @ Vector((0.0, 0.0, -1.0))
            return math.degrees(math.atan2(v.x, -v.z))
        return yaw(g[0]), yaw(g[3]), yaw(g[5])
    def heading(fr):
        a, b, c = global_yaws(fr)
        return (a + 2.0 * b + c) / 4.0
    def turn_root(fr, deg):
        t0, q0 = fr[0]
        fr[0] = (t0, Quaternion((0.0, 1.0, 0.0), math.radians(deg)) @ q0)
    if cname == 'IDLE':
        idle_heading = heading(frames_tq[0]); idle_frame0 = [(t.copy(), q.copy()) for t, q in frames_tq[0]]
    if cname in ('JUMP', 'FALL'):
        shift = sum(heading(fr) for fr in frames_tq) / len(frames_tq) - idle_heading
        for fr in frames_tq: turn_root(fr, shift)
        if cname == 'FALL': fall_shift = shift
        print(f"  {cname}: endireitado em {shift:.1f} graus (direcao do corpo -> IDLE)")
    elif cname == 'LAND':
        # último quadro: pose "parado em pé" do pack (com a cabeça virada de lado): alinha só quadril e peito ao IDLE
        def hc(fr):
            y0, y3, _ = global_yaws(fr)
            return (y0 + 2.0 * y3) / 3.0
        end_shift = hc(frames_tq[-1]) - hc(idle_frame0)
        n_l = len(frames_tq)
        for k, fr in enumerate(frames_tq):
            sm = smoothstep(0.0, 1.0, k / max(1, n_l - 1))
            turn_root(fr, fall_shift + (end_shift - fall_shift) * sm)
        print(f"  LAND: giro {fall_shift:.1f} -> {end_shift:.1f} graus")
    if mode == 'idle_loop':
        shift = heading(frames_tq[0]) - idle_heading
        for fr in frames_tq: turn_root(fr, shift)
        n_i = len(frames_tq); k0 = int(n_i * 0.8)   # últimos 20% voltam ao 1º quadro: loop sem salto
        for k in range(k0, n_i):
            frames_tq[k] = blend_poses(frames_tq[k], frames_tq[0], smoothstep(float(k0), float(n_i - 1), float(k)))
        print(f"  {cname}: giro {shift:.1f} graus, loop em {n_i - k0} quadros")
    if cname == 'IDLE':
        idle_fingers.update({i: frames_tq[0][i] for i in list(range(11, 31)) + list(range(35, 55))})
    # Centraliza o deslocamento horizontal inicial (x, z) do osso raiz (Hips) na origem de repouso
    dx0 = 0.0 if (mode or '').startswith('pose_') else rest_root_x - frames_tq[0][0][0].x
    dz0 = 0.0 if (mode or '').startswith('pose_') else rest_root_z - frames_tq[0][0][0].z
    for fr in frames_tq:
        t0, q0 = fr[0]
        fr[0] = (Vector((t0.x + dx0, t0.y, t0.z + dz0)), q0)

    prev_q = [None] * nb
    frames_bytes = bytearray()
    for fr in frames_tq:
        frames_bytes += pack_pose(fr, prev_q)

    exported_clips.append((cname, fps, len(frames_tq), loop, frames_bytes))
    print(f"CLIP {cname:8s}: {len(frames_tq):3d} frames @ {fps:.0f} fps ({len(frames_tq)/fps:.2f}s) loop={loop} <- {rel_path}")

    for o in new_objs:
        bpy.data.objects.remove(o, do_unlink=True)
    bpy.data.actions.remove(act, do_unlink=True)

with open(dst, 'wb') as f:
    f.write(b'E0A1')
    f.write(struct.pack('<II', nb, len(exported_clips)))
    f.write(struct.pack('<%db3B' % nb, *parents, 0, 0, 0))
    for m in inv_bind:
        # Column-major 4x4 (igual a Mat4 do Isolated / OpenGL)
        cols = [m[r][c] for c in range(4) for r in range(4)]
        f.write(struct.pack('<16f', *cols))
    f.write(pack_pose(rest_local))
    for cname, fps, nfr, loop, fbytes in exported_clips:
        nbuf = cname.encode('ascii')[:15].ljust(16, b'\x00')
        f.write(nbuf)
        f.write(struct.pack('<fII', fps, nfr, loop))
        f.write(fbytes)

print(f"OK: {dst} ({os.path.getsize(dst) / 1024:.1f} KB)")
