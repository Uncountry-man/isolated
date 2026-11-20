# ESTÁGIO C dos piratas "tripo": RIG NO BLENDER (roda dentro do Blender, chamado por tools/export_tripo.py ou à mão):
#   flatpak run --filesystem=host org.blender.Blender -b -P /abs/tools/tripo_rig.py -- /abs/RAIZ /abs/SAIDA_DIR [--write] [--poses]
# Lê a geometria em pose T já ajustada ao Y Bot (assets/pirate_<m|f>[_lod1|_lod2].e0m + textura build/tripo/out/<m|f>_lod<k>.png), importa o ARMATURE REAL do
# `import assets/Y Bot.fbx`, faz o "Armature > Automatic Weights" (difusão de calor, a mesma ferramenta do Blender) e:
#   --poses  renderiza folhas de teste com ossos posados de verdade (cotovelo 90, agachar, punho, braços para frente) em SAIDA_DIR
#   --write  grava os pesos no lugar dos antigos nos .e0m (os originais vão para SAIDA_DIR/original/)
# Índice do osso = ordem de arm.data.bones (a mesma do export_mesh.py/export_anims.py = ybot.e0a); confere as posições das juntas contra o ybot.e0a.
import bpy, sys, os, struct, math, shutil
import numpy as np
from mathutils import Vector, Matrix, kdtree

args = sys.argv[sys.argv.index('--') + 1:]
ROOT, OUT = args[0], args[1]
WRITE = '--write' in args; POSES = '--poses' in args
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', 'Y Bot.fbx'))
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
arm.data.pose_position = 'REST'
bpy.context.view_layer.update()


def conv(v): return (-v.x, v.z, v.y)        # Blender (Z-up, frente -Y) -> engine (Y-up, frente -Z), igual ao export_mesh.py


allv = []
for o in [o for o in bpy.data.objects if o.type == 'MESH']:
    allv += [conv(o.matrix_world @ v.co) for v in o.data.vertices]
mn = [min(v[i] for v in allv) for i in range(3)]; mx = [max(v[i] for v in allv) for i in range(3)]
OFF = ((mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2)
for o in [o for o in bpy.data.objects if o.type == 'MESH']: bpy.data.objects.remove(o)   # só o esqueleto interessa
bones = list(arm.data.bones)
bone_idx = {b.name: i for i, b in enumerate(bones)}


def to_blender(e): return Vector((-(e[0] + OFF[0]), e[2] + OFF[2], e[1] + OFF[1]))   # inverso de conv() + origem


# confere a ordem dos ossos com o ybot.e0a (juntas = inversa das invBind)
a = open(os.path.join(ROOT, 'assets', 'ybot.e0a'), 'rb').read(); nb = struct.unpack('<I', a[4:8])[0]
inv = np.frombuffer(a, np.float32, nb * 16, 12 + ((nb + 3) & ~3)).reshape(nb, 4, 4)
J = np.array([np.linalg.inv(inv[i].T)[:3, 3] for i in range(nb)])
worst = 0.0
for i, b in enumerate(bones):
    h = conv(arm.matrix_world @ b.head_local); e = np.array([h[0] - OFF[0], h[1] - OFF[1], h[2] - OFF[2]])
    worst = max(worst, float(np.linalg.norm(e - J[i])))
print('ossos: %d (e0a: %d), pior diferença de junta vs ybot.e0a = %.4f m' % (len(bones), nb, worst))
assert len(bones) == nb and worst < 0.02, 'ordem/posição dos ossos não bate com o ybot.e0a'


def read_e0m(path):
    b = open(path, 'rb').read(); assert b[:4] == b'E0M3'
    nv, ni = struct.unpack('<II', b[8 + 12:8 + 20])
    v = np.frombuffer(b, np.uint8, nv * 40, 28).reshape(nv, 40)
    P = v[:, :12].copy().view('<f4').reshape(nv, 3).astype(np.float64)
    UV = v[:, 32:40].copy().view('<f4').reshape(nv, 2).astype(np.float64)
    I = np.frombuffer(b, '<u4', ni, 28 + nv * 40).reshape(-1, 3).astype(np.int64)
    return b, P, UV, I


def build_mesh(name, P, UV, I, png):
    """malha soldada por posição (as costuras de UV separariam o cálculo de pesos); UV por canto; material com a textura."""
    key = np.round(P * 20000).astype(np.int64)
    _, first, inv_ = np.unique(key, axis=0, return_index=True, return_inverse=True); inv_ = inv_.reshape(-1)
    F = inv_[I]; ok = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(to_blender(P[f])) for f in first], [], [tuple(map(int, f)) for f in F[ok]]); me.update()
    uvl = me.uv_layers.new(name='uv'); Iok = I[ok]
    for pi, poly in enumerate(me.polygons):
        for k, li in enumerate(poly.loop_indices): uvl.data[li].uv = (float(UV[Iok[pi][k]][0]), 1.0 - float(UV[Iok[pi][k]][1]))
    for p in me.polygons: p.use_smooth = True
    ob = bpy.data.objects.new(name, me); sc.collection.objects.link(ob)
    mat = bpy.data.materials.new(name); mat.use_nodes = True
    node = mat.node_tree.nodes.new('ShaderNodeTexImage'); node.image = bpy.data.images.load(png)
    bsdf = mat.node_tree.nodes['Principled BSDF']; mat.node_tree.links.new(node.outputs['Color'], bsdf.inputs['Base Color'])
    me.materials.append(mat)
    return ob, inv_, first


