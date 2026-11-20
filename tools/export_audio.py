#!/usr/bin/env python3
"""Converte os efeitos sonoros de ~/Downloads/sfx (mp3) em assets/sfx/*.wav (PCM 16 bit mono) para a engine (audio.cpp).
Decodifica com GStreamer (gst-launch-1.0) e processa com numpy:
  passos de grama/areia: recortados de gravações de caminhada (cada passo vira um arquivo), normalizados
  passo duro (terra/pedra/piso): 1 passo + variações de tom;  nado: 1 braçada;  ambientes: trechos em loop sem emenda (crossfade).
Uso: python3 tools/export_audio.py [pasta_mp3]"""
import os, sys, subprocess, tempfile, wave
import numpy as np

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads/sfx")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "sfx")
os.makedirs(OUT, exist_ok=True)
TMP = tempfile.mkdtemp()

def decode(name, rate, folder=None):
    wav = os.path.join(TMP, name + ".wav")
    subprocess.run(["gst-launch-1.0", "-q", "filesrc", f"location={os.path.join(folder or SRC, name)}.mp3", "!", "decodebin", "!", "audioconvert", "!", "audioresample", "!",
                    f"audio/x-raw,format=S16LE,rate={rate},channels=1", "!", "wavenc", "!", "filesink", f"location={wav}"], check=True)
    w = wave.open(wav)
    return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0

def save(name, data, rate):
    data = np.clip(data, -1, 1)
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes((data * 32767).astype(np.int16).tobytes())

def onsets(d, sr, rel=0.25, mind=0.25):
    win = int(sr * 0.03)
    env = np.sqrt(np.convolve(d * d, np.ones(int(sr * 0.01)) / int(sr * 0.01), "same"))
    env = np.convolve(env, np.ones(win) / win, "same")
    thr = env.max() * rel; pk = []; last = -1e9
    for i in range(1, len(env) - 1):
        if env[i] > thr and env[i] >= env[i - 1] and env[i] >= env[i + 1] and i - last > sr * mind:
            pk.append(i); last = i
    return pk

def cut_steps(d, sr, target_peak, maxn):
    clips = []
    for p in onsets(d, sr):
        a = max(0, p - int(sr * 0.07)); b = min(len(d), p + int(sr * 0.27))
        c = d[a:b].copy()
        if len(c) < sr * 0.2: continue
        n = int(sr * 0.015); c[:n] *= np.linspace(0, 1, n)                 # entrada suave
        n = int(sr * 0.08); c[-n:] *= np.linspace(1, 0, n)                 # saída suave
        clips.append(c)
    # fica com os mais fortes/variados (até maxn), normaliza o conjunto pelo mais forte
    clips.sort(key=lambda c: -np.abs(c).max())
    clips = clips[:maxn]
    pk = max(np.abs(c).max() for c in clips)
    return [c * (target_peak / pk) for c in clips]

def loop_seg(d, sr, start_s, dur_s, xf_s):
    seg = d[int(start_s * sr): int((start_s + dur_s + xf_s) * sr)].copy()
    n = int(xf_s * sr)
    fade = np.linspace(0, 1, n)
    head = seg[:n] * fade + seg[-n:] * (1 - fade)       # a cauda funde de volta com o começo
    out = np.concatenate([head, seg[n:-n]])
    return out

def rms_norm(d, target):
    r = np.sqrt(np.mean(d * d)) + 1e-9
    return d * (target / r)

# --- passos ---
g = decode("spinopel-walking-on-grass-381887", 32000)
for i, c in enumerate(cut_steps(g, 32000, 0.85, 8)): save(f"step_grass_{i}", c, 32000)
s = decode("freesound_community-sand-walk-106366", 32000)
for i, c in enumerate(cut_steps(s, 32000, 0.80, 8)): save(f"step_sand_{i}", c, 32000)
h = decode("yarzur_ofc-st3-footstep-sfx-323056", 32000)
h = h / (np.abs(h).max() + 1e-9) * 0.85
save("step_hard_0", h, 32000)
for i, f in enumerate((0.90, 1.12)):                      # variações de tom (reamostragem)
    idx = np.arange(0, len(h) - 1, f)
    v = np.interp(idx, np.arange(len(h)), h)
    save(f"step_hard_{i + 1}", v, 32000)
# --- nado ---
sw = decode("freesound_community-swim-44183", 32000)
save("swim", sw / (np.abs(sw).max() + 1e-9) * 0.9, 32000)
# --- ambientes (loops) ---
j = decode("emycutiepants-jungle-ambience-339096", 22050)
save("amb_jungle", np.clip(rms_norm(loop_seg(j, 22050, 30, 40, 3), 0.12), -1, 1), 22050)
o = decode("rmultimediaeu-ocean-waves-250310", 22050)
save("amb_ocean", np.clip(rms_norm(loop_seg(o, 22050, 8, 38, 3), 0.14), -1, 1), 22050)
# --- ferramentas: lenhar (machadinha) e minerar (picareta); os mp3 ficam em ~/Downloads (pasta acima de sfx) ---
DL = os.path.dirname(SRC.rstrip("/"))
ck = decode("freesound_community-clock-tick-tik-tak-76043", 32000, DL)
sr = 32000
def hit(d, t, pre=0.03, post=0.42):
    a = max(0, int((t - pre) * sr)); c = d[a:a + int(post * sr)].copy()
    n = int(sr * 0.006); c[:n] *= np.linspace(0, 1, n)
    n = int(sr * 0.10); c[-n:] *= np.linspace(1, 0, n)
    return c
tik, tak = hit(ck, 0.08), hit(ck, 1.07)
def resample(x, f): return np.interp(np.arange(0, len(x) - 1, f), np.arange(len(x)), x)
chops = [tik, tak, resample(tik, 0.8), resample(tak, 0.8)]       # as cópias mais lentas soam mais "pesadas"
pk = max(np.abs(c).max() for c in chops)
for i, c in enumerate(chops): save(f"chop_{i}", c * (0.9 / pk), sr)
pa = decode("creatorshome-mine-stone-with-a-pickaxe-333694", 32000, DL)
ph = [hit(pa, t / sr, 0.05, 0.45) for t in onsets(pa, sr, 0.2, 0.3)]
ph = [h for h in ph if len(h) > sr * 0.3]
ph.sort(key=lambda c: -np.abs(c).max()); ph = ph[:8]
pk = max(np.abs(c).max() for c in ph)
for i, c in enumerate(ph): save(f"pick_{i}", c * (0.95 / pk), sr)
print("ok ->", OUT, sorted(os.listdir(OUT)))

# APK: os .wav viram IMA ADPCM (4x menores); ver tools/compress_wav.py
import subprocess, sys
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "compress_wav.py")], check=True)
