#!/usr/bin/env python3
"""Converte os WAV PCM 16 bit mono de assets/sfx em IMA ADPCM (WAVE_FORMAT 0x11, 4 bits/amostra = 4x menor).

Uso: python3 tools/compress_wav.py [arquivos .wav...]   (sem argumentos = assets/sfx/*.wav)
  - O aapt2 guarda .wav SEM compressão no APK; com ADPCM o APK perde ~3/4 do tamanho dos sons.
  - Audio::loadWav (engine/src/audio.cpp) decodifica ADPCM para PCM 16 ao carregar (mesma RAM de antes, mesmo som).
  - Originais vão para build/wav_pcm/. Rodar de novo depois de tools/export_audio.py (que grava PCM).
Blocos de 512 bytes (1017 amostras); chunk 'fact' com o total de amostras (o decodificador corta o enchimento do último bloco).
"""
import glob
import os
import shutil
import struct
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
        1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]
BLOCK = 512
SPB = (BLOCK - 4) * 2 + 1


def encode(samples):
    out = bytearray()
    n = len(samples)
    pos = 0
    idx = 0
    while pos < n:
        blk = samples[pos:pos + SPB]
        blk = blk + [0] * (SPB - len(blk))
        pred = blk[0]
        out += struct.pack("<hBB", pred, idx, 0)
        nib = []
        for s in blk[1:]:
            step = STEP[idx]
            diff = s - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            # quantização igual ao decodificador (diferença reconstruída)
            vd = step >> 3
            if diff >= step:
                code |= 4; diff -= step; vd += step
            step >>= 1
            if diff >= step:
                code |= 2; diff -= step; vd += step
            step >>= 1
            if diff >= step:
                code |= 1; vd += step
            pred = pred - vd if code & 8 else pred + vd
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IDX[code & 7]))
            nib.append(code)
        for i in range(0, len(nib), 2):
            out.append(nib[i] | (nib[i + 1] << 4))
        pos += SPB
    return bytes(out)


def convert(path):
    with open(path, "rb") as f:
        head = f.read(64)
    if b"fmt " in head and struct.unpack("<H", head[head.index(b"fmt ") + 8:head.index(b"fmt ") + 10])[0] == 0x11:
        print(f"{path}: já é ADPCM")
        return
    w = wave.open(path)
    assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
    rate, n = w.getframerate(), w.getnframes()
    samples = list(struct.unpack("<%dh" % n, w.readframes(n)))
    w.close()
    data = encode(samples)
    fmt = struct.pack("<HHIIHHHH", 0x11, 1, rate, (rate * BLOCK) // SPB, BLOCK, 4, 2, SPB)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"fact" + struct.pack("<II", 4, n) + b"data" + struct.pack("<I", len(data)) + data
    if len(data) & 1:
        body += b"\0"
    bak = os.path.join(ROOT, "build", "wav_pcm")
    os.makedirs(bak, exist_ok=True)
    shutil.copy2(path, os.path.join(bak, os.path.basename(path)))
    old = os.path.getsize(path)
    with open(path + ".tmp", "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    os.replace(path + ".tmp", path)
    print(f"{os.path.basename(path)}: {old // 1024} KB -> {os.path.getsize(path) // 1024} KB")


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "assets", "sfx", "*.wav")))
    for f in files:
        convert(f)


if __name__ == "__main__":
    main()
