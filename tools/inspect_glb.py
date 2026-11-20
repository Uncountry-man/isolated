import bpy, sys
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=sys.argv[sys.argv.index('--')+1])
for o in bpy.data.objects:
    print("OBJ", o.name, o.type, "dim", tuple(round(x,3) for x in o.dimensions), "loc", tuple(round(x,3) for x in o.location))
    if o.type=='MESH':
        me=o.data; me.calc_loop_triangles()
        print("  tris",len(me.loop_triangles),"mats",[m.name if m else None for m in me.materials])
        zs=[tuple(v.co) for v in me.vertices]
        for ax in range(3):
            print("  axis",ax,round(min(v[ax] for v in zs),3),round(max(v[ax] for v in zs),3))
for i in bpy.data.images: print("IMG", i.name, i.size[:])
