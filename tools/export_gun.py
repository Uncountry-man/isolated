# Exporta o BACAMARTE PIRATA (import assets/espingarda_pirata/espingarda_pirata.blend, gerado por gerar_espingarda.py) para
# assets/tool_gun.e0m (E0M2) preso ao osso da MÃO DIREITA do Y Bot (osso 34, peso 255: o skinning por GPU leva a arma junto em todas as
# animações) e gera engine/src/gun_tip.h (boca do cano e quadro da arma no espaço de bind, usados pelo tiro, pelo clarão e para guardar nas costas).
# Roda no Blender (Flatpak; caminhos ABSOLUTOS):  blender -b -P /abs/tools/export_gun.py --
#
# Empunhadura: a arma é posicionada na pose "idle aiming" do Pro Rifle Pack (o mesmo pack das animações de rifle; todos os clipes dele
# seguram o rifle invisível no MESMO lugar em relação às mãos): o punho da mão direita (GRIP_C, centro do "furo" do punho medido em
# export_tools.py) fica logo atrás do guarda-mato, o cano aponta para a frente na horizontal (direção dos dedos da mão direita projetada no
# chão) e o topo da arma para cima. A mão esquerda cai sob o guarda-mão (~20 cm à frente).
# Otimização p/ mobile: os arabescos/filigranas em ouro (dezenas de milhares de triângulos) não aparecem no tamanho do jogo e saem;
# cada peça restante é decimada (alvo ~1100 tris no total), sem textura (4 submalhas por material: madeira, ouro, ferro, pederneira).
import bpy, bmesh, os, sys, struct, math
from mathutils import Matrix, Vector

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLEND = os.path.join(ROOT, 'import assets', 'espingarda_pirata', 'espingarda_pirata.blend')
YBOT = os.path.join(ROOT, 'import assets', 'Y Bot.fbx')
AIM_CLIP = os.path.join(ROOT, 'import assets', '_Packs', 'Pro Rifle Pack', 'idle aiming.fbx')
args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUTDIR = args[args.index('--out') + 1] if '--out' in args else os.path.join(ROOT, 'assets')
HDR = os.path.join(ROOT, 'engine', 'src', 'gun_tip.h')

GRIP_C = Vector((-0.010, 0.132, 0.034))   # centro do punho da mão direita (espaço do osso da mão, m)
GUN_GRIP = Vector((0.300, 0.0, -0.035))    # ponto da arma (coords do .blend: x = para a boca, z = para cima, -y = lado do fecho/direita) que fica no punho
MUZZLE = Vector((0.989, 0.0, 0.011))       # boca do sino (eixo do cano: z = 0.011)

# peça -> alvo de triângulos (o que não está aqui sai)
KEEP = {
    'Coronha': 300, 'Cano': 96, 'Boca': 120, 'Anel_culatra': 24, 'Anel_1': 20, 'Abracadeira_1': 24, 'Abracadeira_2': 24, 'Abracadeira_3': 24,
    'Moldura_sino': 28, 'Placa_fecho': 40, 'Cao': 30, 'Cao_espora': 20, 'Guarda_mato': 44, 'Gatilho': 16, 'Chapa_coice': 36, 'Ponteira': 36,
    'Espiga': 20, 'Contraplaca': 30, 'Mandibula_inf': 12, 'Mandibula_sup': 12, 'Pederneira': 12, 'Cacoleta': 12, 'Fuzil': 12, 'Fuzil_tampa': 12,
    'Vareta': 16, 'Vareta_ponta': 12, 'Mira': 12, 'Medalhao_E': 40, 'Medalhao_D': 40,
}
# cor por material (sem textura; tons do render do bacamarte)
COLORS = {'Madeira': (0.40, 0.22, 0.12), 'Ouro': (0.86, 0.64, 0.30), 'Ferro': (0.17, 0.17, 0.19), 'Pederneira': (0.38, 0.36, 0.31)}

