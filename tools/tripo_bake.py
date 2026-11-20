# ESTÁGIO A dos piratas "tripo" (roda DENTRO do Blender; chamado por tools/export_tripo.py):
#   flatpak run --filesystem=host org.blender.Blender -b -P /abs/tools/tripo_bake.py -- ENTRADA.glb SAIDA_DIR NOME [alvos] [texturas]
# Entrada: scan de ~2 milhões de triângulos (1 malha, 3 texturas PBR, sem ossos). Saída em SAIDA_DIR:
#   NOME_lod<k>.npz  (P, N, UV, I: malha baixa, espaço glTF Y-up em metros do arquivo, vértice único por (posição, UV))
#   NOME_lod<k>.png  (cor do scan "assada" na malha baixa pelo Cycles)
# Cada LOD tem o seu desenho de UV e a sua textura (1024 / 512 / 256): decimar UV de um LOD para outro misturava as ilhas.
import bpy, bmesh, sys, os, math
import numpy as np

args = sys.argv[sys.argv.index('--') + 1:]
src, out, name = args[:3]
targets = [int(x) for x in (args[3] if len(args) > 3 else '12000,4500,1600').split(',')]
sizes = [int(x) for x in (args[4] if len(args) > 4 else '1024,512,256').split(',')]
os.makedirs(out, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
hi = meshes[0]
for o in bpy.context.scene.objects: o.select_set(False)
hi.select_set(True); bpy.context.view_layer.objects.active = hi
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
hi.parent = None
bm = bmesh.new(); bm.from_mesh(hi.data)
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)   # o scan vem com vértices duplicados nas costuras de UV (bordas "abertas" travam o Decimate); as UVs ficam nos loops
bm.to_mesh(hi.data); bm.free()
print('scan:', len(hi.data.polygons), 'faces', len(hi.data.vertices), 'verts')

sc = bpy.context.scene
sc.render.engine = 'CYCLES'; sc.cycles.device = 'CPU'; sc.cycles.samples = 8
sc.cycles.use_denoising = False
bk = sc.render.bake
bk.use_selected_to_active = True; bk.use_pass_direct = False; bk.use_pass_indirect = False; bk.use_pass_color = True
bk.cage_extrusion = 0.02; bk.max_ray_distance = 0.05
bk.target = 'IMAGE_TEXTURES'


def decimate_copy(srcob, tris, nm):
    """cópia sem UV, decimada (colapso) até ~tris triângulos."""
    me = srcob.data.copy(); ob = bpy.data.objects.new(nm, me); sc.collection.objects.link(ob)
    while me.uv_layers: me.uv_layers.remove(me.uv_layers[0])
    me.materials.clear()
    n = len(me.polygons)
    m = ob.modifiers.new('dec', 'DECIMATE'); m.decimate_type = 'COLLAPSE'; m.ratio = min(1.0, tris / n); m.use_collapse_triangulate = True
    bpy.context.view_layer.objects.active = ob
    for o in sc.objects: o.select_set(False)
    ob.select_set(True)
    bpy.ops.object.modifier_apply(modifier='dec')
    return ob


def unwrap(ob):
    """UV: o Smart UV em malha decimada (normais ruidosas) vira milhares de ilhas minúsculas; numa versão SUAVIZADA da mesma malha (mesma topologia)
    dá ~150 ilhas grandes. As posições originais voltam depois (a UV fica nos loops)."""
    for o in sc.objects: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob
    orig = [v.co.copy() for v in ob.data.vertices]
    m = ob.modifiers.new('s', 'SMOOTH'); m.factor = 0.9; m.iterations = int(os.environ.get('UV_SMOOTH', 30)); bpy.ops.object.modifier_apply(modifier='s')
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(float(os.environ.get('UV_ANGLE', 50))), island_margin=0.003, area_weight=0.5)
    bpy.ops.uv.average_islands_scale()
    bpy.ops.uv.pack_islands(rotate=True, rotate_method='ANY', shape_method='CONCAVE', margin=0.003)
    bpy.ops.object.mode_set(mode='OBJECT')
    for v, c in zip(ob.data.vertices, orig): v.co = c


def bake(lo, size):
    img = bpy.data.images.new('bake_' + lo.name, size, size, alpha=False)
    img.generated_color = (0.5, 0.5, 0.5, 1)
    mat = bpy.data.materials.new('m_' + lo.name); mat.use_nodes = True
    nt = mat.node_tree; node = nt.nodes.new('ShaderNodeTexImage'); node.image = img; nt.nodes.active = node
    lo.data.materials.clear(); lo.data.materials.append(mat)
    for o in sc.objects: o.select_set(False)
    hi.select_set(True); lo.select_set(True); bpy.context.view_layer.objects.active = lo
    bk.margin = max(4, size // 128); bk.margin_type = 'EXTEND'
    bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'})
    return img


def export(lo, path):
    me = lo.data
    me.calc_loop_triangles()
    uvl = me.uv_layers[0].data
    key = {}; P = []; N = []; UV = []; I = []
    for tri in me.loop_triangles:
        for li in tri.loops:
            vi = me.loops[li].vertex_index; uv = uvl[li].uv
            k = (vi, round(uv[0], 6), round(uv[1], 6))
            if k not in key:
                key[k] = len(P); v = me.vertices[vi]
                P.append((v.co[0], v.co[2], -v.co[1])); N.append((v.normal[0], v.normal[2], -v.normal[1])); UV.append((uv[0], 1.0 - uv[1]))   # Blender Z-up -> glTF Y-up; V de cima para baixo
            I.append(key[k])
    np.savez(path, P=np.array(P, np.float64), N=np.array(N, np.float64), UV=np.array(UV, np.float64), I=np.array(I, np.int64).reshape(-1, 3))
    return len(I) // 3


prev = hi
for k, (tris, size) in enumerate(zip(targets, sizes)):
    if k >= int(os.environ.get('MAXLOD', 9)): break
    d = decimate_copy(prev if k else hi, tris, 'lod%d' % k)
    prev = d
    lo = d.copy(); lo.data = d.data.copy(); lo.name = 'bake%d' % k; sc.collection.objects.link(lo)
    for p in lo.data.polygons: p.use_smooth = True
    unwrap(lo)
    img = bake(lo, size)
    img.filepath_raw = os.path.join(out, '%s_lod%d.png' % (name, k)); img.file_format = 'PNG'; img.save()
    n = export(lo, os.path.join(out, '%s_lod%d.npz' % (name, k)))
    print('LOD', k, n, 'tris', size, 'px')
    d.hide_set(True); lo.hide_set(True)
