import bpy, sys
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=sys.argv[sys.argv.index('--')+1])
for o in bpy.data.objects:
    print("OBJ", o.name, o.type, tuple(round(v,3) for v in o.dimensions), tuple(round(v,3) for v in o.scale), tuple(round(v,3) for v in o.location), o.parent.name if o.parent else None)
    if o.type=='MESH':
        m=o.data; print("  verts",len(m.vertices),"polys",len(m.polygons),"uv",len(m.uv_layers),"mats",[s.material.name if s.material else None for s in o.material_slots], "vgroups", len(o.vertex_groups))
        for s in o.material_slots:
            if s.material and s.material.node_tree:
                for n in s.material.node_tree.nodes:
                    if n.type=='BSDF_PRINCIPLED': print("  base", tuple(n.inputs['Base Color'].default_value))
                    if n.type=='TEX_IMAGE': print("  tex", n.image.name if n.image else None)
    if o.type=='ARMATURE': print("  bones", len(o.data.bones))
print("ANIMS", [a.name for a in bpy.data.actions])
print("UNITS", bpy.context.scene.unit_settings.scale_length)
