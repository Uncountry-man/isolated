# RIG DOS PIRATAS NO BLENDER (esqueleto PRÓPRIO de cada um; o Y Bot só empresta os nomes/orientações dos ossos e as animações).
#   flatpak run --filesystem=host org.blender.Blender -b -P /abs/tools/rig_pirates.py -- /abs/RAIZ /abs/SAIDA [--poses] [--save] [--only m|f]
# Para cada pirata (m = homem, f = mulher):
#   1. lê o scan em pose A (build/tripo/out/<k>_lod<n>.npz, as 3 malhas com textura) sem deformar nada;
#   2. monta o armature de 65 ossos com as juntas de tools/pirate_skel.py (dentro do corpo do pirata);
#   3. pesos: Armature > Automatic Weights (difusão de calor) no LOD0 + correções (cabelo/bandana/rosto presos à cabeça, faixa da cintura ao quadril,
#      mão inteira com dedos), e os LOD1/LOD2 copiam os pesos do LOD0 pela superfície mais próxima (mesmo comportamento nos 3);
#   4. --poses: aplica quadros de clipes reais do ybot.e0a com a MESMA conta da engine e renderiza folhas (frente/lado) em SAIDA, além de medir o
#      quanto cada triângulo estica/encolhe (deformação) por osso;
#   5. grava SAIDA/<k>_rig.npz (pesos por LOD) e, com --save, SAIDA/pirate_<k>.blend para abrir no Blender.
import bpy, sys, os, math
import numpy as np
from mathutils import Matrix, Vector, bvhtree

args = sys.argv[sys.argv.index('--') + 1:]
ROOT, OUT = args[0], args[1]
POSES = '--poses' in args; SAVE = '--save' in args
ONLY = args[args.index('--only') + 1] if '--only' in args else 'mf'
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pirate_skel as ps
os.makedirs(OUT, exist_ok=True)

CM = np.array([[-1.0, 0, 0], [0, 0, 1.0], [0, 1.0, 0]])     # engine (Y cima, frente -Z) -> Blender (Z cima, frente -Y... espelhado em X): det = +1
C4 = np.eye(4); C4[:3, :3] = CM


def to_bl(M): return Matrix((C4 @ M).tolist())


def weld(P, I):
    key = np.round(P * 20000).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True); inv = inv.reshape(-1)
    F = inv[I]; ok = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    return first, inv, F[ok], ok


