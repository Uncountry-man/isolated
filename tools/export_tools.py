# Exporta as ferramentas (machadinha e picareta) de "pick-axe-and-hatchets" para .e0m (E0M2) presas ao osso da MÃO DIREITA do Y Bot.
# Roda no Blender (Flatpak: caminhos ABSOLUTOS):
#   flatpak run org.blender.Blender -b -P /abs/tools/export_tools.py --
# Saída: assets/tool_hatchet.e0m e assets/tool_pickaxe.e0m  (2 submalhas cada: madeira + metal, cor média da textura de albedo).
# Os vértices já saem no ESPAÇO DE BIND do esqueleto da engine (mesma conversão de tools/export_anims.py) com osso = mão direita e
# peso 255, então o skinning por GPU existente (uBones) faz a ferramenta seguir a mão em TODAS as animações, sem shader novo.
# Otimização p/ multiplayer mobile: sem textura (cor uniforme por submalha), decimação das cabeças/cabos, ~200-300 tris por ferramenta.
import bpy, os, sys, struct, math
from mathutils import Matrix, Vector, Euler, Quaternion

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PK = os.path.join(ROOT, 'import assets', 'modelos3d', 'pick-axe-and-hatchets')
YBOT = os.path.join(ROOT, 'import assets', 'Y Bot.fbx')
args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []

# ---- Mão FECHADA ao segurar a ferramenta ----
# Os dedos (ossos 35..54) vêm de um quadro REAL do Mixamo segurando um machado (Pro Melee Axe Pack/standing idle): pegada de
# martelo verdadeira (4 dedos envolvem o cabo, polegar por cima). Medido nesse quadro (espaço local do osso da mão, cm):
# os dedos giram em torno do eixo X do osso (X = lado do POLEGAR, -X = mindinho, Y = dedos/antebraço, Z = palma) e o
# "furo" do punho é um cilindro ao longo de X centrado em (y 13.2, z 3.4) cm.
FIST_CLIP = ('_Packs/Pro Melee Axe Pack/standing idle.fbx', 10)

# ---- Empunhadura (espaço local do osso da mão, metros, sem escala) ----
#  centro do punho, direção cabo->cabeça (h) e direção do gume (e). O cabo atravessa o punho ao longo de X com a CABEÇA do lado
#  do polegar (+X) e o gume para a frente do punho (+Y = direção do antebraço/nós dos dedos), como numa machadinha de verdade.
GRIP_C = (-0.010, 0.132, 0.034)
GRIP_H = (1.0, 0.0, 0.0)
GRIP_E = (0.0, 1.0, 0.0)
for i, a in enumerate(args):
    if a == '--grip':   # experimentos: --grip hx,hy,hz,ex,ey,ez
        v = [float(x) for x in args[i + 1].split(',')]; GRIP_H, GRIP_E = tuple(v[:3]), tuple(v[3:6])
OUTDIR = args[args.index('--out') + 1] if '--out' in args else None   # experimentos: grava só os .e0m nesta pasta

# Ferramentas: (nome, [cabo], [cabeça], escala, espessura do cabo (fator só na seção), pegada: distância do PONTO DE PEGADA
#  à ponta de baixo do cabo (m, já em escala), alvo de tris cabo/cabeça)
# A fonte tem cabos grossos demais para o punho do Y Bot (furo ~4 cm): a seção do cabo é afinada para ~3 x 3.8 cm.
TOOLS = [
    ('hatchet', ['Handle2'], ['Head3'], 2.2, 0.80, 0.075, 70, 150),
    ('pickaxe', ['Handle'],  ['Head'],  1.9, 0.68, 0.080, 80, 130),
]

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=YBOT)
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
ymeshes = [o for o in bpy.data.objects if o.type == 'MESH']
arm.data.pose_position = 'REST'
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()

