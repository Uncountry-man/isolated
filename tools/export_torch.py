# Exporta a TOCHA medieval (GLB) para assets/tool_torch.e0m (E0M2) presa ao osso da MÃO DIREITA do Y Bot, + engine/src/torch_tip.h
# (posição da chama no espaço de bind). Mesmo esquema de tools/export_tools.py (skinning por GPU com osso = mão, peso 255).
# Roda no Blender (Flatpak; caminhos ABSOLUTOS):  flatpak run --filesystem=host org.blender.Blender -b -P /abs/tools/export_torch.py --
# Otimização: 2500 tris -> ~300; sem textura (2 submalhas: madeira escura + pano/fibra enrolado, cor média do albedo por triângulo).
import bpy, bmesh, os, sys, struct, math
from mathutils import Matrix, Vector

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'import assets', 'modelos3d', 'medieval-torch')
GLB = os.path.join(SRC, 'source', 'godot', 'torch_godot.glb')
TEX = os.path.join(SRC, 'textures', 'torch_juego_color_2.png')
YBOT = os.path.join(ROOT, 'import assets', 'Y Bot.fbx')
args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUTDIR = args[args.index('--out') + 1] if '--out' in args else None
GRIP_C = (-0.010, 0.132, 0.034); GRIP_H = (1.0, 0.0, 0.0); GRIP_E = (0.0, 1.0, 0.0)
for i, a in enumerate(args):
    if a == '--grip':
        v = [float(x) for x in args[i + 1].split(',')]; GRIP_H, GRIP_E = tuple(v[:3]), tuple(v[3:6])
TORCH_LEN = 0.56          # comprimento total na mão (m)
GRIP_FROM_END = 0.13      # do ponto de pegada até a ponta de baixo do cabo (m)
T_WOOD, T_WRAP = 120, 200 # alvo de triângulos

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
_bw = M_arm @ arm.data.bones[hand_name].matrix_local
BONE_UNIT = Matrix.Translation(_bw.translation) @ _bw.to_3x3().normalized().to_4x4()

def grip_matrix():
    h = Vector(GRIP_H).normalized(); e = Vector(GRIP_E); e = (e - h * e.dot(h)).normalized(); xt = h.cross(e)
    R = Matrix(((xt.x, h.x, e.x, 0), (xt.y, h.y, e.y, 0), (xt.z, h.z, e.z, 0), (0, 0, 0, 1)))
    return Matrix.Translation(Vector(GRIP_C)) @ R

# ---- tocha ----
before = set(bpy.data.objects)
bpy.ops.import_scene.gltf(filepath=GLB)
tobj = [o for o in bpy.data.objects if o not in before and o.type == 'MESH'][0]
bpy.context.view_layer.objects.active = tobj
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
me = tobj.data
# eixo principal (PCA por potência) do cabo: direção em que a tocha é mais comprida
pts = [v.co.copy() for v in me.vertices]
cen = sum(pts, Vector()) / len(pts)
ax = Vector((0.3, 0.2, 1.0)).normalized()
for _ in range(40):
    n = Vector()
    for p in pts:
        d = p - cen; n += d * d.dot(ax)
    ax = n.normalized()
ts = [(p - cen).dot(ax) for p in pts]; lo, hi = min(ts), max(ts)
# a CABEÇA é a ponta mais larga (raio médio maior)
def radius(sel):
    r = 0.0; c = 0
    for p, t in zip(pts, ts):
        if sel(t):
            d = (p - cen) - ax * t; r += d.length; c += 1
    return r / max(c, 1)
span = hi - lo
if radius(lambda t: t > lo + span * 0.75) < radius(lambda t: t < lo + span * 0.25): ax = -ax; ts = [-t for t in ts]; lo, hi = -hi, -lo
print("EIXO", tuple(round(x, 3) for x in ax), "comprimento", round(hi - lo, 3))
# eixo secundário estável (Z do tool-local): perpendicular ao eixo, o mais próximo de +Y global da fonte
sec = Vector((0, 1, 0)); sec = (sec - ax * sec.dot(ax)).normalized()
# base ortonormal fonte -> tool-local (X, Y = ax cabo->cabeça, Z = sec)
xs = ax.cross(sec).normalized() * -1.0
Bm = Matrix(((xs.x, ax.x, sec.x), (xs.y, ax.y, sec.y), (xs.z, ax.z, sec.z))).to_4x4()   # tool-local -> fonte (colunas)
src2t = Bm.inverted()
scale = TORCH_LEN / (hi - lo)
low_pt = cen + ax * lo
to_local = Matrix.Scale(scale, 4) @ src2t @ Matrix.Translation(-(low_pt + ax * (GRIP_FROM_END / scale)))
G = grip_matrix()
M_full = T_B2E @ BONE_UNIT @ G @ to_local

# classifica triângulos pela cor do albedo (UV): pano claro/acinzentado x madeira escura
img = bpy.data.images.load(TEX); W, H = img.size; px = list(img.pixels)
def sample(u, v):
    x = min(W - 1, max(0, int(u * W))); y = min(H - 1, max(0, int(v * H))); i = (y * W + x) * 4
    return px[i], px[i + 1], px[i + 2]
