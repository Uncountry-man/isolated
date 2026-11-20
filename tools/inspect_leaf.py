import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath="/home/neto/Área de trabalho/isolated/import assets/modelos3d/palm_leave/scene.gltf")
o=[x for x in bpy.data.objects if x.type=='MESH'][0]
me=o.data; uv=me.uv_layers.active.data
print("MAT blend", o.data.materials[0].blend_method if hasattr(o.data.materials[0],'blend_method') else None)
for v in sorted(me.vertices,key=lambda v:v.co.y):
    print("V %.3f %.3f %.3f"%tuple(o.matrix_world@v.co))
