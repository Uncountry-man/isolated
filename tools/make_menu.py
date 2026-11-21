#!/usr/bin/env python3
"""Gera assets/menu.e0i (atlas E0I1 RGBA8) e engine/src/menu_atlas.h (retangulos dos sprites) para o MENU PRINCIPAL.
Textos com acento (fonte 5x7 do motor nao tem) sao rasterizados aqui com Pirata One (titulo) e Cinzel (itens).
Sprites de texto/icone sao BRANCOS (o motor tinge no desenho); o titulo ja sai colorido (ouro).
Uso: python3 tools/make_menu.py   (requer Pillow e cairosvg)"""
import io, struct
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
import cairosvg

PIRATA = "tools/fonts/PirataOne-Regular.ttf"
CINZEL = "tools/fonts/Cinzel-Variable.ttf"
ATLAS_W = 2048

def cinzel(size, weight=700):
    f = ImageFont.truetype(CINZEL, size)
    f.set_variation_by_axes([weight])
    return f

def text_sprite(txt, font, spacing=0, pad=6):
    """Texto branco com alfa, com espaco entre letras."""
    widths = [font.getlength(c) for c in txt]
    w = int(sum(widths) + spacing * (len(txt) - 1)) + pad * 2
    asc, desc = font.getmetrics()
    h = asc + desc + pad * 2
    im = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    x = pad
    for c, cw in zip(txt, widths):
        d.text((x, pad), c, font=font, fill=(255, 255, 255, 255))
        x += cw + spacing
    return im

def gold_title(txt, size):
    font = ImageFont.truetype(PIRATA, size)
    pad = 40
    bbox = font.getbbox(txt)
    w, h = bbox[2] - bbox[0] + pad * 2, bbox[3] - bbox[1] + pad * 2
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).text((pad - bbox[0], pad - bbox[1]), txt, font=font, fill=255)
    # degradê vertical de ouro velho
    grad = Image.new("RGBA", (w, h))
    gp = grad.load()
    stops = [(0.00, (255, 243, 188)), (0.38, (238, 190, 78)), (0.62, (200, 132, 38)), (1.00, (122, 66, 18))]
    for y in range(h):
        t = y / (h - 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0)
                c = tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
                break
        for x in range(w):
            gp[x, y] = c + (255,)
    # contorno marrom-escuro (4 px), sombra suave e brilho interno
    outline = mask.filter(ImageFilter.MaxFilter(9))
    shadow = outline.filter(ImageFilter.GaussianBlur(10))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sh = Image.new("RGBA", (w, h), (0, 0, 0, 0)); sh.putalpha(shadow.point(lambda v: int(v * 0.85)))
    out.alpha_composite(ImageChops.offset(sh, 6, 10))
    ol = Image.new("RGBA", (w, h), (38, 18, 6, 255)); ol.putalpha(outline)
    out.alpha_composite(ol)
    fill = grad.copy(); fill.putalpha(mask)
    out.alpha_composite(fill)
    # fio de luz no topo das letras
    edge = ImageChops.subtract(mask, ImageChops.offset(mask, 0, 3))
    hl = Image.new("RGBA", (w, h), (255, 250, 225, 255)); hl.putalpha(edge.point(lambda v: int(v * 0.55)))
    out.alpha_composite(hl)
    return out

