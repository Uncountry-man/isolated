# Blender: lista objetos/malhas/materiais/texturas de cada FBX de import assets/modelos3d
# uso: blender -b -P tools/inspect_props.py
import bpy, os, glob
from io_scene_fbx import import_fbx

# Blender 5.x: o importador FBX quebra em luzes (cycles.cast_shadow não existe mais) -> ignora luzes
_orig_light = import_fbx.blen_read_light
def _safe_light(*a, **k):
    try:
        return _orig_light(*a, **k)
    except AttributeError:
        return bpy.data.lights.new("ignorada", 'POINT')
import_fbx.blen_read_light = _safe_light

ROOT = "/home/neto/Área de trabalho/isolated/import assets/modelos3d"
for fbx in sorted(glob.glob(ROOT + "/**/*.fbx", recursive=True)):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=fbx)
    print("\n==========", os.path.relpath(fbx, ROOT))
    total = 0
    for o in bpy.context.scene.objects:
        if o.type != 'MESH':
            print("  [%s] %s" % (o.type, o.name)); continue
        me = o.data
        tris = sum(len(p.vertices) - 2 for p in me.polygons)
        total += tris
        d = o.dimensions
        mats = []
        for m in me.materials:
            if not m: continue
            texs = []
            if m.use_nodes:
                for n in m.node_tree.nodes:
                    if n.type == 'TEX_IMAGE' and n.image:
                        texs.append(os.path.basename(n.image.filepath))
            mats.append("%s%s" % (m.name, texs))
        print("  %-40s tris=%7d dim=(%.2f %.2f %.2f) loc=(%.2f %.2f %.2f) uv=%d mats=%s" % (
            o.name, tris, d.x, d.y, d.z, o.location.x, o.location.y, o.location.z, len(me.uv_layers), mats))
    print("  TOTAL tris:", total)
