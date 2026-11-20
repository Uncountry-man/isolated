#!/usr/bin/env python3
"""Comprime os texture arrays PBR do terreno (.e0t RGBA8) para ASTC com mipmaps pré-gerados.

Por quê: a Adreno 710 fica limitada por banda de textura com RGBA8 (o terreno caía para ~20 FPS).
ASTC lê 4-9x menos memória por texel e é obrigatório no GLES 3.2.

Uso:  python3 tools/compress_pbr.py
  - Entrada: "import assets/pbr_raw/terrain_{albedo,normal}.e0t" (RGBA8, format 4, gerados por tools/pack_pbr.py).
    Se ainda não existirem, copia os de assets/ para lá (backup do original sem perdas).
  - Saída: assets/terrain_{albedo,normal}.e0t no formato ASTC.
  - Requer astcenc (https://github.com/ARM-software/astc-encoder); procura em ~/.local/astc/bin ou no PATH.

Formato E0T1 comprimido (format = 100 + bloco, ex.: 106 = ASTC 6x6, 105 = ASTC 5x5):
  header <4sHHHHI> magic, width, height, layers, format, dataSize
  para cada nível de mip (até 1x1): u32 tamanho, dados ASTC de TODAS as camadas (sem o cabeçalho .astc de 16 B)
"""
import os, shutil, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, 'import assets', 'pbr_raw')
OUT = os.path.join(ROOT, 'assets')
JOBS = [('terrain_albedo.e0t', 6), ('terrain_normal.e0t', 5)]


def find_astcenc():
    for name in ('astcenc-avx2', 'astcenc-sse4.1', 'astcenc-sse2', 'astcenc'):
        p = os.path.expanduser(f'~/.local/astc/bin/{name}')
        if os.path.isfile(p):
            return p
        p = shutil.which(name)
        if p:
            return p
    sys.exit('astcenc não encontrado (baixe em github.com/ARM-software/astc-encoder para ~/.local/astc/bin)')


def read_raw(path):
    d = open(path, 'rb').read()
    magic, w, h, layers, fmt, size = struct.unpack('<4sHHHHI', d[:16])
    assert magic == b'E0T1' and fmt == 4, f'{path}: esperado E0T1 RGBA8 (format 4), veio {fmt}'
    a = np.frombuffer(d[16:16 + size], dtype=np.uint8).reshape(layers, h, w, 4)
    return a


def mip_chain(img):
    """Box filter 2x2 em float (sem pré-multiplicar o alfa, que aqui é roughness/AO e não transparência)."""
    lv = [img]
    cur = img.astype(np.float32)
    while cur.shape[0] > 1 or cur.shape[1] > 1:
        h, w = cur.shape[:2]
        nh, nw = max(h // 2, 1), max(w // 2, 1)
        cur = cur[:nh * 2 if h > 1 else 1, :nw * 2 if w > 1 else 1]
        if h > 1: cur = 0.5 * (cur[0::2] + cur[1::2])
        if w > 1: cur = 0.5 * (cur[:, 0::2] + cur[:, 1::2])
        lv.append(np.clip(cur + 0.5, 0, 255).astype(np.uint8))
    return lv


def main():
    enc = find_astcenc()
    os.makedirs(RAW, exist_ok=True)
    for name, blk in JOBS:
        raw = os.path.join(RAW, name)
        if not os.path.exists(raw):
            src = os.path.join(OUT, name)
            hdr = open(src, 'rb').read(16)
            if struct.unpack('<4sHHHHI', hdr)[4] != 4:
                sys.exit(f'{src} já está comprimido e não há original em {raw}; rode tools/pack_pbr.py antes')
            shutil.copy2(src, raw)
            print(f'backup do original: {raw}')
        arr = read_raw(raw)
        layers, h, w = arr.shape[:3]
        chains = [mip_chain(arr[i]) for i in range(layers)]
        nlev = len(chains[0])
        body = b''
        with tempfile.TemporaryDirectory() as td:
            for lvl in range(nlev):
                lvdata = b''
                for li in range(layers):
                    png, astc = os.path.join(td, 'in.png'), os.path.join(td, 'out.astc')
                    Image.fromarray(chains[li][lvl], 'RGBA').save(png)
                    subprocess.run([enc, '-cl', png, astc, f'{blk}x{blk}', '-medium', '-silent'], check=True)
                    lvdata += open(astc, 'rb').read()[16:]
                body += struct.pack('<I', len(lvdata)) + lvdata
        out = os.path.join(OUT, name)
        with open(out, 'wb') as f:
            f.write(struct.pack('<4sHHHHI', b'E0T1', w, h, layers, 100 + blk, len(body)))
            f.write(body)
        print(f'{out}: ASTC {blk}x{blk}, {nlev} mips, {os.path.getsize(out) / 1e6:.2f} MB (era {os.path.getsize(raw) / 1e6:.2f} MB)')


if __name__ == '__main__':
    main()
