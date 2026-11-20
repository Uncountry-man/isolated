# Analisa clipes: velocidade das mãos, altura, distância entre as mãos (2 mãos?) -> escolher golpes de UMA mão.
# Uso: blender -b -P /abs/tools/analyze_clips.py -- LISTA.txt
import bpy, os, sys, math
from mathutils import Vector
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lst = sys.argv[sys.argv.index('--') + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(ROOT, 'import assets', 'Y Bot.fbx'))
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
M = arm.matrix_world
names = [b.name for b in arm.data.bones]
RH = [n for n in names if n.endswith('RightHand')][0]; LH = [n for n in names if n.endswith('LeftHand')][0]
HEAD = [n for n in names if n.endswith(':Head')][0]; HIPS = names[0]
arm.data.pose_position = 'POSE'
for clip in [l.strip() for l in open(lst, encoding='utf-8') if l.strip()]:
    path = os.path.join(ROOT, 'import assets', clip)
    if not os.path.exists(path): print("FALTA", clip); continue
    before = set(bpy.data.objects)
    try: bpy.ops.import_scene.fbx(filepath=path)
    except Exception as e: print("ERRO", clip, e); continue
    new = [o for o in bpy.data.objects if o not in before]
    narm = [o for o in new if o.type == 'ARMATURE']
    if not narm or not narm[0].animation_data: print("SEM ANIM", clip)
    else:
        act = narm[0].animation_data.action; arm.animation_data_create(); arm.animation_data.action = act
        f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])
        prevR = prevL = None; vR = []; vL = []; zR = []; dist = []; hips0 = None
        for f in range(f0, f1 + 1, 2):
            bpy.context.scene.frame_set(f); bpy.context.view_layer.update()
            r = (M @ arm.pose.bones[RH].matrix).translation.copy(); l = (M @ arm.pose.bones[LH].matrix).translation.copy()
            h = (M @ arm.pose.bones[HIPS].matrix).translation.copy()
            if hips0 is None: hips0 = h.copy()
            if prevR is not None: vR.append((r - prevR).length / (2 / 30.0)); vL.append((l - prevL).length / (2 / 30.0))
            prevR, prevL = r, l; zR.append(r.z); dist.append((r - l).length)
        n = len(vR) or 1
        # "ativo" = quadros em que a mão direita se move rápido
        act_idx = [i for i, v in enumerate(vR) if v > 1.5] or [0]
        twoh = sum(1 for i in act_idx if i < len(dist) and dist[i] < 0.30) / len(act_idx)
        print("CLIPE %-45s q=%3d  Rvmax=%4.1f Lvmax=%4.1f  Rz=[%.2f..%.2f] duasmaos=%.0f%%" % (clip, f1 - f0 + 1, max(vR or [0]), max(vL or [0]), min(zR), max(zR), twoh * 100))
        bpy.data.actions.remove(act, do_unlink=True)
    for o in new:
        try: bpy.data.objects.remove(o, do_unlink=True)
        except Exception: pass