def auto_weights(ob):
    for o in sc.objects: o.select_set(False)
    ob.select_set(True); arm.select_set(True); bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type='ARMATURE_AUTO')
    n = len(ob.data.vertices)
    W = np.zeros((n, len(bones)))
    for v in ob.data.vertices:
        for g in v.groups:
            nm = ob.vertex_groups[g.group].name
            if nm in bone_idx: W[v.index, bone_idx[nm]] = g.weight
    return W


def clean(W, ob):
    """poda influências < 2%, normaliza e dá a vértices sem peso o do vizinho com peso mais próximo."""
    W = np.where(W < 0.02, 0, W)
    s = W.sum(1); empty = np.where(s < 1e-6)[0]
    if len(empty):
        kd = kdtree.KDTree(len(W)); [kd.insert(ob.data.vertices[i].co, i) for i in range(len(W))]; kd.balance()
        for i in empty:
            for co, j, d in kd.find_n(ob.data.vertices[i].co, 40):
                if s[j] > 1e-6: W[i] = W[j]; break
        print('vertices sem peso corrigidos:', len(empty))
    return W / np.maximum(W.sum(1, keepdims=True), 1e-9)


def top4(W):
    top = np.argsort(-W, axis=1)[:, :4]; w4 = np.take_along_axis(W, top, 1); w4 = w4 / np.maximum(w4.sum(1, keepdims=True), 1e-9)
    q = np.floor(w4 * 255 + 0.5).astype(np.int64); q[:, 0] += 255 - q.sum(1); top[q == 0] = 0
    return top.astype(np.uint8), q.astype(np.uint8)


# ---- poses de teste ----------------------------------------------------------------------------------------------------------------------------------------
def rot_bone(pb, axis, ang):
    """gira o osso em volta da sua cabeça por um eixo do espaço do armature (os filhos acompanham)."""
    M = pb.matrix.copy(); h = M.translation.copy()
    t0 = (pb.matrix @ Vector((0, pb.length, 0))).copy()
    pb.matrix = Matrix.Translation(h) @ Matrix.Rotation(ang, 4, axis) @ Matrix.Translation(-h) @ M
    bpy.context.view_layer.update()
    if os.environ.get('RIGDBG'): print('rot', pb.name, 'len', round(pb.length, 3), 'tail', [round(x, 3) for x in t0], '->', [round(x, 3) for x in (pb.matrix @ Vector((0, pb.length, 0)))], 'mode', pb.rotation_mode)


def pb(name): return arm.pose.bones[[b.name for b in bones if name in b.name][0]]


