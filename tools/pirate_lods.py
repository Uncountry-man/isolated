# LODs dos piratas (roda DENTRO do Blender, chamado por tools/export_pirates.py):
#   flatpak run --filesystem=host org.blender.Blender -b -P /abs/tools/pirate_lods.py -- /abs/build/pirates
# Para cada build/pirates/pirate_<x>.npz (P, N, B, W, UV, I já no espaço do jogo, com pesos) monta a malha (vértices soldados por posição, UV por canto,
# 1 grupo de vértices por osso), aplica o Decimate (colapso, que interpola UV e pesos) e grava pirate_<x>_lod1.npz (50%) e _lod2.npz (25%).
import bpy, sys, os
import numpy as np

work = sys.argv[sys.argv.index('--') + 1]
RATIOS = {1: 0.50, 2: 0.25}


def build(d):
    P, UV, I, B, W = d['P'], d['UV'], d['I'], d['B'], d['W']
    key = np.round(P * 5000).astype(np.int64)
    uniq, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inv = inv.reshape(-1)
    verts = P[first]
    faces = inv[I]
    ok = (faces[:, 0] != faces[:, 1]) & (faces[:, 1] != faces[:, 2]) & (faces[:, 0] != faces[:, 2])
    faces = faces[ok]; Iok = I[ok]
    me = bpy.data.meshes.new('pirate')
    me.from_pydata([tuple(map(float, v)) for v in verts], [], [tuple(map(int, f)) for f in faces])
    me.update()
    uvl = me.uv_layers.new(name='uv')
    # o Blender pode reordenar/descartar faces inválidas: confere a contagem
    assert len(me.polygons) == len(faces), (len(me.polygons), len(faces))
    for pi, poly in enumerate(me.polygons):
        for k, li in enumerate(poly.loop_indices):
            u, v = UV[Iok[pi][k]]
            uvl.data[li].uv = (float(u), float(v))
    ob = bpy.data.objects.new('pirate', me)
    bpy.context.scene.collection.objects.link(ob)
    groups = [ob.vertex_groups.new(name='b%02d' % i) for i in range(65)]
    for vi, src in enumerate(first):
        for k in range(4):
            if W[src][k] > 0: groups[int(B[src][k])].add([vi], float(W[src][k]) / 255.0, 'ADD')
    for p in me.polygons: p.use_smooth = True
    return ob


def export(ob, path):
    dg = bpy.context.evaluated_depsgraph_get()
    me = ob.evaluated_get(dg).to_mesh()
    me.calc_loop_triangles()
    uvl = me.uv_layers[0].data
    out = {}; P = []; N = []; B = []; W = []; UV = []; I = []
    for tri in me.loop_triangles:
        for li in tri.loops:
            vi = me.loops[li].vertex_index
            uv = uvl[li].uv
            k = (vi, round(uv[0], 5), round(uv[1], 5))
            if k not in out:
                out[k] = len(P)
                v = me.vertices[vi]
                P.append(tuple(v.co)); N.append(tuple(v.normal)); UV.append((uv[0], uv[1]))
                gs = sorted(((g.weight, int(ob.vertex_groups[g.group].name[1:])) for g in v.groups), reverse=True)[:4]
                tot = sum(w for w, _ in gs) or 1.0
                q = [int(w / tot * 255 + 0.5) for w, _ in gs]
                if not q: q = [255]; gs = [(1.0, 0)]
                q[0] += 255 - sum(q)
                b = [g for _, g in gs]
                while len(q) < 4: q.append(0); b.append(0)
                B.append(b); W.append(q)
            I.append(out[k])
    np.savez(path, P=np.array(P, np.float64), N=np.array(N, np.float64), B=np.array(B, np.uint8), W=np.array(W, np.uint8),
             UV=np.array(UV, np.float64), I=np.array(I, np.int64).reshape(-1, 3))
    return len(I) // 3


for f in sorted(os.listdir(work)):
    if not f.endswith('.npz') or '_lod' in f: continue
    d = np.load(os.path.join(work, f))
    for lod, ratio in RATIOS.items():
        ob = build(d)
        m = ob.modifiers.new('dec', 'DECIMATE')
        m.decimate_type = 'COLLAPSE'; m.ratio = ratio; m.use_collapse_triangulate = True
        n = export(ob, os.path.join(work, f.replace('.npz', '_lod%d.npz' % lod)))
        print('LOD', f, lod, n, 'tris')
        bpy.data.objects.remove(ob)
