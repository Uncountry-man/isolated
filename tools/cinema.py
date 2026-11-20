#!/usr/bin/env python3
# Painel de linha de comando do MODO CINEMA (o app precisa ser o build de cinema: E0_CINEMA=1 ./build.sh).
#   tools/cinema.py gravar roteiros/navios.cine [--escala 0.5]   envia o roteiro, grava no aparelho, traz os vídeos para videos/<nome>/ e gera folha de contato
#   tools/cinema.py previa roteiros/navios.cine                  roda na tela do aparelho, sem gravar
#   tools/cinema.py parar | status
# --escala: rascunho rápido (reduz resolução e bitrate). Saída: <nome>.mp4 (completo) + <nome>_NN_<plano>.mp4 (um clipe por plano).
import argparse, os, re, subprocess, sys, tempfile, time

PKG = 'com.rungo.isolatedMT.cinema'
DIR = f'/sdcard/Android/data/{PKG}/files/cinema'


def adb(*a, check=True, cap=True, timeout=30):
    # sem limite, o adb fica em "waiting for device" para sempre se o Wi-Fi cair; pull de vídeo grande usa timeout maior
    try:
        return subprocess.run(['adb', *a], check=check, capture_output=cap, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        sys.exit(f'o adb nao respondeu em {timeout} s: aparelho desconectado? (adb devices; a depuracao Wi-Fi pode ter caido)')


def status():
    r = adb('shell', f'cat {DIR}/status.txt', check=False)
    p = r.stdout.split()
    return p if len(p) >= 3 else None


def send_cmd(line):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write(line + '\n')
    adb('push', f.name, f'{DIR}/cmd.tmp')
    adb('shell', f'mv {DIR}/cmd.tmp {DIR}/cmd.txt')
    os.unlink(f.name)


def ensure_running():
    adb('shell', f'am start -n {PKG}/android.app.NativeActivity')
    for _ in range(60):
        if status(): return
        time.sleep(0.5)
    sys.exit('o app de cinema nao respondeu (instale com E0_CINEMA=1 ./build.sh)')


def push_script(path, escala):
    text = open(path, encoding='utf8').read()
    if escala != 1.0:
        def fix(m):
            w, h, fps, mb = int(m[1]), int(m[2]), int(m[3]), float(m[4])
            return f'video {int(w * escala) // 2 * 2} {int(h * escala) // 2 * 2} {fps} {max(4.0, mb * escala):.0f}'
        text = re.sub(r'^video (\d+) (\d+) (\d+) ([\d.]+)', fix, text, flags=re.M)
    name = os.path.basename(path)
    with tempfile.NamedTemporaryFile('w', suffix='.cine', delete=False, encoding='utf8') as f:
        f.write(text)
    adb('push', f.name, f'{DIR}/{name}')
    os.unlink(f.name)
    return name, re.search(r'^nome (\S+)', text, re.M)[1] if re.search(r'^nome (\S+)', text, re.M) else 'video'


def contact_sheet(mp4, out, cols=4, rows=4):
    import cv2, numpy as np
    c = cv2.VideoCapture(mp4)
    n = int(c.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = c.get(cv2.CAP_PROP_FPS); w = int(c.get(cv2.CAP_PROP_FRAME_WIDTH)); h = int(c.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f'{os.path.basename(mp4)}: {w}x{h} {fps:.2f} fps, {n} quadros, {n / max(fps, 1):.1f} s, {os.path.getsize(mp4) / 1e6:.1f} MB')
    tw, th = 480, int(480 * h / max(w, 1))
    sheet = np.zeros((rows * th, cols * tw, 3), np.uint8)
    for i in range(cols * rows):
        c.set(cv2.CAP_PROP_POS_FRAMES, int((i + 0.5) * n / (cols * rows)))
        ok, f = c.read()
        if ok: sheet[(i // cols) * th:(i // cols + 1) * th, (i % cols) * tw:(i % cols + 1) * tw] = cv2.resize(f, (tw, th))
    cv2.imwrite(out, sheet)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('acao', choices=['gravar', 'previa', 'parar', 'status'])
    ap.add_argument('roteiro', nargs='?')
    ap.add_argument('--escala', type=float, default=1.0)
    a = ap.parse_args()
    if a.acao == 'status': print(status()); return
    if a.acao == 'parar': send_cmd('parar'); return
    ensure_running()
    name, video = push_script(a.roteiro, a.escala)
    adb('shell', f'rm -f {DIR}/out/* {DIR}/status.txt')   # um status velho ("pronto") faria a espera acabar antes da hora
    t0 = time.time()
    send_cmd(f"{a.acao} {name}")
    if a.acao == 'previa': return
    last, last_t = None, time.time()
    while True:
        time.sleep(1.5)
        s = status()
        if s and s[:2] != last: last, last_t = s[:2], time.time()
        if time.time() - last_t > 60: sys.exit(f'gravacao parada ha 60 s no estado {last}: veja adb logcat -s Isolated')
        if s and s[0] == 'erro': sys.exit('erro no roteiro: ' + ' '.join(s[3:]))
        if s and s[0] == 'pronto' and time.time() - t0 > 3: break
        if s and s[0] == 'gravando':
            print(f'\rquadro {s[1]}/{s[2]}  ({time.time() - t0:.0f} s)', end='', flush=True)
    print(f'\npronto em {time.time() - t0:.0f} s: {" ".join(s[3:])}')
    dest = os.path.join('videos', video)
    os.makedirs(dest, exist_ok=True)
    for f in os.listdir(dest): os.remove(os.path.join(dest, f))
    for f in adb('shell', f'ls {DIR}/out').stdout.split():
        adb('pull', f'{DIR}/out/{f}', dest, timeout=300)
    contact_sheet(os.path.join(dest, video + '.mp4'), os.path.join(dest, 'folha.jpg'))
    print('videos em', dest, sorted(os.listdir(dest)))


main()
