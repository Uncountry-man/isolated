# Exporta a ADAGA DE PEDRA (GLB) para assets/tool_dagger.e0m (E0M2) presa ao osso da MÃO DIREITA do Y Bot (mesmo esquema de export_tools.py /
# export_torch.py: skinning por GPU com osso = mão, peso 255; guardada na cintura via kHandGrip em engine/src/torch_tip.h).
# Roda no Blender (Flatpak; caminhos ABSOLUTOS):  flatpak run --filesystem=host org.blender.Blender -b -P /abs/tools/export_dagger.py --
# Otimização: 5160 tris -> ~340; sem textura (3 submalhas por região do albedo: pedra, madeira, couro).
import bpy, bmesh, os, sys, struct, math
from mathutils import Matrix, Vector

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'import assets', 'modelos3d', 'primal-stone-dagger')
GLB = os.path.join(SRC, 'primal_stone_dagger.glb')
TEX = os.path.join(SRC, 'textures', 'albedo_512.png')   # Image_0 do GLB reduzida a 512x512
YBOT = os.path.join(ROOT, 'import assets', 'Y Bot.fbx')
args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUTDIR = args[args.index('--out') + 1] if '--out' in args else None
GRIP_C = (-0.010, 0.132, 0.034); GRIP_H = (1.0, 0.0, 0.0); GRIP_E = (0.0, 1.0, 0.0)
for i, a in enumerate(args):
    if a == '--grip':
        v = [float(x) for x in args[i + 1].split(',')]; GRIP_H, GRIP_E = tuple(v[:3]), tuple(v[3:6])
DAGGER_LEN = 0.30         # comprimento total (m)
GRIP_FROM_END = 0.045     # do ponto de pegada até a ponta do cabo (m)
TARGETS = {'stone': 200, 'wood': 70, 'leather': 70}
COLORS = {'stone': (0.42, 0.43, 0.44), 'wood': (0.33, 0.20, 0.09), 'leather': (0.20, 0.12, 0.12)}

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


# ---- adaga ----
before = set(bpy.data.objects)
bpy.ops.import_scene.gltf(filepath=GLB)
tobj = [o for o in bpy.data.objects if o not in before and o.type == 'MESH'][0]
me = tobj.data
me.transform(tobj.matrix_world)            # a hierarquia do GLB tem escala: assa no mesh
tobj.parent = None; tobj.matrix_world = Matrix.Identity(4)
pts = [v.co.copy() for v in me.vertices]
cen = sum(pts, Vector()) / len(pts)
def principal(start, ortho=None):
    a = start.normalized()
    for _ in range(60):
        n = Vector()
        for p in pts:
            d = p - cen; n += d * d.dot(a)
        if ortho is not None: n = n - ortho * n.dot(ortho)
        a = n.normalized()
    return a
ax = principal(Vector((0.05, 1.0, 0.2)))
wd = principal(Vector((1.0, 0.1, 0.1)) - ax * 0.1, ortho=ax)       # direção da LARGURA da lâmina (2º eixo)

# classifica triângulos pela cor do albedo (UV): pedra cinza, madeira laranja-marrom, couro escuro arroxeado
img = bpy.data.images.load(TEX); W, H = img.size; px = list(img.pixels)
def sample(u, v):
    x = min(W - 1, max(0, int((u % 1.0) * W))); y = min(H - 1, max(0, int((v % 1.0) * H))); i = (y * W + x) * 4
    return px[i], px[i + 1], px[i + 2]
uv = me.uv_layers.active.data
me.calc_loop_triangles()
groups = {'stone': [], 'wood': [], 'leather': []}
for t in me.loop_triangles:
    us = [uv[l].uv for l in t.loops]; r, g, b = sample(sum(u.x for u in us) / 3, sum(u.y for u in us) / 3)
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    if r > 1.7 * b + 0.01 and lum > 0.045: key = 'wood'
    elif abs(r - b) < 0.03 and lum > 0.035: key = 'stone'
    else: key = 'leather'
    groups[key].append(t)
print("TRIS fonte:", {k: len(v) for k, v in groups.items()})
# a PONTA da lâmina é o lado da pedra: eixo cabo -> lâmina
sc = [sum((me.vertices[i].co - cen).dot(ax) for i in t.vertices) / 3 for t in groups['stone']]
if sum(sc) / max(len(sc), 1) < 0: ax = -ax
ts = [(p - cen).dot(ax) for p in pts]; lo, hi = min(ts), max(ts)
print("EIXO", tuple(round(x, 3) for x in ax), "comprimento (unid. fonte)", round(hi - lo, 3))
sec = (wd - ax * wd.dot(ax)).normalized()
xs = ax.cross(sec).normalized() * -1.0
Bm = Matrix(((xs.x, ax.x, sec.x), (xs.y, ax.y, sec.y), (xs.z, ax.z, sec.z))).to_4x4()
scale = DAGGER_LEN / (hi - lo)
low_pt = cen + ax * lo
to_local = Matrix.Scale(scale, 4) @ Bm.inverted() @ Matrix.Translation(-(low_pt + ax * (GRIP_FROM_END / scale)))
M_full = T_B2E @ BONE_UNIT @ grip_matrix() @ to_local

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
for key in ('stone', 'wood', 'leather'):
    if groups[key]:
        v, n, i = submesh(groups[key], TARGETS[key]); subs.append((COLORS[key], v, n, i))
outdir = OUTDIR or os.path.join(ROOT, 'assets')
with open(os.path.join(outdir, 'tool_dagger.e0m'), 'wb') as f:
    f.write(b'E0M2'); f.write(struct.pack('<I', len(subs)))
    for color, verts, norms, idx in subs:
        f.write(struct.pack('<3fII', *color, len(verts), len(idx)))
        for v, n in zip(verts, norms):
            f.write(struct.pack('<6f8B', v.x, v.y, v.z, n.x, n.y, n.z, hand_idx, 0, 0, 0, 255, 0, 0, 0))
        f.write(struct.pack('<%dI' % len(idx), *idx))
print("ESCRITO tool_dagger.e0m", [(len(s[1]), len(s[3]) // 3) for s in subs], "(verts, tris)")