# mesma conversão Blender -> engine de export_anims.py (pés em y=0, centrado em x/z)
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
hb = arm.data.bones[hand_name]
_bw = M_arm @ hb.matrix_local
BONE_SCALE = _bw.to_scale()[0]                       # o FBX do Y Bot está em cm (armature com escala 0.01)
BONE_UNIT = Matrix.Translation(_bw.translation) @ _bw.to_3x3().normalized().to_4x4()   # osso em metros, sem escala
BONE_LEN_M = hb.length * BONE_SCALE
print("MAO DIREITA:", hand_name, "indice", hand_idx, "comprimento do osso (m)", round(BONE_LEN_M, 4), "escala", BONE_SCALE)

def grip_matrix():
    """tool-local (Y = cabo p/ a cabeça, Z = gume) -> espaço local (metros) do osso da mão."""
    h = Vector(GRIP_H).normalized(); e = Vector(GRIP_E); e = (e - h * e.dot(h)).normalized(); xt = h.cross(e)
    R = Matrix(((xt.x, h.x, e.x, 0), (xt.y, h.y, e.y, 0), (xt.z, h.z, e.z, 0), (0, 0, 0, 1)))
    return Matrix.Translation(Vector(GRIP_C)) @ R
# tool-local (Y = cabo p/ cabeça, Z = gume) <- fonte (Z cima, gume em -Y)
SRC2T = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))

