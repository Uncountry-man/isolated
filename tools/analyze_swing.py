# Para cada clipe: direção (no espaço local da mão) em que a CABEÇA da ferramenta viaja no pico de velocidade.
# Serve p/ escolher a empunhadura (gume alinhado com o movimento) e os golpes realistas de uma mão.
# Uso: blender -b -P /abs/tools/analyze_swing.py -- LISTA.txt TOOL
import bpy, os, sys, math, struct
from mathutils import Matrix, Vector, Quaternion
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lst, TOOL = sys.argv[sys.argv.index('--') + 1], sys.argv[sys.argv.index('--') + 2]
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
M = arm.matrix_world
names = [b.name for b in arm.data.bones]
RH = [n for n in names if n.endswith('RightHand')][0]
hb = arm.data.bones[RH]
bind = M @ hb.matrix_local
# ferramenta -> coordenadas locais da mão
b = open(os.path.join(ROOT, 'assets', 'tool_%s.e0m' % TOOL), 'rb').read()
nsub = struct.unpack_from('<I', b, 4)[0]; o = 8; subs = []
for _ in range(nsub):
    nv, ni = struct.unpack_from('<II', b, o + 12); o += 20
    vs = [bind.inverted() @ (T_B2E.inverted() @ Vector(struct.unpack_from('<3f', b, o + i * 32))) for i in range(nv)]
    o += nv * 32 + ni * 4; subs.append(vs)
# escala do osso: coordenadas locais do osso estão em cm (armature escala 0.01) -> converte p/ metros
sc = bind.to_scale()[0]
def cent(vs): return sum(vs, Vector()) / len(vs)
head_l = cent(subs[1]) * sc if len(subs) > 1 else cent(subs[0]) * sc     # centro da cabeça (metal), em metros, local da mão
wood = subs[0]
# eixo do cabo no local da mão: do ponto mais "baixo" ao mais "alto" (PCA simples)
import itertools
pts = [v * sc for v in wood]; c0 = cent(pts)
best = max(((p - c0).length, p) for p in pts)[1]; handle_dir = (best - c0).normalized()
if (head_l - c0).dot(handle_dir) < 0: handle_dir = -handle_dir
print("TOOL", TOOL, "cabeça (local mão, m)", [round(x, 3) for x in head_l], "cabo->cabeça", [round(x, 2) for x in handle_dir])
arm.data.pose_position = 'POSE'
for clip in [l.strip() for l in open(lst, encoding='utf-8') if l.strip()]:
    path = os.path.join(ROOT, 'import assets', clip)
    before = set(bpy.data.objects); bpy.ops.import_scene.fbx(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    narm = [o for o in new if o.type == 'ARMATURE'][0]; act = narm.animation_data.action
    arm.animation_data_create(); arm.animation_data.action = act
    f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])
    P = []; R = []
    for f in range(f0, f1 + 1):
        bpy.context.scene.frame_set(f); bpy.context.view_layer.update()
        pw = M @ arm.pose.bones[RH].matrix
        Rm = pw.to_3x3().normalized()
        P.append(pw.translation + Rm @ head_l); R.append(Rm)
    sp = [((P[i + 1] - P[i]).length * 30.0) for i in range(len(P) - 1)]
    i = max(range(len(sp)), key=lambda k: sp[k])
    v = (P[i + 1] - P[i]).normalized(); u = R[i].transposed() @ v
    hw = R[i] @ handle_dir
    # direção do movimento em "frente/cima" do personagem (frente = -Y Blender)
    print("CLIPE %-42s pico f%3d v=%4.1f m/s  u(local mão)=(%5.2f,%5.2f,%5.2f)  mundo: frente=%5.2f lado(+dir)=%5.2f cima=%5.2f  cabo.mundo cima=%5.2f" % (clip, f0 + i, sp[i], u.x, u.y, u.z, -v.y, -v.x, v.z, hw.z))
    bpy.data.actions.remove(act, do_unlink=True)
    for o in new:
        try: bpy.data.objects.remove(o, do_unlink=True)
        except Exception: pass
