#!/usr/bin/env python3
"""Converte texturas OPACAS E0I1 (RGBA8 cru) em E0I2 (ETC1 = subconjunto do ETC2 RGB8, 4 bits/pixel, com mipmaps).

Uso: python3 tools/compress_textures.py [arquivos .e0i...]   (sem argumentos = a lista padrão abaixo)
  - 8x menos memória de GPU que RGBA8 e ~3x menos no APK; GLES 3.0 garante ETC2 (o ETC1 é decodificado por ele).
  - Só para imagens SEM transparência (o alfa é descartado; o script recusa se houver alfa < 255).
  - Usa o etc1tool do Android SDK (platform-tools). O original vai para build/e0i_rgba/ (para reconverter).
  - Rodar DE NOVO depois de regenerar as texturas (export_glb_rig.py, make_loading.py, make_scene.py gravam E0I1).

Formato E0I2: 'E0I2' + w u16 + h u16 + níveis u8 + 3 bytes 0, e para cada nível: tamanho u32 + blocos ETC1 (8 B por bloco 4x4).
Os loaders (engine/src/e0i.h) aceitam E0I1 e E0I2.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ["pirate_m", "pirate_m_lod1", "pirate_m_lod2", "pirate_f", "pirate_f_lod1", "pirate_f_lod2",
           "pirate_g", "pirate_g_lod1", "pirate_g_lod2", "pirate_c", "pirate_c_lod1", "pirate_c_lod2", "loading", "scene_deck"]
ETC1TOOL = os.path.expanduser(os.environ.get("ETC1TOOL", "~/Android/Sdk/platform-tools/etc1tool"))


def encode_level(img, tmp):
    png, pkm = os.path.join(tmp, "l.png"), os.path.join(tmp, "l.pkm")
    img.save(png)
    subprocess.run([ETC1TOOL, png, "--encode", "-o", pkm], check=True, stdout=subprocess.DEVNULL)
    data = open(pkm, "rb").read()
    assert data[:4] == b"PKM ", "etc1tool: saída inesperada"
    w, h = img.size
    n = ((w + 3) // 4) * ((h + 3) // 4) * 8
    blocks = data[16:16 + n]
    assert len(blocks) == n
    return blocks


def convert(path):
    raw = open(path, "rb").read()
    if raw[:4] == b"E0I2":
        print(f"{path}: já é E0I2")
        return
    assert raw[:4] == b"E0I1", path
    w, h = struct.unpack("<HH", raw[4:8])
    px = np.frombuffer(raw[8:8 + w * h * 4], np.uint8).reshape(h, w, 4)
    if px[..., 3].min() < 255:
        print(f"{path}: tem transparência, ficou RGBA8")
        return
    bak = os.path.join(ROOT, "build", "e0i_rgba")
    os.makedirs(bak, exist_ok=True)
    shutil.copy2(path, os.path.join(bak, os.path.basename(path)))
    img = Image.fromarray(px[..., :3].copy(), "RGB")
    levels = []
    with tempfile.TemporaryDirectory() as tmp:
        lw, lh = w, h
        while True:
            lvl = img if (lw, lh) == (w, h) else img.resize((lw, lh), Image.LANCZOS)
            levels.append(encode_level(lvl, tmp))
            if lw == 1 and lh == 1:
                break
            lw, lh = max(1, lw // 2), max(1, lh // 2)
    out = bytearray(b"E0I2" + struct.pack("<HHB3x", w, h, len(levels)))
    for b in levels:
        out += struct.pack("<I", len(b)) + b
    tmpf = path + ".tmp"
    open(tmpf, "wb").write(out)
    os.replace(tmpf, path)
    print(f"{path}: {w}x{h} RGBA8 {len(raw) // 1024} KB -> ETC1 + {len(levels)} mips {len(out) // 1024} KB")


def main():
    files = sys.argv[1:] or [os.path.join(ROOT, "assets", n + ".e0i") for n in DEFAULT]
    for f in files:
        if os.path.exists(f):
            convert(f)


if __name__ == "__main__":
    main()