def make_mesh(name, P, UV, I, png):
    first, inv, F, ok = weld(P, I)
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(CM @ P[f]) for f in first], [], [tuple(map(int, f)) for f in F]); me.update()
    uvl = me.uv_layers.new(name='uv'); Iok = I[ok]
    for pi, poly in enumerate(me.polygons):
        for k, li in enumerate(poly.loop_indices): uvl.data[li].uv = (float(UV[Iok[pi][k]][0]), 1.0 - float(UV[Iok[pi][k]][1]))
    for p in me.polygons: p.use_smooth = True
    ob = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(ob)
    mat = bpy.data.materials.new(name); mat.use_nodes = True
    node = mat.node_tree.nodes.new('ShaderNodeTexImage'); node.image = bpy.data.images.load(png)
    mat.node_tree.links.new(node.outputs['Color'], mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
    me.materials.append(mat)
    return ob, inv


def make_armature(key, sk):
    ad = bpy.data.armatures.new('rig_' + key); ao = bpy.data.objects.new('rig_' + key, ad)
    bpy.context.scene.collection.objects.link(ao)
    bpy.context.view_layer.objects.active = ao; bpy.ops.object.mode_set(mode='EDIT')
    J, B, par = sk['J'], sk['B'], sk['par']
    kids = {}
    for i in range(65):
        if par[i] >= 0: kids.setdefault(par[i], []).append(i)
    eb = []
    for i in range(65):
        b = ad.edit_bones.new(ps.NAMES[i])
        b.head = (0, 0, 0); b.tail = (0, 0.1, 0)
        b.matrix = to_bl(B[i])
        if i in kids:
            L = max(0.01, float(np.mean([np.linalg.norm(J[c] - J[i]) for c in kids[i]])))
        else:
            L = max(0.01, 0.5 * float(np.linalg.norm(J[i] - J[par[i]])))
        b.length = L
        eb.append(b)
    for i in range(65):
        if par[i] >= 0: eb[i].parent = eb[par[i]]
    bpy.ops.object.mode_set(mode='OBJECT')
    return ao


def heat(ob, ao):
    for o in bpy.context.scene.objects: o.select_set(False)
    ob.select_set(True); ao.select_set(True); bpy.context.view_layer.objects.active = ao
    bpy.ops.object.parent_set(type='ARMATURE_AUTO')
    W = np.zeros((len(ob.data.vertices), 65)); idx = {n: i for i, n in enumerate(ps.NAMES)}
    for v in ob.data.vertices:
        for g in v.groups:
            n = ob.vertex_groups[g.group].name
            if n in idx: W[v.index, idx[n]] = g.weight
    return W


def set_groups(ob, W):
    ob.vertex_groups.clear()
    gs = [ob.vertex_groups.new(name=n) for n in ps.NAMES]
    for b in range(65):
        nz = np.nonzero(W[:, b])[0]
        for v in nz: gs[b].add([int(v)], float(W[v, b]), 'REPLACE')


def top4(W):
    top = np.argsort(-W, axis=1)[:, :4]; w4 = np.take_along_axis(W, top, 1); w4 = w4 / np.maximum(w4.sum(1, keepdims=True), 1e-9)
    q = np.floor(w4 * 255 + 0.5).astype(np.int64); q[:, 0] += 255 - q.sum(1); top[q == 0] = 0
    return top.astype(np.uint8), q.astype(np.uint8)


def neighbors(F, n):
    E = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]]); E = np.concatenate([E, E[:, ::-1]])
    E = np.unique(E, axis=0)
    return E


def smooth(W, E, mask, it):
    """média dos vizinhos só nos vértices de `mask` (os de fora seguram a borda)."""
    n = len(W); deg = np.bincount(E[:, 0], minlength=n).astype(np.float64)
    for _ in range(it):
        S = np.zeros_like(W); np.add.at(S, E[:, 0], W[E[:, 1]])
        Wn = S / np.maximum(deg, 1)[:, None]
        W = np.where(mask[:, None], Wn, W)
    return W


def ring(E, mask, k):
    m = mask.copy()
    for _ in range(k):
        m2 = m.copy(); m2[E[m[E[:, 0]], 1]] = True; m = m2
    return m


