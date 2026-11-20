#!/usr/bin/env python3
"""Gera assets/ui_font.e0i (atlas R8 com mipmaps) e engine/src/ui_font.h para toda a UI do jogo.
Usa Cinzel (a mesma fonte do Menu Principal) com acentos Latin-1 nativos e metricas proporcionais
calibradas para cap-height = 7.0 unidades (drop-in com Renderer::text(x, y, scale, ...)).
Uso: python3 tools/make_ui_font.py"""
import struct
from PIL import Image, ImageDraw, ImageFont

CINZEL = "tools/fonts/Cinzel-Variable.ttf"
ATLAS_W = 1024
FONT_SIZE = 74
WEIGHT = 680
PAD = 6  # folga para mipmaps sem sangramento

font = ImageFont.truetype(CINZEL, FONT_SIZE)
font.set_variation_by_axes([WEIGHT])

# Mede cap-height de referencia usando 'H' (topo e base exatos)
hb = font.getbbox("H")
CAP_TOP = hb[1]
CAP_BOT = hb[3]
CAP_H = float(CAP_BOT - CAP_TOP)
UNIT = CAP_H / 7.0  # 1 unidade de escala = UNIT pixels no atlas

# Conjunto de caracteres: ASCII 32..95 (e 96..126 mapeiam para maiusculas quando letra) + Latin-1 0xC0..0xDF
# Vamos gerar tabela direta indexada por codigo 0..255 (Latin-1):
# - a..z mapeia para A..Z
# - 0xE0..0xFE (exceto 0xF7) mapeia para 0xC0..0xDE
# - 0xFF -> Y
CHARS = []
for cp in range(32, 96):
    CHARS.append(cp)
for cp in (96, 123, 124, 125, 126):  # ` { | } ~
    CHARS.append(cp)
for cp in range(0xC0, 0xE0):
    if cp in (0xD7,):  # multiply sign -> ok incluir tambem
        pass
    CHARS.append(cp)

glyphs = {}  # cp -> (img_L, xoff_px, yoff_px, adv_px)
for cp in CHARS:
    ch = chr(cp)
    adv = float(font.getlength(ch))
    if cp == 32:
        glyphs[cp] = (None, 0.0, 0.0, adv * 0.92)
        continue
    bb = font.getbbox(ch)
    if not bb or bb[2] <= bb[0] or bb[3] <= bb[1]:
        glyphs[cp] = (None, 0.0, 0.0, adv)
        continue
    gw = bb[2] - bb[0] + 4
    gh = bb[3] - bb[1] + 4
    im = Image.new("L", (gw, gh), 0)
    d = ImageDraw.Draw(im)
    d.text((2 - bb[0], 2 - bb[1]), ch, font=font, fill=255)
    # xoff relativo a origem x; yoff relativo a CAP_TOP (0 = topo do 'H')
    xoff = float(bb[0] - 2)
    yoff = float(bb[1] - 2 - CAP_TOP)
    # leve tracking de 0.35 unidades para ficar elegante e respirado como no Menu Principal
    glyphs[cp] = (im, xoff, yoff, adv + 0.32 * UNIT)

# Empacota em prateleiras; reserva bloco 16x16 solido branco em (2, 2) para Renderer::rect()
x = 24
y = 4
rowh = 20
placed = {}  # cp -> (ax, ay, gw, gh)

order = sorted([cp for cp in CHARS if glyphs[cp][0] is not None], key=lambda c: -glyphs[c][0].size[1])
for cp in order:
    im = glyphs[cp][0]
    gw, gh = im.size
    if x + gw + PAD > ATLAS_W - 2:
        x = 4
        y += rowh + PAD
        rowh = 0
    placed[cp] = (x, y, gw, gh)
    x += gw + PAD
    rowh = max(rowh, gh)

atlas_h = 1
while atlas_h < y + rowh + 4:
    atlas_h *= 2

atlas = Image.new("L", (ATLAS_W, atlas_h), 0)
# Bloco solido branco 16x16 em (2,2)..(18,18); centro UV = (10/W, 10/H) -> sem vazamento mesmo com mipmap
ImageDraw.Draw(atlas).rectangle([2, 2, 18, 18], fill=255)
for cp, (ax, ay, gw, gh) in placed.items():
    atlas.paste(glyphs[cp][0], (ax, ay))

# Salva assets/ui_font.e0i (E0F1 + w u16 + h u16 + R8 bytes)
with open("assets/ui_font.e0i", "wb") as f:
    f.write(b"E0F1" + struct.pack("<HH", ATLAS_W, atlas_h) + atlas.tobytes())

# Gera tabela de 256 entradas em engine/src/ui_font.h
def resolve_cp(cp):
    if ord('a') <= cp <= ord('z'):
        return cp - 32
    if 0xE0 <= cp <= 0xFE and cp != 0xF7:
        return cp - 0x20
    if cp == 0xFF:
        return ord('Y')
    return cp

with open("engine/src/ui_font.h", "w") as f:
    f.write("// GERADO por tools/make_ui_font.py - nao editar. Fonte Cinzel para toda a UI (assets/ui_font.e0i).\n")
    f.write("#pragma once\n#include <cstdint>\n\nnamespace e0 {\n\n")
    f.write(f"static constexpr int kUiFontW = {ATLAS_W}, kUiFontH = {atlas_h};\n")
    f.write(f"static constexpr float kUiSolidU = {10.0 / ATLAS_W:.6f}f, kUiSolidV = {10.0 / atlas_h:.6f}f;\n")
    f.write("struct UiGlyph { float u0, v0, u1, v1; float xo, yo, w, h, adv; };\n")
    f.write("static constexpr UiGlyph kUiGlyphs[256] = {\n")
    for cp in range(256):
        rcp = resolve_cp(cp)
        if rcp not in glyphs:
            rcp = ord('?')
        im, xoff, yoff, adv = glyphs[rcp]
        if im is None:
            f.write(f"    {{0, 0, 0, 0, 0, 0, 0, 0, {adv / UNIT:.4f}f}},  // {cp}\n")
        else:
            ax, ay, gw, gh = placed[rcp]
            u0 = ax / ATLAS_W
            v0 = ay / atlas_h
            u1 = (ax + gw) / ATLAS_W
            v1 = (ay + gh) / atlas_h
            f.write(f"    {{{u0:.6f}f, {v0:.6f}f, {u1:.6f}f, {v1:.6f}f, {xoff / UNIT:.4f}f, {yoff / UNIT:.4f}f, {gw / UNIT:.4f}f, {gh / UNIT:.4f}f, {adv / UNIT:.4f}f}},  // {cp}\n")
    f.write("};\n\n}  // namespace e0\n")

atlas.save("/tmp/ui_font_preview.png")
print(f"ok ui_font: {ATLAS_W}x{atlas_h}, cap_h={CAP_H:.1f}px, unit={UNIT:.2f}px, glyphs={len(placed)}")
