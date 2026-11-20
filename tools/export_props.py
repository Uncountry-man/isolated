import bpy, os, sys, math
from mathutils import Vector, Matrix
from io_scene_fbx import import_fbx

# Monkeypatch light import bug in Blender 5.2.0
_orig_light = import_fbx.blen_read_light
def _safe_light(*a, **k):
    try: return _orig_light(*a, **k)
    except AttributeError: return bpy.data.lights.new("dummy_light", 'POINT')
import_fbx.blen_read_light = _safe_light

ROOT = "/home/neto/Área de trabalho/isolated/import assets/modelos3d"
OUT_HEADER = "/home/neto/Área de trabalho/isolated/engine/src/props_data.h"

def load_texture_pixels(tex_path):
    if not os.path.exists(tex_path):
        print(f"Warning: texture not found: {tex_path}")
        return None, 0, 0
    img = bpy.data.images.load(tex_path)
    iw, ih = img.size[0], img.size[1]
    pixels = list(img.pixels)
    print(f"Loaded texture {os.path.basename(tex_path)}: {iw}x{ih}")
    return pixels, iw, ih

def sample_pixels(pixels, iw, ih, u, v):
    if not pixels:
        return (180, 180, 180)
    px = int((u % 1.0) * (iw - 1))
    py = int((v % 1.0) * (ih - 1))
    idx = (py * iw + px) * 4
    return (int(min(255, max(0, pixels[idx] * 255))),
            int(min(255, max(0, pixels[idx+1] * 255))),
            int(min(255, max(0, pixels[idx+2] * 255))))

def decimate_mesh(obj, target_tris):
    curr_tris = sum(len(p.vertices) - 2 for p in obj.data.polygons)
    if curr_tris > target_tris:
        ratio = max(0.00005, min(1.0, float(target_tris) / float(curr_tris)))
        mod = obj.modifiers.new(name="Decimate", type='DECIMATE')
        mod.ratio = ratio
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.modifier_apply(modifier="Decimate")
        print(f"Decimated {obj.name}: {curr_tris} -> {len(obj.data.polygons)} polys")

def extract_mesh_triangles(obj, pixels_info, transform_fn):
    dg = bpy.context.evaluated_depsgraph_get()
    eo = obj.evaluated_get(dg)
    me = eo.to_mesh()
    me.calc_loop_triangles()

    uv_layer = me.uv_layers.active.data if me.uv_layers.active else None
    pixels, iw, ih = pixels_info if pixels_info else (None, 0, 0)

    verts_out = []
    for tri in me.loop_triangles:
        fn = tri.normal
        for i in range(3):
            vi = tri.vertices[i]
            li = tri.loops[i]
            co = me.vertices[vi].co
            vn = me.vertices[vi].normal if tri.use_smooth else fn
            
            p, n = transform_fn(co, vn)
            nl = math.sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2])
            if nl > 1e-6:
                n = (n[0]/nl, n[1]/nl, n[2]/nl)
            else:
                n = (0.0, 1.0, 0.0)

            if uv_layer and pixels:
                u, v = uv_layer[li].uv
                c = sample_pixels(pixels, iw, ih, u, v)
            else:
                c = (160, 160, 160)
            verts_out.append((p, n, c))
    eo.to_mesh_clear()
    return verts_out

def pack_normal(val):
    iv = int(round(val * 127.0))
    return max(-127, min(127, iv))

# =========================================================================
# 1. PALM TREE & COCONUT TREE
# =========================================================================
print("Processing Palm and Coconut trees...")
fbx_palm = os.path.join(ROOT, "stylized-palm-coconut-tree-pack/source/stylized_palm_pack.fbx")
tex_palm = os.path.join(ROOT, "stylized-palm-coconut-tree-pack/textures/PalmTree_C.png")
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_palm)
pix_palm = load_texture_pixels(tex_palm)

# ObjPalm: PalmTree1
o_palm = bpy.data.objects.get("PalmTree1")
decimate_mesh(o_palm, 300)

p1_verts = [v.co for v in o_palm.data.vertices]
p1_min_z = min(v.z for v in p1_verts)
p1_max_z = max(v.z for v in p1_verts)
p1_base = [v for v in p1_verts if v.z < p1_min_z + 0.5]
p1_bcx = sum(v.x for v in p1_base) / len(p1_base)
p1_bcy = sum(v.y for v in p1_base) / len(p1_base)
palm_target_h = 6.9
palm_scale = palm_target_h / (p1_max_z - p1_min_z)

