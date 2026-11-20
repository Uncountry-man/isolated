import bpy
from mathutils import Vector
R="/home/neto/Área de trabalho/isolated/import assets/modelos3d/"
def rep(tag):
    for o in bpy.data.objects:
        if o.type!='MESH': continue
        vs=[o.matrix_world@v.co for v in o.data.vertices]
        mn=[min(v[i] for v in vs) for i in range(3)]; mx=[max(v[i] for v in vs) for i in range(3)]
        print(tag,o.name,"verts",len(vs),"tris",sum(len(p.vertices)-2 for p in o.data.polygons),"min",[round(x,3) for x in mn],"max",[round(x,3) for x in mx],"uv",bool(o.data.uv_layers), "mats",[m.name for m in o.data.materials])
bpy.ops.wm.read_factory_settings(use_empty=True)
try:
    from io_scene_fbx import import_fbx
    _o=import_fbx.blen_read_light
    def _s(*a,**k):
        try: return _o(*a,**k)
        except AttributeError: return bpy.data.lights.new("d",'POINT')
    import_fbx.blen_read_light=_s
except Exception as e: print(e)
bpy.ops.import_scene.fbx(filepath=R+"coconut/source/duakho.fbx"); rep("COCO")
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=R+"palm_leave/scene.gltf"); rep("LEAF")