def pose_sheet(ob, inv_, path):
    """poses de teste com ossos posados DE VERDADE: o Blender avalia a deformação (modificador Armature) e gravo as posições deformadas de cada vértice do
    arquivo em <path>_poses.npz (o desenho da folha é feito em Python, tools/export_tripo.py --poses: o render em segundo plano do Blender usava a pose antiga)."""
    X, Y, Z = (1, 0, 0), (0, 1, 0), (0, 0, 1)   # espaço do armature do FBX: X lateral, Y para CIMA, Z para a FRENTE
    out = {}
    def reset():
        for p in arm.pose.bones: p.matrix_basis = Matrix.Identity(4)
        bpy.context.view_layer.update()
    def grab(tag):
        dg = bpy.context.evaluated_depsgraph_get(); ev = ob.evaluated_get(dg); me = ev.to_mesh()
        P = np.array([conv(v.co) for v in me.vertices]) - np.array(OFF)
        out[tag] = P[inv_]; ev.to_mesh_clear()
    arm.data.pose_position = 'POSE'
    reset(); grab('0repouso')
    for side in ('Left', 'Right'):   # braços descem 60 graus e o antebraço dobra 100 graus para a frente
        rot_bone(pb(side + 'Arm'), Z, math.radians(60) * (-1 if side == 'Left' else 1))
        rot_bone(pb(side + 'ForeArm'), X, math.radians(-100))
    grab('1cotovelo'); reset()
    for side in ('Left', 'Right'):   # braços para a frente (ombro) + cotovelo 90
        rot_bone(pb(side + 'Arm'), Y, math.radians(90) * (1 if side == 'Left' else -1))
        rot_bone(pb(side + 'ForeArm'), Y, math.radians(90) * (1 if side == 'Left' else -1))
    grab('2bracos_frente'); reset()
    for side in ('Left', 'Right'):
        rot_bone(pb(side + 'UpLeg'), X, math.radians(-75)); rot_bone(pb(side + 'Leg'), X, math.radians(110))
    grab('3agachar'); reset()
    for b in bones:   # punho: cada falange gira 70 graus para baixo (sinal escolhido pela direção em que a ponta desce)
        if any(t in b.name for t in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky')) and not b.name.endswith('4'):
            p = arm.pose.bones[b.name]; z0 = (p.matrix @ Vector((0, p.length, 0))).y
            rot_bone(p, Z, math.radians(70)); z1 = (p.matrix @ Vector((0, p.length, 0))).y
            if z1 > z0 - 1e-6: rot_bone(p, Z, math.radians(-140))
    grab('4punho'); reset()
    for side in ('Left', 'Right'):   # braços para cima (ombro 150) e pernas abertas (quadril 45 para o lado)
        rot_bone(pb(side + 'Arm'), Z, math.radians(150) * (1 if side == 'Left' else -1))
        rot_bone(pb(side + 'UpLeg'), Z, math.radians(35) * (1 if side == 'Left' else -1))
    grab('5bracos_cima'); reset(); arm.data.pose_position = 'REST'
    np.savez(path, **out)


for key in ('m', 'f'):
    for lod in range(3):
        name = 'pirate_%s%s' % (key, '' if lod == 0 else '_lod%d' % lod)
        path = os.path.join(ROOT, 'assets', name + '.e0m')
        raw, P, UV, I = read_e0m(path)
        ob, inv_, first = build_mesh(name, P, UV, I, os.path.join(ROOT, 'build', 'tripo', 'out', '%s_lod%d.png' % (key, lod)))
        W = clean(auto_weights(ob), ob)
        Wv = W[inv_]                                    # peso por vértice do arquivo (as duplicatas das costuras recebem o mesmo)
        B, W8 = top4(Wv)
        fin = Wv[:, 35:55].sum(1) + Wv[:, 11:31].sum(1)
        print('%s: %d vertices, %.0f%% com peso de dedo na mao' % (name, len(P), 100 * (fin > 0.5).sum() / max(1, (np.abs(P[:, 0]) > 0.74).sum())))
        np.savez(os.path.join(OUT, name + '_w.npz'), B=B, W=W8)
        if POSES and lod == 0: pose_sheet(ob, inv_, os.path.join(OUT, name + '_poses.npz'))
        if WRITE:
            os.makedirs(os.path.join(OUT, 'original'), exist_ok=True); shutil.copy(path, os.path.join(OUT, 'original', name + '.e0m'))
            buf = bytearray(raw); nv = len(P)
            v = np.frombuffer(buf, np.uint8, nv * 40, 28).reshape(nv, 40)
            v[:, 24:28] = B; v[:, 28:32] = W8
            open(path, 'wb').write(bytes(buf))
        bpy.data.objects.remove(ob)