def avg_color(path):
    img = bpy.data.images.load(path); px = list(img.pixels); n = len(px) // 4
    step = max(1, n // 4000); r = g = b = 0.0; c = 0
    for i in range(0, n, step):
        r += px[i * 4]; g += px[i * 4 + 1]; b += px[i * 4 + 2]; c += 1
    # albedo está em sRGB; o shader da engine usa cores diretas (mesma convenção dos materiais do Y Bot)
    return (r / c, g / c, b / c)

# carrega a fonte uma vez
bpy.ops.import_scene.fbx(filepath=os.path.join(PK, 'source', 'PickAxe.fbx'))
src = {o.name: o for o in bpy.data.objects if o.type == 'MESH' and o.name in ('Handle', 'Handle2', 'Handle3', 'Handle4', 'Head', 'Head1', 'Head3', 'polySurface1')}
col_wood = avg_color(os.path.join(PK, 'textures', 'AxeWood_albedo.jpg'))
col_metal = avg_color(os.path.join(PK, 'textures', 'AxeMetal_albedo.jpg'))
print("cores", [round(c, 3) for c in col_wood], [round(c, 3) for c in col_metal])

def decimated(obj, target_tris):
    dgx = bpy.context.evaluated_depsgraph_get()
    me0 = obj.evaluated_get(dgx).to_mesh(); me0.calc_loop_triangles()
    tris0 = len(me0.loop_triangles); obj.evaluated_get(dgx).to_mesh_clear()
    if tris0 > target_tris:
        m = obj.modifiers.new('d', 'DECIMATE'); m.decimate_type = 'COLLAPSE'; m.ratio = target_tris / float(tris0); m.use_collapse_triangulate = True
    bpy.context.view_layer.update()
    dgx = bpy.context.evaluated_depsgraph_get()
    return obj.evaluated_get(dgx)

def build_tool(name, handles, heads, scale, thin, grip_from_end, t_handle, t_head):
    h = src[handles[0]]
    bb = [h.matrix_world @ Vector(c) for c in h.bound_box]
    cx = (min(v.x for v in bb) + max(v.x for v in bb)) / 2; cy = (min(v.y for v in bb) + max(v.y for v in bb)) / 2
    zbot = min(v.z for v in bb)
    grip_src = Vector((cx, cy, zbot + grip_from_end / scale))
    G = grip_matrix()
    # fonte -> tool-local (pegada na origem, escala) -> osso -> mundo Blender -> engine
    to_local = Matrix.Scale(scale, 4) @ SRC2T @ Matrix.Translation(-grip_src)
    THIN = Matrix.Diagonal((thin, 1, thin, 1))          # afina só a seção do cabo (X/Z do tool-local)
    out = []
    for names, color, target, extra in ((handles, col_wood, t_handle, THIN), (heads, col_metal, t_head, Matrix.Identity(4))):
        verts, norms, idx = [], [], []
        for nme in names:
            eo = decimated(src[nme], target); me = eo.to_mesh(); me.calc_loop_triangles()
            mw = eo.matrix_world
            M_full = T_B2E @ BONE_UNIT @ G @ extra @ to_local @ mw
            nm3 = M_full.to_3x3().inverted_safe().transposed()
            base = len(verts)
            for v in me.vertices:
                verts.append(M_full @ v.co); n = (nm3 @ v.normal); n.normalize(); norms.append(n)
            idx += [base + i for t in me.loop_triangles for i in t.vertices]
            eo.to_mesh_clear()
        out.append((color, verts, norms, idx))
    L = [(BONE_UNIT.inverted() @ T_B2E.inverted() @ v) for v in out[0][1]]
    print(name, "cabo: comprimento %.3f m" % (max(v.x for v in L) - min(v.x for v in L)), "bb local mao", [round(min(v[i] for v in L), 3) for i in range(3)], [round(max(v[i] for v in L), 3) for i in range(3)])
    return out

def write_e0m(path, subs):
    with open(path, 'wb') as f:
        f.write(b'E0M2'); f.write(struct.pack('<I', len(subs)))
        for color, verts, norms, idx in subs:
            f.write(struct.pack('<3fII', *color, len(verts), len(idx)))
            for v, n in zip(verts, norms):
                f.write(struct.pack('<6f8B', v.x, v.y, v.z, n.x, n.y, n.z, hand_idx, 0, 0, 0, 255, 0, 0, 0))
            f.write(struct.pack('<%dI' % len(idx), *idx))
    print("ESCRITO", path, [(len(s[1]), len(s[3]) // 3) for s in subs], "(verts, tris)")

# --- pose da mão fechada (locais dos ossos 35..54 no espaço da engine) -> engine/src/fist_pose.h ---
C_inv = C.inverted(); M_arm_inv = M_arm.inverted()
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', FIST_CLIP[0]))
fnew = [o for o in bpy.data.objects if o not in before]
fact = [o for o in fnew if o.type == 'ARMATURE'][0].animation_data.action
arm.animation_data_create(); arm.animation_data.action = fact
arm.data.pose_position = 'POSE'
bpy.context.scene.frame_set(FIST_CLIP[1]); bpy.context.view_layer.update()
G_f = [T_B2E @ M_arm @ arm.pose.bones[b.name].matrix @ M_arm_inv @ C_inv for b in bones]
parents_idx = [[x.name for x in bones].index(b.parent.name) if b.parent else -1 for b in bones]
fist_rows = []
for i in range(35, 55):
    L = G_f[parents_idx[i]].inverted() @ G_f[i]
    t, q, _ = L.decompose(); q.normalize()
    fist_rows.append((t.x, t.y, t.z, q.x, q.y, q.z, q.w))
arm.animation_data.action = None; arm.data.pose_position = 'REST'; bpy.context.view_layer.update()
for o in fnew: bpy.data.objects.remove(o, do_unlink=True)
if not OUTDIR:
    with open(os.path.join(ROOT, 'engine', 'src', 'fist_pose.h'), 'w') as f:
        f.write("// fist_pose.h - pose de mão direita FECHADA (ossos 35..54 = polegar..mindinho), local = (t xyz, q xyzw) no espaço da engine.\n")
        f.write("// Autogerado por tools/export_tools.py a partir de '%s' quadro %d - NÃO EDITE MANUALMENTE\n#pragma once\nstatic const float kFistPose[20][7] = {\n" % FIST_CLIP)
        for r in fist_rows: f.write("    {" + ", ".join("%.6ff" % v for v in r) + "},\n")
        f.write("};\n")
    print("ESCRITO fist_pose.h")

for t in TOOLS:
    subs = build_tool(*t)
    write_e0m(os.path.join(OUTDIR or os.path.join(ROOT, 'assets'), 'tool_%s.e0m' % t[0]), subs)
