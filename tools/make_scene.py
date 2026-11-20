#!/usr/bin/env python3
"""Gera assets/scene_deck.e0i (E0I1 RGBA8) a partir de tools/icon/scene_deck_source.png: arte do convés ao pôr do sol, usada de FUNDO
das telas NAVEGAÇÃO e BANDEIRA (Renderer::uiBackdrop). Uso: python3 tools/make_scene.py"""
import struct
from PIL import Image
im = Image.open("tools/icon/scene_deck_source.png").convert("RGBA").resize((1280, 720), Image.LANCZOS)
open("assets/scene_deck.e0i", "wb").write(b"E0I1" + struct.pack("<HH", *im.size) + im.tobytes())
print("ok", im.size)

# RAM/APK: vira ETC2 (E0I2); ver tools/compress_textures.py
import subprocess, sys
subprocess.run([sys.executable, "tools/compress_textures.py", "assets/scene_deck.e0i"], check=True)