def ornament(width, color=(255, 255, 255, 255)):
    """Divisoria: linha com losango no centro e pontas afinando."""
    h = 40
    im = Image.new("RGBA", (width, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = width // 2, h // 2
    for side in (-1, 1):
        for x in range(0, cx - 26):
            a = int(255 * min(1.0, (x / (cx - 26)) ** 0.6))
            px = cx + side * (26 + x) if side > 0 else cx - 26 - x
            d.line([(px, cy - 1), (px, cy)], fill=color[:3] + (a,))
    d.polygon([(cx, cy - 13), (cx + 13, cy), (cx, cy + 13), (cx - 13, cy)], fill=color)
    d.polygon([(cx, cy - 6), (cx + 6, cy), (cx, cy + 6), (cx - 6, cy)], fill=(0, 0, 0, 0))
    for dx in (-24, 24):
        d.ellipse([cx + dx - 3, cy - 3, cx + dx + 3, cy + 3], fill=color)
    return im

def compass_star(size):
    """Marcador do item selecionado: estrela de 4 pontas (rosa dos ventos)."""
    s = size * 4
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = s / 2
    L, k = c * 0.98, c * 0.22
    d.polygon([(c, c - L), (c + k, c - k), (c + L * 0.62, c), (c + k, c + k), (c, c + L), (c - k, c + k), (c - L * 0.62, c), (c - k, c - k)], fill=(255, 255, 255, 255))
    d.polygon([(c, c - L * 0.62), (c + k * 0.7, c - k * 0.7), (c + L, c), (c + k * 0.7, c + k * 0.7), (c, c + L * 0.62), (c - k * 0.7, c + k * 0.7), (c - L, c), (c - k * 0.7, c - k * 0.7)], fill=(255, 255, 255, 150))
    return im.resize((size, size), Image.LANCZOS)

def svg_icon(path, size):
    png = cairosvg.svg2png(url=path, output_width=size, output_height=size, scale=1.0)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    a = im.split()[3]
    white = Image.new("RGBA", im.size, (255, 255, 255, 255)); white.putalpha(a)
    return white

# ---- sprites ----
sprites = []   # (nome, imagem)
sprites.append(("TITLE", gold_title("Isolated", 330)))
sprites.append(("SUBTITLE", text_sprite("SOBREVIVÊNCIA PIRATA", cinzel(40, 600), spacing=14)))
sprites.append(("ORNAMENT", ornament(760)))
F = cinzel(78, 700)
sprites.append(("PLAY", text_sprite("JOGAR", F, spacing=8)))
sprites.append(("CHARACTER", text_sprite("PERSONAGEM", F, spacing=8)))
sprites.append(("SETTINGS", text_sprite("CONFIGURAÇÕES", F, spacing=8)))
sprites.append(("QUIT", text_sprite("SAIR", F, spacing=8)))
FS = cinzel(58, 700)
sprites.append(("SET_HEADER", text_sprite("CONFIGURAÇÕES", cinzel(72, 800), spacing=8)))
sprites.append(("SET_QUALITY", text_sprite("QUALIDADE", FS, spacing=6)))
sprites.append(("SET_FILTER", text_sprite("FILTRO PIRATA", FS, spacing=6)))
sprites.append(("SET_SOUND", text_sprite("SOM", FS, spacing=6)))
sprites.append(("SET_BACK", text_sprite("VOLTAR", F, spacing=8)))
for key, txt in [("V_AUTO", "AUTO"), ("V_LOW", "BAIXA"), ("V_MED", "MÉDIA"), ("V_HIGH", "ALTA"),
                 ("V_OFF", "DESLIGADO"), ("V_SOFT", "SUAVE"), ("V_STRONG", "FORTE"), ("V_ON", "LIGADO")]:
    sprites.append((key, text_sprite(txt, FS, spacing=6)))
sprites.append(("SOON", text_sprite("EM BREVE", cinzel(46, 700), spacing=8)))
sprites.append(("INVITE", text_sprite("CONVIDAR AMIGOS", F, spacing=8)))
sprites.append(("CREDITS", text_sprite("CRÉDITOS", F, spacing=8)))
sprites.append(("CRED_HEADER", text_sprite("CRÉDITOS", cinzel(72, 800), spacing=8)))
sprites.append(("SET_SERVER", text_sprite("SERVIDOR", FS, spacing=6)))
sprites.append(("V_DEFAULT", text_sprite("PADRÃO", FS, spacing=6)))
FM = cinzel(40, 700)
for key, txt in [("SRV_ONLINE", "ONLINE"), ("SRV_OFFLINE", "OFFLINE"), ("SRV_CHECK", "VERIFICANDO"), ("SRV_LOCAL", "MODO LOCAL"),
                 ("BAD_ADDR", "ENDEREÇO INVÁLIDO"), ("MS", "MS")]:
    sprites.append((key, text_sprite(txt, FM, spacing=5)))
for d in "0123456789":                       # D0..D9 precisam ficar CONSECUTIVOS (o motor usa MS_D0 + digito)
    sprites.append(("D" + d, text_sprite(d, FM, spacing=0, pad=3)))
sprites.append(("DDOT", text_sprite(".", FM, spacing=0, pad=3)))
sprites.append(("DCOLON", text_sprite(":", FM, spacing=0, pad=3)))
FC = cinzel(34, 600)
CREDITS = [   # (titulo, texto) - so o que e verificavel (ver CREDITOS_ICONES.txt)
    ("JOGO", "Isolated - sobrevivência cooperativa pirata"),
    ("PERSONAGENS E ANIMAÇÕES", "Piratas: mastjie, poly.pizza (CC0) - animações: Mixamo / Adobe"),
    ("ÍCONES DO JOGO", "game-icons.net - Lorc, Delapouite, Skoll, Zeromancer e Sbed (CC BY 3.0)"),
    ("FONTES", "Pirata One e Cinzel (SIL Open Font License 1.1)"),
    ("ÍCONES SOCIAIS", "Simple Icons (CC0 1.0) e logo do Google - marcas de seus respectivos donos"),
]
for i, (hd, tx) in enumerate(CREDITS):
    sprites.append((f"CRED_H{i}", text_sprite(hd, cinzel(30, 700), spacing=7)))
    sprites.append((f"CRED_B{i}", text_sprite(tx, FC, spacing=1)))
sprites.append(("VERSION", text_sprite("v0.1", cinzel(34, 600), spacing=4)))
sprites.append(("STAR", compass_star(72)))
# ---- TELA DE LOGIN (antes do menu) e CONTA (configurações) ----
FL = cinzel(54, 700)
sprites.append(("LOGIN_HEAD", text_sprite("ENTRE PARA SALVAR SEU PROGRESSO", cinzel(40, 700), spacing=6)))
sprites.append(("LOGIN_GOOGLE", text_sprite("ENTRAR COM GOOGLE", FL, spacing=5)))
sprites.append(("LOGIN_GUEST", text_sprite("JOGAR COMO VISITANTE", FL, spacing=5)))
FN = cinzel(32, 600)
for key, txt in [("LOGIN_NOTE", "VISITANTE: O PROGRESSO FICA SÓ NESTE APARELHO"), ("LOGIN_BUSY", "CONECTANDO..."),
                 ("LOGIN_ERR_CFG", "LOGIN COM GOOGLE AINDA NÃO ESTÁ ATIVO"), ("LOGIN_ERR", "NÃO FOI POSSÍVEL ENTRAR. TENTE DE NOVO"),
                 ("LOGIN_CANCEL", "LOGIN CANCELADO"), ("LOGIN_NET", "SEM INTERNET. VERIFIQUE A CONEXÃO")]:
    sprites.append((key, text_sprite(txt, FN, spacing=4)))
sprites.append(("ACC_GUEST", text_sprite("VISITANTE", FM, spacing=5)))
sprites.append(("ACC_GOOGLE", text_sprite("CONTA GOOGLE", FM, spacing=5)))
sprites.append(("SET_ACCOUNT", text_sprite("CONTA", FS, spacing=6)))
sprites.append(("V_LOGOUT", text_sprite("SAIR DA CONTA", FS, spacing=6)))
_g = cairosvg.svg2png(url="tools/icon/social/google_g.svg", output_width=128, output_height=128)
sprites.append(("G_LOGO", Image.open(io.BytesIO(_g)).convert("RGBA")))   # logo colorido (não é tingido: desenhar com cor branca)
for n in ("discord", "youtube", "instagram", "tiktok"):
    sprites.append(("ICON_" + n.upper(), svg_icon(f"tools/icon/social/{n}.svg", 128)))
sprites.append(("CREW", text_sprite("TRIPULAÇÃO", F, spacing=8)))   # item do menu principal (tela da TRIPULAÇÃO, engine_team.inc)
# ---- LOGIN POR E-MAIL E SENHA (Firebase; conta de revisão da Play) ----
sprites.append(("LOGIN_EMAIL", text_sprite("ENTRAR COM E-MAIL", FL, spacing=5)))
sprites.append(("ACC_EMAIL", text_sprite("CONTA E-MAIL", FM, spacing=5)))
for key, txt in [("LOGIN_ERR_CRED", "E-MAIL OU SENHA INCORRETOS"), ("LOGIN_ERR_EMAIL", "E-MAIL INVÁLIDO"),
                 ("LOGIN_ERR_MANY", "MUITAS TENTATIVAS. ESPERE UM POUCO"), ("LOGIN_ERR_EMPTY", "DIGITE O E-MAIL E A SENHA")]:
    sprites.append((key, text_sprite(txt, FN, spacing=4)))

# ---- empacota (prateleiras) com 2 px de folga ----
GAP = 2
order = sorted(range(len(sprites)), key=lambda i: -sprites[i][1].size[1])
pos = {}
x = y = rowh = 0
for i in order:
    w, h = sprites[i][1].size
    if x + w + GAP > ATLAS_W:
        x, y, rowh = 0, y + rowh + GAP, 0
    pos[i] = (x, y)
    x += w + GAP; rowh = max(rowh, h)
placed = [(sprites[i][0], pos[i][0], pos[i][1], sprites[i][1].size[0], sprites[i][1].size[1]) for i in range(len(sprites))]
atlas_h = ((y + rowh + 7) // 8) * 8
atlas = Image.new("RGBA", (ATLAS_W, atlas_h), (255, 255, 255, 0))
for (name, px, py, w, h), (_, im) in zip(placed, sprites):
    atlas.paste(im, (px, py))
# PRE-MULTIPLICA (o motor desenha com blend ONE, ONE_MINUS_SRC_ALPHA; evita halo claro nas bordas com filtro bilinear)
pm = atlas.copy(); px = pm.load()
for yy in range(pm.size[1]):
    for xx in range(pm.size[0]):
        r, g, b, a = px[xx, yy]
        px[xx, yy] = (r * a // 255, g * a // 255, b * a // 255, a)
atlas_pm = pm
open("assets/menu.e0i", "wb").write(b"E0I1" + struct.pack("<HH", *atlas.size) + atlas_pm.tobytes())

with open("engine/src/menu_atlas.h", "w") as f:
    f.write("// GERADO por tools/make_menu.py - nao editar. Retangulos (pixels) dos sprites em assets/menu.e0i.\n#pragma once\nnamespace e0 {\n")
    f.write(f"static constexpr int kMenuAtlasW = {atlas.size[0]}, kMenuAtlasH = {atlas.size[1]};\n")
    f.write("struct MenuSprite { int x, y, w, h; };\nenum MenuSpriteId {\n")
    for name, *_ in placed: f.write(f"    MS_{name},\n")
    f.write("    MS_COUNT\n};\nstatic constexpr MenuSprite kMenuSprites[MS_COUNT] = {\n")
    for name, px, py, w, h in placed: f.write(f"    {{{px}, {py}, {w}, {h}}},  // {name}\n")
    f.write("};\n}  // namespace e0\n")
atlas.save("/tmp/menu_atlas_preview.png")
print("ok", atlas.size, len(placed), "sprites")