def fix_weights(key, V, W, sk, col, F):
    """V: vértices (engine) do LOD0 soldado; col: cor da textura no vértice. Correções sobre o calor do Blender."""
    J = sk['J']; E = neighbors(F, len(V))
    W = np.where(W < 0.01, 0, W)
    print('  sem peso nenhum depois do calor: %d vertices' % (W.sum(1) < 1e-6).sum())
    W = W / np.maximum(W.sum(1, keepdims=True), 1e-9)
    hw = np.zeros(65); hw[5] = 1
    # 1) cabeça rígida: do queixo para cima tudo no osso da cabeça (cabelo, bandana, orelhas, barba); degradê suave no pescoço
    yH = J[5, 1]
    t = np.clip((V[:, 1] - (yH - 0.045)) / 0.06, 0, 1); t = t * t * (3 - 2 * t)
    W = W * (1 - t[:, None]) + hw[None, :] * t[:, None]
    # 2) CABELO longo (mulher): escuro, atrás do pescoço/costas ou sobre os ombros. Cabeça em cima -> peito (Spine2) embaixo, nunca braço/ombro
    if key == 'f':
        dark = col.max(1) < 0.55
        hair = dark & (V[:, 1] > J[2, 1]) & (V[:, 1] < yH + 0.05) & (np.abs(V[:, 0]) < 0.24) & \
            ((V[:, 2] > J[4, 2] - 0.01) | (V[:, 1] > J[31, 1] - 0.02))
        hair = ring(E, hair, 1) & (V[:, 1] > J[2, 1]) & (np.abs(V[:, 0]) < 0.25)
        # cada fio segue o ponto do corpo logo abaixo dele (costas/ombro), sem os ossos do braço: o cabelo e a camisa embaixo andam juntos
        from mathutils import kdtree
        arm = np.zeros(65, bool); arm[8:31] = True; arm[32:55] = True
        body = ~hair & (V[:, 1] > J[2, 1] - 0.05) & (V[:, 1] < yH) & (W[:, arm].sum(1) < 0.3)
        bi = np.nonzero(body)[0]; kd = kdtree.KDTree(len(bi))
        for k_, g_ in enumerate(bi): kd.insert(Vector(V[g_]), k_)
        kd.balance()
        th = np.clip((V[:, 1] - J[4, 1]) / max(1e-6, yH + 0.02 - J[4, 1]), 0, 1)
        for i in np.nonzero(hair)[0]:
            w = W[bi[kd.find(Vector(V[i]))[1]]].copy(); w[arm] = 0; w /= max(w.sum(), 1e-9)
            W[i] = (1 - th[i]) * w + th[i] * hw
        W = smooth(W, E, hair & (th < 0.999), 3)
        print('  cabelo: %d vertices presos as costas/cabeca' % hair.sum())
    # 3) FAIXA vermelha da cintura (ponta pendurada do lado esquerdo): presa ao quadril, só a ponta acompanha um pouco a coxa
    r, g, b = col[:, 0], col[:, 1], col[:, 2]
    red = (r > 0.18) & (r > 1.9 * g) & (r > 1.6 * b)
    sash = red & (V[:, 1] < J[0, 1] + 0.06) & (V[:, 1] > 0.3) & (np.abs(V[:, 0]) < 0.30)
    k = np.clip((J[0, 1] + 0.02 - V[:, 1]) / 0.40, 0, 1)
    Ws = np.zeros_like(W); Ws[:, 0] = 1 - 0.6 * k
    Ws[V[:, 0] < 0, 55] = 0.6 * k[V[:, 0] < 0]; Ws[V[:, 0] >= 0, 60] = 0.6 * k[V[:, 0] >= 0]
    W[sash] = Ws[sash]
    W = smooth(W, E, ring(E, sash, 2) & ~sash, 4)
    print('  faixa: %d vertices' % sash.sum())
    # 4) suavização geral leve (tira degraus do calor) e órfãos
    W = smooth(W, E, np.ones(len(V), bool) & (t < 0.999), 2)
    W = np.where(W < 0.02, 0, W)
    W = W / np.maximum(W.sum(1, keepdims=True), 1e-9)
    bad = W.sum(1) < 0.5
    if bad.any():
        from mathutils import kdtree
        good = np.nonzero(~bad)[0]; kd = kdtree.KDTree(len(good))
        for k_, g_ in enumerate(good): kd.insert(Vector(V[g_]), k_)
        kd.balance()
        for i in np.nonzero(bad)[0]: W[i] = W[good[kd.find(Vector(V[i]))[1]]]
    return W / np.maximum(W.sum(1, keepdims=True), 1e-9)


def transfer(Vsrc, Fsrc, Wsrc, Vdst):
    """pesos do LOD0 -> outro LOD: ponto mais perto na superfície do LOD0, pesos interpolados pelo triângulo."""
    tree = bvhtree.BVHTree.FromPolygons([Vector(v) for v in Vsrc], [tuple(map(int, f)) for f in Fsrc])
    out = np.zeros((len(Vdst), 65))
    for i, v in enumerate(Vdst):
        loc, nor, fi, d = tree.find_nearest(Vector(v))
        a, b, c = (Vsrc[k] for k in Fsrc[fi]); p = np.array(loc)
        v0, v1, v2 = b - a, c - a, p - a
        d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
        den = d00 * d11 - d01 * d01
        if abs(den) < 1e-14: l = np.array([1.0, 0, 0])
        else:
            l1 = (d11 * d20 - d01 * d21) / den; l2 = (d00 * d21 - d01 * d20) / den; l = np.clip([1 - l1 - l2, l1, l2], 0, 1); l /= l.sum()
        out[i] = l[0] * Wsrc[Fsrc[fi][0]] + l[1] * Wsrc[Fsrc[fi][1]] + l[2] * Wsrc[Fsrc[fi][2]]
    return out