uv = me.uv_layers.active.data
me.calc_loop_triangles()
wood_tris, wrap_tris = [], []
sum_w = [0, 0, 0, 0]; sum_r = [0, 0, 0, 0]
HEAD_T = lo + (hi - lo) * 0.68        # a cabeça (pano enrolado) é a parte de cima: o corte vem do perfil de vértices (salto de densidade em 70%)
for t in me.loop_triangles:
    us = [uv[l].uv for l in t.loops]; cu = sum(u.x for u in us) / 3; cv = sum(u.y for u in us) / 3
    r, g, b = sample(cu, cv)
    tc = sum((me.vertices[i].co - cen).dot(ax) for i in t.vertices) / 3
    is_wrap = tc > HEAD_T
    (wrap_tris if is_wrap else wood_tris).append(t)
    s = sum_r if is_wrap else sum_w
    s[0] += r; s[1] += g; s[2] += b; s[3] += 1
def avg(s, dflt): return tuple(s[i] / s[3] for i in range(3)) if s[3] else dflt
col_wood = (0.20, 0.125, 0.07); col_wrap = (0.30, 0.215, 0.13)   # albedo médio sai lavado pelo fundo da textura: cores escolhidas à mão
print("TRIS fonte: madeira", len(wood_tris), "pano", len(wrap_tris), "cores", [round(c, 3) for c in col_wood], [round(c, 3) for c in col_wrap])

def submesh(tris, target):
    bm = bmesh.new(); bm.from_mesh(me); bm.faces.ensure_lookup_table()
    keep = set(t.polygon_index for t in tris)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index not in keep], context='FACES')
    nm = bpy.data.meshes.new('part'); bm.to_mesh(nm); bm.free()
    ob = bpy.data.objects.new('part', nm); bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    n0 = len(nm.polygons)
    if n0 > target:
        m = ob.modifiers.new('d', 'DECIMATE'); m.decimate_type = 'COLLAPSE'; m.ratio = target / float(n0); m.use_collapse_triangulate = True
    bpy.context.view_layer.update()
    eo = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()); em = eo.to_mesh(); em.calc_loop_triangles()
    nm3 = M_full.to_3x3().inverted_safe().transposed()
    verts, norms = [], []
    for v in em.vertices:
        verts.append(M_full @ v.co); nn = nm3 @ v.normal; nn.normalize(); norms.append(nn)
    idx = [i for t in em.loop_triangles for i in t.vertices]
    eo.to_mesh_clear()
    return (verts, norms, idx)

subs = []
for color, tris, target in ((col_wood, wood_tris, T_WOOD), (col_wrap, wrap_tris, T_WRAP)):
    if tris:
        v, n, i = submesh(tris, target); subs.append((color, v, n, i))

outdir = OUTDIR or os.path.join(ROOT, 'assets')
with open(os.path.join(outdir, 'tool_torch.e0m'), 'wb') as f:
    f.write(b'E0M2'); f.write(struct.pack('<I', len(subs)))
    for color, verts, norms, idx in subs:
        f.write(struct.pack('<3fII', *color, len(verts), len(idx)))
        for v, n in zip(verts, norms):
            f.write(struct.pack('<6f8B', v.x, v.y, v.z, n.x, n.y, n.z, hand_idx, 0, 0, 0, 255, 0, 0, 0))
        f.write(struct.pack('<%dI' % len(idx), *idx))
print("ESCRITO tool_torch.e0m", [(len(s[1]), len(s[3]) // 3) for s in subs], "(verts, tris)")

# chama: nasce DENTRO da bola de pano (7,5 cm abaixo do topo da cabeça) (espaço de bind da engine)
tip_src = cen + ax * (hi - 0.075 / scale)
tip = M_full @ tip_src
top = M_full @ (cen + ax * hi)
print("PONTA DA CHAMA (bind)", tuple(round(x, 3) for x in tip))
HG = T_B2E @ BONE_UNIT @ G     # tool-local (origem = ponto de pegada; Y = cabo->cabeça, Z = gume) -> espaço de bind da engine
for i, b in enumerate(bones):
    if i < 6 or b.name.endswith(('UpLeg', 'Shoulder', 'Spine2')):
        bp = T_B2E @ M_arm @ b.head_local
        print("OSSO", i, b.name, tuple(round(x, 3) for x in bp))
if not OUTDIR:
    with open(os.path.join(ROOT, 'engine', 'src', 'torch_tip.h'), 'a') as f:
        pass
    with open(os.path.join(ROOT, 'engine', 'src', 'torch_tip.h'), 'w') as f:
        f.write("// torch_tip.h - posição da chama da tocha no espaço de BIND da engine (skinning pelo osso da mão direita, índice %d).\n" % hand_idx)
        f.write("// Autogerado por tools/export_torch.py - NÃO EDITE MANUALMENTE\n#pragma once\n")
        f.write("static const float kTorchFlame[3] = {%.5ff, %.5ff, %.5ff};\n" % (tip.x, tip.y, tip.z))
        f.write("// tool-local -> bind da engine (linha a linha): usado para guardar as ferramentas no corpo (Renderer::drawStowed)\nstatic const float kHandGrip[16] = {" + ", ".join("%.6ff" % HG[r][c] for r in range(4) for c in range(4)) + "};\n")
        f.write("static const float kTorchHeadTop[3] = {%.5ff, %.5ff, %.5ff};\n" % (top.x, top.y, top.z))
    print("ESCRITO torch_tip.h")
