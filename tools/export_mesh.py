# Converte um FBX para o formato binário .e0m (E0M2, com pesos de skinning) do Isolated (roda dentro do Blender).
# Uso: blender -b -P tools/export_mesh.py -- entrada.fbx saida.e0m [razao_decimacao]
#   razao 1.0 (padrão) = malha completa; 0.2 = ~20% dos triângulos (LOD1); 0.05 (LOD2). Os LODs servem para
#   jogadores distantes, para o mapa de sombra e para o reflexo (menos triângulos = mais FPS no multiplayer).
# Formato E0M2 (little-endian): "E0M2" u32 | nSub u32 | por submalha: float rgb[3], u32 nv, u32 ni,
#   nv*(pos xyz f32, normal xyz f32, bones u8[4], weights u8[4]) (32 B/vértice), ni*u32 índices.
#   Eixos: Y para cima, frente = -Z, pés em y=0, centrado em x/z.
import bpy, sys, struct
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
src, dst = args[0], args[1]
ratio = float(args[2]) if len(args) > 2 else 1.0

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)

arm = None
for o in bpy.data.objects:
    if o.type == 'ARMATURE':
        o.data.pose_position = 'REST'   # pose de repouso (T-pose)
        arm = o

bone_idx = {}
if arm:
    for i, b in enumerate(arm.data.bones):
        bone_idx[b.name] = i

def conv(v):  # Blender (Z-up, frente -Y) -> engine (Y-up, frente -Z)
    return (-v.x, v.z, v.y)

# Calcula a caixa envolvente na malha original (sem decimação) para que todos os LODs tenham exatamente a mesma origem.
bpy.context.view_layer.update()
dg0 = bpy.context.evaluated_depsgraph_get()
base_verts = []
for o in bpy.data.objects:
    if o.type != 'MESH': continue
    eo = o.evaluated_get(dg0); me = eo.to_mesh()
    mw = o.matrix_world
    base_verts += [conv(mw @ v.co) for v in me.vertices]
    eo.to_mesh_clear()

mn = [min(v[i] for v in base_verts) for i in range(3)]
mx = [max(v[i] for v in base_verts) for i in range(3)]
off = ((mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2)

if ratio < 0.999:
    for o in bpy.data.objects:
        if o.type == 'MESH':
            m = o.modifiers.new('lod', 'DECIMATE')
            m.decimate_type = 'COLLAPSE'; m.ratio = ratio; m.use_collapse_triangulate = True

bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()

subs, allv = [], []
for o in bpy.data.objects:
    if o.type != 'MESH': continue
    eo = o.evaluated_get(dg); me = eo.to_mesh()
    me.calc_loop_triangles()
    mw = o.matrix_world
    verts = [conv(mw @ v.co) for v in me.vertices]
    nm = mw.to_3x3().inverted_safe().transposed()
    norms = [conv((nm @ v.normal).normalized()) for v in me.vertices]
    vg_map = {i: bone_idx.get(vg.name, -1) for i, vg in enumerate(o.vertex_groups)}
    skin = []
    for v in me.vertices:
        bw = []
        for g in v.groups:
            bi = vg_map.get(g.group, -1)
            if bi >= 0 and g.weight > 1e-4:
                bw.append((bi, g.weight))
        bw.sort(key=lambda x: -x[1])
        bw = bw[:4]
        while len(bw) < 4:
            bw.append((0, 0.0))
        tw = sum(w for _, w in bw)
        if tw > 1e-6:
            wq = [int(round(w / tw * 255.0)) for _, w in bw]
            wq[0] = max(0, min(255, wq[0] + (255 - sum(wq))))
            bi = [b for b, _ in bw]
        else:
            bi = [0, 0, 0, 0]
            wq = [255, 0, 0, 0]
        skin.append((bi[0], bi[1], bi[2], bi[3], wq[0], wq[1], wq[2], wq[3]))
    idx = [i for t in me.loop_triangles for i in t.vertices]
    col = (0.5, 0.5, 0.5)
    if o.material_slots and o.material_slots[0].material and o.material_slots[0].material.node_tree:
        for n in o.material_slots[0].material.node_tree.nodes:
            if n.type == 'BSDF_PRINCIPLED':
                c = n.inputs['Base Color'].default_value; col = (c[0], c[1], c[2])
    subs.append((o.name, col, verts, norms, skin, idx)); allv += verts
    eo.to_mesh_clear()

# sanidade da orientação: bico dos pés (8% mais baixos) deve ficar em -Z em relação ao centro
low = [v for v in allv if v[1] < mn[1] + 0.08 * (mx[1] - mn[1])]
toe = sum(v[2] for v in low) / len(low) - off[2]
print("BBOX min", [round(x, 3) for x in mn], "max", [round(x, 3) for x in mx], "altura", round(mx[1] - mn[1], 3), "pé_z_médio", round(toe, 3))

with open(dst, 'wb') as f:
    f.write(b'E0M2'); f.write(struct.pack('<I', len(subs)))
    for name, col, verts, norms, skin, idx in subs:
        f.write(struct.pack('<3fII', *col, len(verts), len(idx)))
        for v, n, sk in zip(verts, norms, skin):
            f.write(struct.pack('<6f8B', v[0] - off[0], v[1] - off[1], v[2] - off[2], n[0], n[1], n[2], *sk))
        f.write(struct.pack('<%dI' % len(idx), *idx))
        print("SUB", name, col, len(verts), "verts", len(idx) // 3, "tris")