def tex_colors(UV, png):
    img = bpy.data.images.load(png); w, h = img.size
    px = np.array(img.pixels[:], np.float32).reshape(h, w, 4)
    u = np.clip((UV[:, 0] * w).astype(int), 0, w - 1); v = np.clip(((1 - UV[:, 1]) * h).astype(int), 0, h - 1)
    return px[v, u, :3]


# ---------- poses de teste ----------
TEST = [('IDLE', 0), ('WALK', 8), ('RUN', 6), ('JUMP', 4), ('BATER', 14), ('BATER2', 16), ('CORTAR', 28), ('MINERAR', 26), ('COLETAR', 40),
        ('TMINE', 18), ('MORRER', 93), ('RESGATE', 60), ('LEVANTAR', 40), ('NADAR', 10), ('GAIM', 0), ('GWALK', 10), ('OMBRO', 0), ('LAND', 6)]


def apply_pose(ao, sk, G):
    """matrix_basis de cada osso a partir dos quadros globais da engine (a conjugação engine->Blender se cancela nas contas relativas)."""
    B, par = sk['B'], sk['par']
    for i in range(65):
        rest = B[i] if par[i] < 0 else np.linalg.inv(B[par[i]]) @ B[i]
        cur = G[i] if par[i] < 0 else np.linalg.inv(G[par[i]]) @ G[i]
        ao.pose.bones[ps.NAMES[i]].matrix_basis = Matrix((np.linalg.inv(rest) @ cur).tolist())


def deformed(ob):
    dg = bpy.context.evaluated_depsgraph_get(); ev = ob.evaluated_get(dg); me = ev.to_mesh()
    P = np.array([v.co[:] for v in me.vertices]) @ CM            # Blender -> engine (CM é simétrica e ortogonal)
    ev.to_mesh_clear(); return P


def setup_render():
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sh = sc.display.shading; sh.light = 'STUDIO'; sh.color_type = 'TEXTURE'
    sc.render.resolution_x = 420; sc.render.resolution_y = 620; sc.render.film_transparent = False
    sc.world = bpy.data.worlds.new('w') if sc.world is None else sc.world
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
    cam.data.type = 'ORTHO'; cam.data.ortho_scale = 2.6
    return cam


def render_view(cam, center, yaw_deg, path):
    # câmera olhando para o centro (Blender: Z cima). yaw 0 = de frente (personagem olha para -Y no Blender... frente da engine -Z -> Blender +Y? ver CM)
    a = math.radians(yaw_deg)
    fwd = CM @ np.array([0, 0, -1.0])                             # frente do personagem no Blender
    d = np.array([fwd[0] * math.cos(a) - fwd[1] * math.sin(a), fwd[0] * math.sin(a) + fwd[1] * math.cos(a), 0])
    pos = np.array(center) + d * 6.0
    cam.location = Vector(pos)
    cam.rotation_euler = (Vector(center) - Vector(pos)).to_track_quat('-Z', 'Y').to_euler()
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