def xform_palm(co, norm):
    rel = Vector((co.x - p1_bcx, co.y - p1_bcy, co.z - p1_min_z))
    p = (-rel.x * palm_scale, rel.z * palm_scale, rel.y * palm_scale)
    n = (-norm.x, norm.z, norm.y)
    return p, n

palm_data = extract_mesh_triangles(o_palm, pix_palm, xform_palm)
print(f"ObjPalm extracted: {len(palm_data)} verts ({len(palm_data)//3} tris)")

# ObjCoconut: PalmTree3 + Coconut
o_palm3 = bpy.data.objects.get("PalmTree3")
o_coco = bpy.data.objects.get("Coconut")
decimate_mesh(o_palm3, 300)
decimate_mesh(o_coco, 36)

p3_verts = [v.co for v in o_palm3.data.vertices]
p3_min_z = min(v.z for v in p3_verts)
p3_max_z = max(v.z for v in p3_verts)
p3_base = [v for v in p3_verts if v.z < p3_min_z + 0.5]
p3_bcx = sum(v.x for v in p3_base) / len(p3_base)
p3_bcy = sum(v.y for v in p3_base) / len(p3_base)
coco_target_h = 5.6
coco_scale = coco_target_h / (p3_max_z - p3_min_z)

def xform_coco_tree(co, norm):
    rel = Vector((co.x - p3_bcx, co.y - p3_bcy, co.z - p3_min_z))
    p = (-rel.x * coco_scale, rel.z * coco_scale, rel.y * coco_scale)
    n = (-norm.x, norm.z, norm.y)
    return p, n

coco_data = extract_mesh_triangles(o_palm3, pix_palm, xform_coco_tree)

coco_cluster_pos = Vector((5.18, 3.54, 5.30))
coco_verts = [v.co for v in o_coco.data.vertices]
coco_center = sum(coco_verts, Vector((0,0,0))) / len(coco_verts)

def xform_coconuts(co, norm):
    rel_coco = co - coco_center
    world_co = coco_cluster_pos + rel_coco * 2.2
    rel = Vector((world_co.x - p3_bcx, world_co.y - p3_bcy, world_co.z - p3_min_z))
    p = (-rel.x * coco_scale, rel.z * coco_scale, rel.y * coco_scale)
    n = (-norm.x, norm.z, norm.y)
    return p, n

coco_nuts_data = extract_mesh_triangles(o_coco, pix_palm, xform_coconuts)
coconut_data = coco_data + coco_nuts_data
print(f"ObjCoconut extracted: {len(coconut_data)} verts ({len(coconut_data)//3} tris)")

# =========================================================================
# 2. ROCKS AND PEBBLES
# =========================================================================
print("Processing Rocks and Pebbles...")
fbx_stone = os.path.join(ROOT, "stone-pack/source/ST-PaCK.fbx")
tex_rock_big = os.path.join(ROOT, "stone-pack/textures/Big_02___Default_color.tga.png")
tex_rock_mid = os.path.join(ROOT, "stone-pack/textures/Mid_color.tga.png")
tex_rock_small = os.path.join(ROOT, "stone-pack/textures/Small_clr.tga.png")

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_stone)

pix_rock_big = load_texture_pixels(tex_rock_big)
pix_rock_mid = load_texture_pixels(tex_rock_mid)
pix_rock_small = load_texture_pixels(tex_rock_small)

def process_stone(obj_name, target_tris, target_radius, pix_info, sink_y=0.0):
    obj = bpy.data.objects.get(obj_name)
    decimate_mesh(obj, target_tris)
    
    mw = obj.matrix_world
    mw_verts = [mw @ v.co for v in obj.data.vertices]
    min_x = min(v.x for v in mw_verts); max_x = max(v.x for v in mw_verts)
    min_y = min(v.y for v in mw_verts); max_y = max(v.y for v in mw_verts)
    min_z = min(v.z for v in mw_verts); max_z = max(v.z for v in mw_verts)
    
    cx = (min_x + max_x) * 0.5
    cy = (min_y + max_y) * 0.5
    base_z = min_z
    
    cur_rx = (max_x - min_x) * 0.5
    cur_ry = (max_y - min_y) * 0.5
    cur_r = max(cur_rx, cur_ry)
    scale = target_radius / max(0.001, cur_r)
    
    nm = mw.to_3x3().inverted_safe().transposed()
    
    def xform(co, norm):
        w_co = mw @ co
        w_norm = (nm @ norm).normalized()
        rel_x = (w_co.x - cx) * scale
        rel_y = (w_co.y - cy) * scale
        rel_z = (w_co.z - base_z) * scale - sink_y
        p = (-rel_x, rel_z, rel_y)
        n = (-w_norm.x, w_norm.z, w_norm.y)
        return p, n

    data = extract_mesh_triangles(obj, pix_info, xform)
    return data

