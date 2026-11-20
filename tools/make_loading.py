#!/usr/bin/env python3
"""Gera assets/loading.e0i (E0I1 = RGBA8, w/h u16) a partir da arte da tela de loading. Uso: python3 tools/make_loading.py"""
import struct
from PIL import Image
im = Image.open("tools/icon/loading_source.png").convert("RGBA").resize((1280, 720), Image.LANCZOS)
open("assets/loading.e0i", "wb").write(b"E0I1" + struct.pack("<HH", *im.size) + im.tobytes())
print("ok", im.size)

# RAM/APK: vira ETC2 (E0I2); ver tools/compress_textures.py
import subprocess, sys
subprocess.run([sys.executable, "tools/compress_textures.py", "assets/loading.e0i"], check=True)