bpy.ops.wm.read_factory_settings(use_empty=True)
for key in ONLY:
    P0, N0, UV0, I0 = ps.load_lod(key, 0)
    sk = ps.build(key, P0)
    ao = make_armature(key, sk)
    obs = []; res = {}
    png0 = os.path.join(ps.WORK, '%s_lod0.png' % key)
    ob0, inv0 = make_mesh('pirate_%s' % key, P0, UV0, I0, png0)
    first0, _, F0, _ = weld(P0, I0)
    V0 = P0[first0]
    Wh = heat(ob0, ao)
    col = tex_colors(UV0[first0], png0)
    W0 = fix_weights(key, V0, Wh, sk, col, F0)
    set_groups(ob0, W0)
    res['W0'] = W0[inv0]
    for lod in (1, 2):
        P, N, UV, I = ps.load_lod(key, lod)
        ob, inv = make_mesh('pirate_%s_lod%d' % (key, lod), P, UV, I, os.path.join(ps.WORK, '%s_lod%d.png' % (key, lod)))
        first, _, F, _ = weld(P, I)
        Wl = transfer(V0, F0, W0, P[first])
        Wl = Wl / np.maximum(Wl.sum(1, keepdims=True), 1e-9)
        set_groups(ob, Wl)
        md = ob.modifiers.new('Armature', 'ARMATURE'); md.object = ao; ob.parent = ao
        res['W%d' % lod] = Wl[inv]
        ob.location.x = 1.2 * lod                                  # lado a lado no .blend
        ob.hide_render = True
    for lod in range(3):
        B4, W8 = top4(res['W%d' % lod]); res['B%d' % lod] = B4; res['Q%d' % lod] = W8
    np.savez(os.path.join(OUT, '%s_rig.npz' % key), **{k: v for k, v in res.items() if k[0] in 'BQ'})
    print('%s: pesos gravados' % key)

    if POSES:
        ao.data.pose_position = 'POSE'
        cam = setup_render()
        clips = {c['name']: c for c in sk['clips']}
        # deformação: razão de comprimento das arestas (posada / repouso) com os pesos quantizados (top-4) como na engine
        E = np.unique(np.sort(np.concatenate([F0[:, [0, 1]], F0[:, [1, 2]], F0[:, [2, 0]]]), 1), axis=0)
        L0 = np.linalg.norm(V0[E[:, 0]] - V0[E[:, 1]], axis=1)
        report = []
        for nm, fr in TEST:
            c = clips[nm]; fr = min(fr, c['t'].shape[0] - 1)
            G = ps.pose_globals(sk, c['t'][fr], c['q'][fr])
            apply_pose(ao, sk, G); bpy.context.view_layer.update()
            Pd = deformed(ob0)
            r = np.linalg.norm(Pd[E[:, 0]] - Pd[E[:, 1]], axis=1) / np.maximum(L0, 1e-6)
            r[L0 < 0.006] = 1.0
            bad = (r > 1.6) | (r < 0.45)
            dom = np.argmax(W0, 1)
            bb = np.bincount(dom[E[bad, 0]], minlength=65)
            worst = ', '.join('%s:%d' % (ps.NAMES[b], bb[b]) for b in np.argsort(-bb)[:4] if bb[b] > 0)
            report.append('%-9s q%-3d esticadas/esmagadas: %4d (max %.2f, min %.2f)  %s' % (nm, fr, bad.sum(), r.max(), r.min(), worst))
            hip = Pd[:, :].mean(0); ctr = (CM @ np.array([0, 0.9, 0])) + CM @ np.array([hip[0], 0, hip[2]])
            for yaw, tag in ((0, 'f'), (90, 'l'), (200, 't')):
                render_view(cam, ctr, yaw, os.path.join(OUT, 'pose_%s_%s_%s.png' % (key, nm, tag)))
            # closes: ombros/pescoço e quadril/virilha (onde o skinning costuma quebrar)
            cam.data.ortho_scale = 0.9
            for part, bone in (('torso', 3), ('quadril', 0)):
                pc = CM @ G[bone][:3, 3]
                for yaw, tag in ((25, 'f'), (115, 'l')):
                    render_view(cam, pc, yaw, os.path.join(OUT, 'zoom_%s_%s_%s_%s.png' % (key, nm, part, tag)))
            cam.data.ortho_scale = 2.6
            if bad.any():
                ys = Pd[E[bad, 0], 1]; report[-1] += '  alturas: ' + ' '.join('%.2f' % y for y in np.percentile(ys, [10, 50, 90]))
        ao.data.pose_position = 'REST'
        open(os.path.join(OUT, '%s_deform.txt' % key), 'w').write('\n'.join(report) + '\n')
        print('\n'.join(report))
    if SAVE:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, 'pirate_%s.blend' % key))
    for o in list(bpy.data.objects): bpy.data.objects.remove(o)