rocks_data = [
    process_stone("BIG_1", 110, 0.95, pix_rock_big, sink_y=0.08),
    process_stone("Mid_1", 90, 0.85, pix_rock_mid, sink_y=0.06),
    process_stone("Mid_2", 90, 0.85, pix_rock_mid, sink_y=0.06)
]
for i, rd in enumerate(rocks_data):
    print(f"ObjRock variant {i}: {len(rd)} verts ({len(rd)//3} tris)")

pebbles_data = [
    process_stone("Small_1", 24, 0.16, pix_rock_small, sink_y=-0.02),
    process_stone("Small_3", 24, 0.15, pix_rock_small, sink_y=-0.02),
    process_stone("Small_5", 24, 0.14, pix_rock_small, sink_y=-0.02)
]
for i, pd in enumerate(pebbles_data):
    print(f"ObjPebble variant {i}: {len(pd)} verts ({len(pd)//3} tris)")

# =========================================================================
# 3. STICK AND FALLEN LOG
# =========================================================================
print("Processing Stick and Fallen Log...")
fbx_stick = os.path.join(ROOT, "backyard-stick-lod0/source/Backyard Stick LOD0.fbx")
tex_stick = os.path.join(ROOT, "backyard-stick-lod0/textures/stick_u0_1k.png")

# Log (from large-tree-log)
gltf_large_log = os.path.join(ROOT, "large-tree-log/source/Unity2Skfb.gltf")
tex_large_log = os.path.join(ROOT, "large-tree-log/source/log_albedo_1k.png")

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=gltf_large_log)
pix_large_log = load_texture_pixels(tex_large_log)
obj_log = bpy.data.objects.get("ForestLogLarge_1")
decimate_mesh(obj_log, 220)

log_verts = [v.co for v in obj_log.data.vertices]
lx_min, lx_max = min(v.x for v in log_verts), max(v.x for v in log_verts)
ly_min, ly_max = min(v.y for v in log_verts), max(v.y for v in log_verts)
lz_min, lz_max = min(v.z for v in log_verts), max(v.z for v in log_verts)
lx_len = lx_max - lx_min
lx_mid = (lx_max + lx_min) * 0.5
ly_mid = (ly_max + ly_min) * 0.5

log_scale_x = 4.60 / lx_len
log_scale_r = 0.76 / (ly_max - ly_min)

def xform_log(co, norm):
    rel_x = (co.x - lx_mid) * log_scale_x
    rel_y = (co.z - lz_min) * log_scale_r + 0.02
    rel_z = -(co.y - ly_mid) * log_scale_r
    p = (rel_x, rel_y, rel_z)
    n = (norm.x, norm.z, -norm.y)
    return p, n

log_data = extract_mesh_triangles(obj_log, pix_large_log, xform_log)
print(f"ObjLog extracted: {len(log_data)} verts ({len(log_data)//3} tris)")

# Stick (collectible)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_stick)
pix_stick = load_texture_pixels(tex_stick)
obj_stk = bpy.data.objects.get("complete2_LOD0")
decimate_mesh(obj_stk, 1000)
bpy.context.view_layer.objects.active = obj_stk
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.remove_doubles(threshold=1.2)
bpy.ops.object.mode_set(mode='OBJECT')
print(f"ObjStick after remove_doubles: {len(obj_stk.data.polygons)} polys")

stk_verts = [v.co for v in obj_stk.data.vertices]
sz_min, sz_max = min(v.z for v in stk_verts), max(v.z for v in stk_verts)
sx_min, sx_max = min(v.x for v in stk_verts), max(v.x for v in stk_verts)
sy_min, sy_max = min(v.y for v in stk_verts), max(v.y for v in stk_verts)
sz_len = sz_max - sz_min
sz_mid = (sz_max + sz_min) * 0.5
sx_mid = (sx_max + sx_min) * 0.5
sy_mid = (sy_max + sy_min) * 0.5

stk_scale_x = 0.60 / sz_len
stk_scale_r = 0.025 / max(0.001, max(sx_max - sx_min, sy_max - sy_min) * 0.5)