# ---------------------------------------------------------------- 1) malha da arma (coords do .blend)
bpy.ops.wm.open_mainfile(filepath=BLEND)
dg = bpy.context.evaluated_depsgraph_get()
parts = {k: ([], [], []) for k in COLORS}   # material -> (verts, normals, idx)
total = 0
for o in list(bpy.data.objects):
    if o.name not in KEEP or o.type not in ('MESH', 'CURVE'):
        continue
    eo = o.evaluated_get(dg)
    me = bpy.data.meshes.new_from_object(eo)
    me.transform(o.matrix_world)
    mat = me.materials[0].name if len(me.materials) and me.materials[0] else 'Ouro'
    ob = bpy.data.objects.new('tmp_' + o.name, me); bpy.context.scene.collection.objects.link(ob)
    bm = bmesh.new(); bm.from_mesh(me); bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0002); bmesh.ops.triangulate(bm, faces=bm.faces); bm.to_mesh(me); bm.free()
    n0 = len(me.polygons); tgt = KEEP[o.name]
    if n0 > tgt:
        m = ob.modifiers.new('d', 'DECIMATE'); m.decimate_type = 'COLLAPSE'; m.ratio = tgt / float(n0); m.use_collapse_triangulate = True
    bpy.context.view_layer.update()
    e2 = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()); em = e2.to_mesh(); em.calc_loop_triangles()
    vs, ns, ix = parts.get(mat, parts['Ouro'])
    base = len(vs)
    # vértices por canto (normais suaves do Blender por vértice; a peça é pequena, sombreado chapado nas quinas vem do EDGE_SPLIT do original)
    for t in em.loop_triangles:
        for k in range(3):
            v = em.vertices[t.vertices[k]]
            vs.append(v.co.copy()); ns.append((em.corner_normals[t.loops[k]].vector if hasattr(em, 'corner_normals') else v.normal).copy())
        ix += [base, base + 1, base + 2]; base += 3
    total += len(em.loop_triangles)
    e2.to_mesh_clear()
