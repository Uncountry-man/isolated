# Pré-visualiza clipes do Mixamo com a ferramenta (assets/tool_*.e0m) presa à mão direita, em folha de contato.
# Uso (Blender Flatpak, caminhos ABSOLUTOS):
#   blender -b -P /abs/tools/preview_clips.py -- LISTA.txt SAIDA_DIR TOOL(hatchet|pickaxe) NFRAMES [--fist] [--twist DEG] [--front]
# LISTA.txt: um clipe por linha, relativo a "import assets/" (ex.: Standing Torch Melee Attack 02.fbx).
# Gera SAIDA_DIR/c<clipe>_<k>_f<frame>.png (NFRAMES quadros uniformes, vista pelo lado direito); tools/compose_sheets.py junta em folhas.
import bpy, os, sys, math, struct
from mathutils import Matrix, Vector, Quaternion

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[sys.argv.index('--') + 1:]
LIST, OUTDIR, TOOL, NF = args[0], args[1], args[2], int(args[3])
USE_FIST = '--fist' in args
TWIST = float(args[args.index('--twist') + 1]) if '--twist' in args else 0.0
BEND = float(args[args.index('--bend') + 1]) if '--bend' in args else 0.0
FRONT = '--front' in args
VIEW = args[args.index('--view') + 1] if '--view' in args else ('front' if FRONT else 'side')   # side|front|back|q|qb|hand
TOOLFILE = args[args.index('--toolfile') + 1] if '--toolfile' in args else None
ARMFROM = args[args.index('--armfrom') + 1] if '--armfrom' in args else None   # camada do braço direito (31..54) de outro clipe, em fase
FISTCLIP = ('_Packs/Pro Melee Axe Pack/standing idle.fbx', 10)
RANGE = (int(args[args.index('--range') + 1]), int(args[args.index('--range') + 2])) if '--range' in args else None
os.makedirs(OUTDIR, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', 'Y Bot.fbx'))
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
hb = arm.data.bones[hand_name]
bind_inv = (M_arm @ hb.matrix_local).inverted()

# ferramenta (já em espaço de bind da engine) -> espaço Blender-bind
def read_e0m(path):
    b = open(path, 'rb').read(); assert b[:4] == b'E0M2'
    nsub = struct.unpack_from('<I', b, 4)[0]; o = 8; subs = []
    for _ in range(nsub):
        col = struct.unpack_from('<3f', b, o); nv, ni = struct.unpack_from('<II', b, o + 12); o += 20
        vs = []
        for i in range(nv):
            x, y, z = struct.unpack_from('<3f', b, o + i * 32); vs.append(T_B2E.inverted() @ Vector((x, y, z)))
        o += nv * 32; idx = struct.unpack_from('<%dI' % ni, b, o); o += ni * 4
        subs.append((col, vs, [tuple(idx[i:i + 3]) for i in range(0, ni, 3)]))
    return subs
subs = [] if TOOL == 'none' else read_e0m(TOOLFILE or os.path.join(ROOT, 'assets', 'tool_%s.e0m' % TOOL))

# mão fechada = dedos de um quadro real do Mixamo segurando machado (mesma fonte de tools/export_tools.py)
FIST_BASIS = {}
if USE_FIST:
    _b = set(bpy.data.objects); bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', FISTCLIP[0]))
    _n = [o for o in bpy.data.objects if o not in _b]; _a = [o for o in _n if o.type == 'ARMATURE'][0].animation_data.action
    arm.animation_data_create(); arm.animation_data.action = _a; arm.data.pose_position = 'POSE'
    bpy.context.scene.frame_set(FISTCLIP[1]); bpy.context.view_layer.update()
    for i in range(35, 55):
        pb = arm.pose.bones[bones[i].name]; pb.rotation_mode = 'QUATERNION'
        FIST_BASIS[i] = (pb.location.copy(), pb.rotation_quaternion.copy())
    for o in _n: bpy.data.objects.remove(o, do_unlink=True)
    bpy.data.actions.remove(_a, do_unlink=True)
def apply_fist():
    for i, (l, q) in FIST_BASIS.items():
        pb = arm.pose.bones[bones[i].name]; pb.rotation_mode = 'QUATERNION'; pb.location = l; pb.rotation_quaternion = q

sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'; sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.render.resolution_x = 600; sc.render.resolution_y = 600
if VIEW.startswith('hand'): cam_scale = 0.75
else: cam_scale = 2.3
cam = bpy.data.cameras.new('c'); cam.type = 'ORTHO'; cam.ortho_scale = cam_scale
co = bpy.data.objects.new('c', cam); sc.collection.objects.link(co); sc.camera = co

def finger_curves_remove(act):
    bags = [act] if hasattr(act, 'fcurves') else [cb for l in act.layers for st in l.strips for cb in st.channelbags]
    for bag in bags:
        for fc in list(bag.fcurves):
            if 'RightHand' in fc.data_path and any(k in fc.data_path for k in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky')):
                bag.fcurves.remove(fc)

ARM_ACT = None
if ARMFROM:
    _b = set(bpy.data.objects); bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', ARMFROM))
    _n = [o for o in bpy.data.objects if o not in _b]; ARM_ACT = [o for o in _n if o.type == 'ARMATURE'][0].animation_data.action
    for o in _n: bpy.data.objects.remove(o, do_unlink=True)
    ARM_ACT.use_fake_user = True
def arm_layer(phase):
    act = arm.animation_data.action
    a0, a1 = ARM_ACT.frame_range[0], ARM_ACT.frame_range[1]
    arm.animation_data.action = ARM_ACT; sc.frame_set(int(round(a0 + (a1 - a0) * phase))); bpy.context.view_layer.update()
    basis = {}
    for i in range(31, 55):
        pb = arm.pose.bones[bones[i].name]; pb.rotation_mode = 'QUATERNION'; basis[i] = (pb.location.copy(), pb.rotation_quaternion.copy())
    arm.animation_data.action = act
    return basis
clips = [l.strip() for l in open(LIST, encoding='utf-8') if l.strip()]
for ci, clip in enumerate(clips):
    path = os.path.join(ROOT, 'import assets', clip)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    narm = [o for o in new if o.type == 'ARMATURE'][0]
    act = narm.animation_data.action
    arm.animation_data_create(); arm.animation_data.action = act
    arm.data.pose_position = 'POSE'
    if USE_FIST: finger_curves_remove(act)
    if ARM_ACT:   # o braço direito vem da camada: tira as curvas dele do clipe base (senão o render reavalia e sobrescreve)
        arm_names = {bones[i].name for i in range(31, 55)}
        for bag in ([act] if hasattr(act, 'fcurves') else [cb for l in act.layers for st in l.strips for cb in st.channelbags]):
            for fc in list(bag.fcurves):
                if any('"%s"' % n in fc.data_path for n in arm_names): bag.fcurves.remove(fc)
    f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])
    if RANGE: f0, f1 = RANGE
    frames = [int(round(f0 + (f1 - f0) * k / max(1, NF - 1))) for k in range(NF)]
    tiles = []
    for f in frames:
        layer = arm_layer((f - f0) / max(1, f1 - f0)) if ARM_ACT else None
        sc.frame_set(f); bpy.context.view_layer.update()
        if layer:
            for i, (l, q) in layer.items():
                pb = arm.pose.bones[bones[i].name]; pb.rotation_mode = 'QUATERNION'; pb.location = l; pb.rotation_quaternion = q
        if USE_FIST: apply_fist()
        pbh = arm.pose.bones[hand_name]
        if TWIST or BEND:
            pbh.rotation_mode = 'QUATERNION'; pbh.rotation_quaternion = pbh.rotation_quaternion @ Quaternion((0, 1, 0), math.radians(TWIST)) @ Quaternion((1, 0, 0), math.radians(BEND))
        bpy.context.view_layer.update()
        pose_world = M_arm @ pbh.matrix
        S = pose_world @ bind_inv
        objs = []
        for col, vs, tris in subs:
            me = bpy.data.meshes.new('t'); me.from_pydata([tuple(S @ v) for v in vs], [], tris)
            ob = bpy.data.objects.new('tool', me); sc.collection.objects.link(ob)
            mat = bpy.data.materials.new('m'); mat.diffuse_color = (*col, 1); ob.data.materials.append(mat); objs.append(ob)
        hip = (M_arm @ arm.pose.bones[bones[0].name].matrix).translation
        tgt = (hip.x, hip.y, 1.0)
        if VIEW.startswith('hand'): tgt = tuple((M_arm @ pbh.matrix).translation)
        dirs = {'side': (-1, 0, 0), 'front': (0, -1, 0), 'back': (0, 1, 0), 'q': (-0.7, -0.7, 0.25), 'qb': (-0.6, 0.75, 0.3), 'hand': (-0.75, -0.55, 0.35), 'handside': (-1, 0, 0.1), 'handfront': (-0.2, -1, 0.1), 'left': (1, 0, 0)}
        d = Vector(dirs[VIEW]).normalized()
        co.location = Vector(tgt) + d * 4
        co.rotation_mode = 'QUATERNION'; co.rotation_quaternion = (-d).to_track_quat('-Z', 'Y')
        sc.render.filepath = os.path.join(OUTDIR, 'c%02d_%02d_f%d.png' % (ci, len(tiles), f)); bpy.ops.render.render(write_still=True)
        tiles.append(f)
        for ob in objs: bpy.data.objects.remove(ob, do_unlink=True)
    print("CLIP", ci, clip, f0, f1)
    for o in new: bpy.data.objects.remove(o, do_unlink=True)
    bpy.data.actions.remove(act, do_unlink=True)