def xform_stick(co, norm):
    rel_z = (co.z - sz_mid) * stk_scale_x
    rel_x = (co.x - sx_mid) * stk_scale_r
    rel_y = (co.y - sy_mid) * stk_scale_r
    p = (rel_z, rel_y + 0.025, rel_x)
    n = (norm.z, norm.y, norm.x)
    return p, n

stick_data = extract_mesh_triangles(obj_stk, pix_stick, xform_stick)
print(f"ObjStick extracted: {len(stick_data)} verts ({len(stick_data)//3} tris)")

# =========================================================================
# 4. GRASS TUFT
# =========================================================================
print("Processing Grass tuft...")
fbx_grass = os.path.join(ROOT, "grass/source/Grass.fbx")
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx_grass)

obj_grass = bpy.data.objects.get("Grass1")
decimate_mesh(obj_grass, 10)

dg = bpy.context.evaluated_depsgraph_get()
eo = obj_grass.evaluated_get(dg)
me = eo.to_mesh()
me.calc_loop_triangles()

g_tris = list(me.loop_triangles)[:7]
g_coords = []
for tri in g_tris:
    for i in range(3):
        g_coords.append(Vector(me.vertices[tri.vertices[i]].co))

gz_min = min(v.z for v in g_coords); gz_max = max(v.z for v in g_coords)
gx_min = min(v.x for v in g_coords); gx_max = max(v.x for v in g_coords)
gy_min = min(v.y for v in g_coords); gy_max = max(v.y for v in g_coords)
gx_mid = (gx_min + gx_max) * 0.5
gy_mid = (gy_min + gy_max) * 0.5
g_height = max(0.001, gz_max - gz_min)
cur_gr = max(0.001, max(gx_max - gx_min, gy_max - gy_min) * 0.5)
target_gr = 0.22 # ~22cm tuft radius
gr_scale = target_gr / cur_gr

tuft_verts = []
for v in g_coords:
    # Target radius ~0.22m, Y normalized in [0.0, 1.0]
    nx = -(v.x - gx_mid) * gr_scale
    ny = (v.z - gz_min) / g_height
    nz = (v.y - gy_mid) * gr_scale
    tuft_verts.append((nx, ny, nz))

eo.to_mesh_clear()
print(f"Grass tuft extracted: {len(tuft_verts)} vertices (7 triangles, radius={target_gr}m)")

# =========================================================================
# 4b. COCO (item solto) e FOLHA DE PALMEIRA (item solto)
# =========================================================================
print("Processing Coconut item and Palm leaf item...")
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, "coconut/source/duakho.fbx"))
pix_coco_item = load_texture_pixels(os.path.join(ROOT, "coconut/textures/TexturesCom_Coconut_1K_albedo.png"))
o_ci = [o for o in bpy.data.objects if o.type == 'MESH'][0]
decimate_mesh(o_ci, 90)
mwc = o_ci.matrix_world
cv = [mwc @ v.co for v in o_ci.data.vertices]
cx_min, cx_max = min(v.x for v in cv), max(v.x for v in cv)
cy_min, cy_max = min(v.y for v in cv), max(v.y for v in cv)
cz_min, cz_max = min(v.z for v in cv), max(v.z for v in cv)
coco_item_scale = 0.23 / max(cx_max - cx_min, cy_max - cy_min, cz_max - cz_min)   # maior dimensão = 23 cm
cnm = mwc.to_3x3().inverted_safe().transposed()
def xform_coco_item(co, norm):
    w = mwc @ co
    wn = (cnm @ norm).normalized()
    p = ((w.x - (cx_min + cx_max) * 0.5) * coco_item_scale, (w.z - cz_min) * coco_item_scale, -(w.y - (cy_min + cy_max) * 0.5) * coco_item_scale)
    return p, (wn.x, wn.z, -wn.y)
coco_item_data = extract_mesh_triangles(o_ci, pix_coco_item, xform_coco_item)
print(f"Coconut item: {len(coco_item_data)//3} tris")