print("TRIS arma:", total, {k: len(v[2]) // 3 for k, v in parts.items()})
# junta vértices iguais (posição + normal) para não mandar 3 por triângulo
def weld(vs, ns, ix):
    key = {}; ov, on, oi = [], [], []
    for i in ix:
        p, n = vs[i], ns[i]
        k = (round(p.x, 5), round(p.y, 5), round(p.z, 5), round(n.x, 2), round(n.y, 2), round(n.z, 2))
        j = key.get(k)
        if j is None:
            j = len(ov); key[k] = j; ov.append(p); on.append(n)
        oi.append(j)
    return ov, on, oi
parts = {k: weld(*v) for k, v in parts.items() if v[2]}

# ---------------------------------------------------------------- 2) Y Bot: espaço da engine + pose de mira
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=YBOT)
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
ymeshes = [o for o in bpy.data.objects if o.type == 'MESH']
arm.data.pose_position = 'REST'; bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()
C = Matrix(((-1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
allv = []
for o in ymeshes:
    eo = o.evaluated_get(dg); me = eo.to_mesh(); mw = o.matrix_world
    allv += [(C @ (mw @ v.co)) for v in me.vertices]; eo.to_mesh_clear()
mn = [min(v[i] for v in allv) for i in range(3)]; mx = [max(v[i] for v in allv) for i in range(3)]
off = Vector(((mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2))
T_B2E = Matrix.Translation(-off) @ C
M_arm = arm.matrix_world
bones = list(arm.data.bones)
hand_name = [b.name for b in bones if b.name.endswith('RightHand')][0]
hand_idx = [b.name for b in bones].index(hand_name)
assert hand_idx == 34, hand_idx
def unit(m): return Matrix.Translation(m.translation) @ m.to_3x3().normalized().to_4x4()
H_rest = T_B2E @ unit(M_arm @ arm.data.bones[hand_name].matrix_local)

before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=AIM_CLIP)
na = [o for o in bpy.data.objects if o not in before and o.type == 'ARMATURE'][0]
arm.animation_data.action = na.animation_data.action
arm.data.pose_position = 'POSE'
bpy.context.scene.frame_set(1); bpy.context.view_layer.update()
H_pose = T_B2E @ unit(M_arm @ arm.pose.bones[hand_name].matrix)

g = H_pose @ GRIP_C
fwd = H_pose.to_3x3() @ Vector((0.0, 1.0, 0.0)); fwd.y = 0.0; fwd.normalize()   # dedos da mão direita, no plano do chão
up = Vector((0.0, 1.0, 0.0)); left = up.cross(fwd).normalized()
# quadro da arma na pose: X_arma = fwd, Y_arma = esquerda, Z_arma = cima; origem GUN_GRIP no punho
R = Matrix(((fwd.x, left.x, up.x, 0), (fwd.y, left.y, up.y, 0), (fwd.z, left.z, up.z, 0), (0, 0, 0, 1)))
GunPose = Matrix.Translation(g) @ R @ Matrix.Translation(-GUN_GRIP)
M_full = H_rest @ H_pose.inverted() @ GunPose          # coords do .blend -> espaço de bind da engine
print("PUNHO (pose)", tuple(round(c, 3) for c in g), "frente", tuple(round(c, 3) for c in fwd), "yaw", round(math.degrees(math.atan2(fwd.x, -fwd.z)), 1))

# ---------------------------------------------------------------- 3) grava tool_gun.e0m
nm3 = M_full.to_3x3().inverted_safe().transposed()
order = [k for k in ('Madeira', 'Ouro', 'Ferro', 'Pederneira') if k in parts]
with open(os.path.join(OUTDIR, 'tool_gun.e0m'), 'wb') as f:
    f.write(b'E0M2'); f.write(struct.pack('<I', len(order)))
    for k in order:
        vs, ns, ix = parts[k]
        f.write(struct.pack('<3fII', *COLORS[k], len(vs), len(ix)))
        for v, n in zip(vs, ns):
            p = M_full @ v; q = (nm3 @ n).normalized()
            f.write(struct.pack('<6f8B', p.x, p.y, p.z, q.x, q.y, q.z, hand_idx, 0, 0, 0, 255, 0, 0, 0))
        f.write(struct.pack('<%dI' % len(ix), *ix))
print("ESCRITO tool_gun.e0m", [(k, len(parts[k][0]), len(parts[k][2]) // 3) for k in order], "(verts, tris)")

# ---------------------------------------------------------------- 4) gun_tip.h
mz = M_full @ MUZZLE
md = (M_full.to_3x3() @ Vector((1.0, 0.0, 0.0))).normalized()
G = M_full @ Matrix.Translation(GUN_GRIP)   # quadro da arma com origem no punho -> bind (X = boca, Y = esquerda, Z = topo)
rows = [G[r][c] for r in range(4) for c in range(4)]
with open(HDR, 'w') as f:
    f.write("// gun_tip.h - BACAMARTE (assets/tool_gun.e0m) no espaço de BIND da engine (skinning pelo osso da mão direita, índice 34).\n")
    f.write("// Autogerado por tools/export_gun.py - NÃO EDITE MANUALMENTE\n#pragma once\n")
    f.write("static const float kGunMuzzle[3] = {%.5ff, %.5ff, %.5ff};     // boca do cano\n" % (mz.x, mz.y, mz.z))
    f.write("static const float kGunMuzzleDir[3] = {%.5ff, %.5ff, %.5ff};  // direção do cano (para fora da boca)\n" % (md.x, md.y, md.z))
    f.write("// quadro da arma (origem no punho; X = para a boca, Y = esquerda da arma, Z = topo) -> bind (linha a linha): guardar nas costas (Renderer::drawStowed)\n")
    f.write("static const float kGunGrip[16] = {%s};\n" % ", ".join("%.6ff" % v for v in rows))
print("ESCRITO", HDR, "boca", tuple(round(c, 3) for c in mz), "dir", tuple(round(c, 3) for c in md))
