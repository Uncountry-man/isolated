#!/usr/bin/env python3
"""Gera os mipmaps do icone do app (legado + adaptativo) a partir do PNG original.
Uso: python3 tools/make_appicon.py [png]"""
import sys, os
from PIL import Image, ImageFilter, ImageDraw
SRC = sys.argv[1] if len(sys.argv) > 1 else "tools/icon/icon_source.png"
R = "android/res"
im = Image.open(SRC).convert("RGB")
dens = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
for d, k in dens.items():
    p = f"{R}/mipmap-{d}"
    a = int(108 * k)                       # camada adaptativa
    # fundo: a arte esticada e desfocada (a mascara pode cortar sem aparecer vazio)
    bg = im.resize((a, a), Image.LANCZOS).filter(ImageFilter.GaussianBlur(a / 18))
    bg.save(f"{p}/ic_launcher_background.png")
    # primeiro plano: arte inteira dentro da zona segura (66/108), cantos suaves
    s = int(a * 0.70)
    fg = im.resize((s, s), Image.LANCZOS).convert("RGBA")
    m = Image.new("L", (s * 4, s * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, s * 4 - 1, s * 4 - 1], radius=s * 4 // 5, fill=255)
    fg.putalpha(m.resize((s, s), Image.LANCZOS))
    L = Image.new("RGBA", (a, a), (0, 0, 0, 0)); L.paste(fg, ((a - s) // 2,) * 2, fg)
    L.save(f"{p}/ic_launcher_foreground.png")
    # icone legado (quadrado arredondado) e redondo
    n = int(48 * k)
    for name, rad in (("ic_launcher", 0.18), ("ic_launcher_round", 0.5)):
        t = im.resize((n, n), Image.LANCZOS).convert("RGBA")
        m = Image.new("L", (n * 4, n * 4), 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, n * 4 - 1, n * 4 - 1], radius=int(n * 4 * rad), fill=255)
        t.putalpha(m.resize((n, n), Image.LANCZOS)); t.save(f"{p}/{name}.png")
xml = '''<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_background"/>
    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>
</adaptive-icon>
'''
for n in ("ic_launcher", "ic_launcher_round"):
    open(f"{R}/mipmap-anydpi-v26/{n}.xml", "w").write(xml)
print("ok")