# FOLHA: a malha do pacote é um cartão com a forma recortada só no canal alfa da textura. Aqui a silhueta (recortes, talo, pontas)
# é reconstruída como GEOMETRIA a partir do alfa: por linha da textura pega a borda esquerda/direita, simplifica (Douglas-Peucker) e
# gera uma tira com nervura central levantada, arqueada e com as bordas caídas. Dupla face (frente + verso), cor verde da luminância.
import numpy as np
img = bpy.data.images.load(os.path.join(ROOT, "palm_leave/textures/Material.001_baseColor.png"))
IW, IH = img.size
arr = np.empty(IW * IH * 4, dtype=np.float32)
img.pixels.foreach_get(arr)
arr = arr.reshape(IH, IW, 4)                      # linha 0 = base da imagem (v = 0)
half = arr[:, : IW // 2, :]                       # a folha da esquerda (a da direita é o verso)
mask = half[:, :, 3] > 0.5
rows = np.where(mask.any(axis=1))[0]
r0, r1 = int(rows.min()), int(rows.max())
step = 3
ridx = list(range(r0, r1 + 1, step))
L = []; R = []
for r in ridx:
    cols = np.where(mask[r])[0]
    L.append(float(cols.min())); R.append(float(cols.max()))
colc = (min(L) + max(R)) * 0.5
def rdp(lo, hi, tol, keep):
    best, bi = 0.0, -1
    for i in range(lo + 1, hi):
        t = (i - lo) / float(hi - lo)
        e = max(abs(L[i] - (L[lo] + (L[hi] - L[lo]) * t)), abs(R[i] - (R[lo] + (R[hi] - R[lo]) * t)))
        if e > best: best, bi = e, i
    if bi >= 0 and best > tol:
        keep.add(bi); rdp(lo, bi, tol, keep); rdp(bi, hi, tol, keep)
keep = {0, len(ridx) - 1}
rdp(0, len(ridx) - 1, 5.0, keep)
sel = sorted(keep)
LEN = 1.15
sc = LEN / float(r1 - r0)
maxhalf = max((R[i] - L[i]) * 0.5 for i in sel) * sc
def lum_at(col, row):
    px = half[min(IH - 1, row), min(IW // 2 - 1, int(col))]
    return float(px[0])
def leaf_color(t, ax, row, col):
    lm = lum_at(col, row)
    g = (0.19 + 0.11 * t, 0.46 + 0.14 * (1 - t) + 0.06 * t, 0.12)
    k = 0.60 + 0.55 * lm
    rib = 0.72 if abs(ax) < 0.012 else 1.0        # nervura central mais escura
    return tuple(int(255 * min(1.0, c * k * rib)) for c in g)
def pos(t, ax):
    x = t * LEN
    h = 0.20 * math.sin(math.pi * min(1.0, t * 1.05)) + 0.03 * (1 - t)      # arco: talo no chão, sobe e cai na ponta
    y = h + 0.06 - 0.42 * abs(ax) + (0.025 if abs(ax) < 1e-6 else 0.0)        # bordas caídas, nervura levemente alta
    return (x, max(0.01, y), ax)
rowsV = []
for i in sel:
    r = ridx[i]; t = (r - r0) / float(r1 - r0)
    lx = (L[i] - colc) * sc; rx = (R[i] - colc) * sc
    rowsV.append((t, (lx, 0.0, rx), r, (L[i], (L[i] + R[i]) * 0.5, R[i])))
def tri_out(P, C, out):
    (a_, b_, c_) = P
    e1 = [b_[k] - a_[k] for k in range(3)]; e2 = [c_[k] - a_[k] for k in range(3)]
    n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    nl = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
    n = (n[0] / nl, n[1] / nl, n[2] / nl)
    if n[1] < 0: n = (-n[0], -n[1], -n[2]); P = (a_, c_, b_); C = (C[0], C[2], C[1])   # frente sempre voltada para cima
    out.append((P[0], n, C[0])); out.append((P[1], n, C[1])); out.append((P[2], n, C[2]))
    out.append((P[0], (-n[0], -n[1], -n[2]), C[0])); out.append((P[2], (-n[0], -n[1], -n[2]), C[2])); out.append((P[1], (-n[0], -n[1], -n[2]), C[1]))   # verso
frond_data = []
def vtx(rv, which):
    t, (lx, mx, rx), r, cols = rv
    ax = (lx, mx, rx)[which]
    return pos(t, ax), leaf_color(t, ax, r, cols[which])
for a_i in range(len(rowsV) - 1):
    A, B = rowsV[a_i], rowsV[a_i + 1]
    for (w0, w1) in ((0, 1), (1, 2)):
        v00, v01, v10, v11 = vtx(A, w0), vtx(A, w1), vtx(B, w0), vtx(B, w1)
        tri_out((v00[0], v01[0], v11[0]), (v00[1], v01[1], v11[1]), frond_data)
        tri_out((v00[0], v11[0], v10[0]), (v00[1], v11[1], v10[1]), frond_data)
# centraliza em X (talo na origem do desenho fica no meio da folha)
cxm = LEN * 0.5
frond_data = [((p[0] - cxm, p[1], p[2]), n, c) for p, n, c in frond_data]
print(f"Palm leaf item: {len(frond_data)//3} tris")

# =========================================================================
# 5. WRITE HEADER FILE props_data.h
# =========================================================================
print(f"Writing {OUT_HEADER}...")

def format_vertex_array(name, data):
    lines = []
    lines.append(f"static const PropVertex {name}[{len(data)}] = {{")
    for p, n, c in data:
        lines.append(f"    {{{p[0]:.4f}f, {p[1]:.4f}f, {p[2]:.4f}f, {pack_normal(n[0])}, {pack_normal(n[1])}, {pack_normal(n[2])}, 0, {c[0]}, {c[1]}, {c[2]}, 255}},")
    lines.append("};")
    return "\n".join(lines)

with open(OUT_HEADER, "w") as f:
    f.write("""// props_data.h - Modelos 3D otimizados para Isolated Mobile (extraídos via Blender)
// Autogerado por tools/export_props.py - NÃO EDITE MANUALMENTE
#pragma once
#include <cstdint>
#include <cstddef>

struct PropVertex {
    float x, y, z;
    int8_t nx, ny, nz, nw;
    uint8_t r, g, b, a;
};

struct PropModel {
    const PropVertex* vertices;
    size_t count;
};

""")
    f.write(format_vertex_array("kPropPalmVerts", palm_data) + "\n\n")
    f.write(format_vertex_array("kPropCoconutVerts", coconut_data) + "\n\n")
    for i, rd in enumerate(rocks_data):
        f.write(format_vertex_array(f"kPropRockVerts_{i}", rd) + "\n\n")
    for i, pd in enumerate(pebbles_data):
        f.write(format_vertex_array(f"kPropPebbleVerts_{i}", pd) + "\n\n")
    f.write(format_vertex_array("kPropLogVerts", log_data) + "\n\n")
    f.write(format_vertex_array("kPropStickVerts", stick_data) + "\n\n")
    f.write(format_vertex_array("kPropCoconutItemVerts", coco_item_data) + "\n\n")
    f.write(format_vertex_array("kPropFrondItemVerts", frond_data) + "\n\n")

    f.write(f"static const PropModel kPropPalm = {{ kPropPalmVerts, {len(palm_data)} }};\n")
    f.write(f"static const PropModel kPropCoconut = {{ kPropCoconutVerts, {len(coconut_data)} }};\n")
    f.write("static const PropModel kPropRocks[3] = {\n")
    for i, rd in enumerate(rocks_data):
        f.write(f"    {{ kPropRockVerts_{i}, {len(rd)} }},\n")
    f.write("};\n")
    f.write("static const PropModel kPropPebbles[3] = {\n")
    for i, pd in enumerate(pebbles_data):
        f.write(f"    {{ kPropPebbleVerts_{i}, {len(pd)} }},\n")
    f.write("};\n")
    f.write(f"static const PropModel kPropLog = {{ kPropLogVerts, {len(log_data)} }};\n")
    f.write(f"static const PropModel kPropStick = {{ kPropStickVerts, {len(stick_data)} }};\n")
    f.write(f"static const PropModel kPropCoconutItem = {{ kPropCoconutItemVerts, {len(coco_item_data)} }};\n")
    f.write(f"static const PropModel kPropFrondItem = {{ kPropFrondItemVerts, {len(frond_data)} }};\n\n")

    f.write(f"static const size_t kTuftVertCount = {len(tuft_verts)};\n")
    f.write(f"static const float kTuftMeshVerts[{len(tuft_verts) * 3}] = {{\n")
    for i in range(0, len(tuft_verts), 3):
        v0, v1, v2 = tuft_verts[i], tuft_verts[i+1], tuft_verts[i+2]
        f.write(f"    {v0[0]:.4f}f, {v0[1]:.4f}f, {v0[2]:.4f}f,  {v1[0]:.4f}f, {v1[1]:.4f}f, {v1[2]:.4f}f,  {v2[0]:.4f}f, {v2[1]:.4f}f, {v2[2]:.4f}f,\n")
    f.write("};\n")

print("Finished generating props_data.h successfully!")
