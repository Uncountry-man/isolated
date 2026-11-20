#!/usr/bin/env python3
# Ícones da HUD (botões, barras de fome/vida/moedas): rasteriza SVGs do game-icons.net (CC BY 3.0, ver CREDITOS_ICONES.txt)
# e ANEXA ao atlas assets/icons.e0i como linhas extras (4 colunas x 128 px, depois das 3 linhas dos itens). Idempotente.
# Ícones saem BRANCOS com alfa (o shader multiplica pela cor pedida em uiIconTint).
# Uso: python3 tools/make_hud_icons.py tools/icon/hud [--preview saida.png]   (os SVGs ficam versionados em tools/icon/hud/<autor>/<nome>.svg)
# Rasterização: gdk-pixbuf-thumbnailer (SVG branco sobre fundo preto -> luminância vira alfa).
import sys, os, struct, subprocess, tempfile
import numpy as np
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATLAS = os.path.join(ROOT, 'assets', 'icons.e0i')
CELL, COLS, BASE_ROWS = 128, 4, 3
# ordem = índice HUD (índice no atlas = 12 + i). Manter igual a enum HudIcon em engine.cpp.
ICONS = [
    ('CORTAR',    'lorc/battle-axe'),
    ('MINERAR',   'delapouite/war-pick'),
    ('BATER',     'lorc/fist'),
    ('COLETAR',   'lorc/grab'),
    ('CONSTRUIR', 'lorc/hammer-nails'),
    ('INVENTARIO','delapouite/backpack'),
    ('CRAFT',     'lorc/anvil-impact'),
    ('EMBARCAR',  'delapouite/sailboat'),
    ('DESCER',    'delapouite/exit-door'),
    ('LEME',      'delapouite/ship-wheel'),
    ('CANHAO',    'delapouite/pirate-cannon'),
    ('ABRIR',     'skoll/open-chest'),
    ('SALVAR',    'zeromancer/heart-plus'),
    ('CARREGAR',  'lorc/lift'),
    ('FOME',      'delapouite/coconuts'),
    ('VIDA',      'lorc/heart-inside'),
    ('MOEDAS',    'delapouite/coins'),
    ('ACELERAR',  'delapouite/speedometer'),
    ('RE',        'lorc/back-forth'),
    ('SOLTAR',    'skoll/drop-weapon'),
    ('TECIDO',    'delapouite/rolled-cloth'),
    ('MAPA',      'lorc/treasure-map'),
    ('CORRER',    'lorc/run'),
    ('CAMERA',    'delapouite/photo-camera'),
    # LOJA (botão + arte dos cartões)
    ('LOJA',      'delapouite/shop'),
    ('CHAPEU',    'delapouite/pirate-hat'),
    ('PAPAGAIO',  'lorc/parrot-head'),
    ('TAPAOLHO',  'delapouite/eyepatch'),
    ('BANDANA',   'delapouite/bandana'),
    ('ID',        'delapouite/id-card'),
    ('GALEAO',    'lorc/galleon'),
    ('BANDEIRA',  'delapouite/pirate-flag'),
    ('CAVEIRA',   'lorc/skull-crossed-bones'),
    ('PILHA',     'delapouite/coins-pile'),
    ('FANTASMA',  'lorc/ghost'),
    ('SABRES',    'lorc/crossed-sabres'),
    ('BAUTRANC',  'lorc/locked-chest'),
    ('CASACO',    'delapouite/pirate-coat'),
    ('CAPITAO',   'delapouite/pirate-captain'),
    ('COROA',     'lorc/crown'),
    ('FECHAR',    'lorc/cross-mark'),
    ('MAIS',      'sbed/health-normal'),
    ('2MOEDAS',   'delapouite/two-coins'),
    ('JOIA',      'lorc/emerald'),
    # BACAMARTE (arma de fogo)
    ('BACAMARTE', 'lorc/blunderbuss'),
    ('MIRA',      'delapouite/crosshair'),
    ('TIRO',      'lorc/gunshot'),
    ('RECARGA',   'delapouite/reload-gun-barrel'),
    ('POLVORA',   'delapouite/powder-bag'),
    # RELÓGIO da HUD (hora do jogo)
    ('RELOGIO',   'delapouite/alarm-clock'),
    # RODA DE AÇÕES (botão abaixo do minimapa)
    ('RODA',      'lorc/compass'),
    # CHAT (botão ao lado da roda)
    ('CHAT',      'delapouite/chat-bubble'),
    # FIGURAS DA BANDEIRA (editor de bandeira; botão BANDEIRA ao lado do chat)
    ('ANCORA',    'lorc/anchor'),
    ('CAVEIRA2',  'lorc/dread-skull'),
    ('CAVCOROA',  'lorc/crowned-skull'),
    ('OSSOS',     'lorc/crossed-bones'),
    ('ESPADAS',   'lorc/crossed-swords'),
    ('POLVO',     'lorc/octopus'),
    ('SERPENTE',  'lorc/sea-serpent'),
    ('TUBARAO',   'lorc/shark-jaws'),
    ('BARBATANA', 'delapouite/shark-fin'),
    ('CORVO',     'lorc/raven'),
    ('AMPULHETA', 'lorc/hourglass'),
    ('CORACAO',   'lorc/bleeding-heart'),
    ('ROSA',      'lorc/rose'),
    ('TRIDENTE',  'lorc/trident'),
    ('DRAGAO',    'lorc/dragon-head'),
    ('LOBO',      'lorc/wolf-head'),
    ('COBRA',     'lorc/snake'),
    ('ESCORPIAO', 'lorc/scorpion'),
    ('SOL',       'lorc/sun'),
    ('LUA',       'lorc/moon'),
    ('ESTRELA',   'lorc/barbed-star'),
    ('RAIO',      'lorc/lightning-helix'),
    ('LEAO',      'lorc/lion'),
    ('CAVRACHA',  'lorc/skull-crack'),
    ('DRAGMAR', 'lorc/sea-dragon'),
    ('AGUAVIVA', 'lorc/jellyfish'),
    ('CONCHA', 'lorc/spiral-shell'),
    ('NASCSOL', 'lorc/sunrise'),
    ('CAVESTR', 'lorc/star-skull'),
    ('TRIFOGO', 'lorc/flaming-trident'),
    ('CORVOVOO', 'lorc/crow-dive'),
    ('CORPART', 'lorc/broken-heart'),
    ('ESPPEDRA', 'lorc/sword-in-stone'),
    ('LOBOUIVA', 'lorc/wolf-howl'),
    ('REDEMOIN', 'lorc/star-swirl'),
    ('ESTRMAR', 'delapouite/sea-star'),
    ('LAMINA', 'lorc/crescent-blade'),
    ('CAVRAIO', 'lorc/skull-bolt'),
    ('AGUIA', 'lorc/eagle-emblem'),
    ('GARRAS', 'lorc/claw-slashes'),
    ('OLHO', 'lorc/bleeding-eye'),
    ('LUASIN', 'lorc/evil-moon'),
    ('FOICE', 'lorc/reaper-scythe'),
    ('CEIFADOR', 'lorc/grim-reaper'),
    ('CAVKRAKEN', 'lorc/tentacles-skull'),
    ('BALEIA', 'delapouite/whale-tail'),
    ('PEIXEDIABO', 'lorc/angler-fish'),
    ('TARTARUGA', 'delapouite/sea-turtle'),
    ('CARANGUEJO', 'lorc/crab'),
    ('CAVMARINHO', 'delapouite/seahorse'),
    ('SEREIA', 'delapouite/mermaid'),
    ('CHAVE', 'lorc/skeleton-key'),
    ('AS', 'lorc/ace'),
    ('MORCEGO', 'delapouite/bat'),
    ('HIDRA', 'lorc/hydra'),
    ('ANELFOGO', 'lorc/fire-ring'),
    ('CAVCHIFRE', 'lorc/horned-skull'),
    ('DEMONIO', 'lorc/daemon-skull'),
    ('TRESCAV', 'lorc/triple-skulls'),
    ('ARPAO', 'lorc/harpoon-chain'),
    ('FAROL', 'delapouite/lighthouse'),
    ('NAUFRAGIO', 'delapouite/sinking-ship'),
    ('ONDA', 'lorc/wave-crest'),
    ('TEIA', 'lorc/spider-web'),
]
def raster(svg):
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, 'o.png')
        subprocess.run(['gdk-pixbuf-thumbnailer', '-s', str(CELL * 2), svg, out], check=True)
        im = np.asarray(Image.open(out).convert('RGB')).astype(np.float32) / 255.0
    a = im.mean(axis=2)                       # branco = ícone, preto = fundo
    a = np.clip((a - 0.04) / 0.92, 0, 1)
    img = Image.fromarray((a * 255).astype(np.uint8)).resize((CELL - 8, CELL - 8), Image.LANCZOS)
    cell = np.zeros((CELL, CELL, 4), np.uint8); cell[..., :3] = 255
    cell[4:CELL - 4, 4:CELL - 4, 3] = np.asarray(img)
    return cell
def main():
    repo = sys.argv[1]
    with open(ATLAS, 'rb') as f: d = f.read()
    assert d[:4] == b'E0I1'
    w, h = struct.unpack('<HH', d[4:8]); assert w == COLS * CELL
    base = d[8:8 + w * BASE_ROWS * CELL * 4]            # só as 3 linhas dos itens
    nrows = (len(ICONS) + COLS - 1) // COLS
    extra = np.zeros((nrows * CELL, w, 4), np.uint8)
    for i, (name, rel) in enumerate(ICONS):
        p = os.path.join(repo, rel + '.svg')
        if not os.path.exists(p): sys.exit('SVG não existe: ' + p)
        r, c = divmod(i, COLS)
        extra[r * CELL:(r + 1) * CELL, c * CELL:(c + 1) * CELL] = raster(p)
    th = (BASE_ROWS + nrows) * CELL
    with open(ATLAS, 'wb') as f:
        f.write(b'E0I1'); f.write(struct.pack('<HH', w, th)); f.write(base); f.write(extra.tobytes())
    print('ESCRITO', ATLAS, w, th, '%d ícones HUD (índices %d..%d)' % (len(ICONS), 12, 12 + len(ICONS) - 1))
    if '--preview' in sys.argv:
        p = sys.argv[sys.argv.index('--preview') + 1]
        bg = np.full((extra.shape[0], w, 3), 70, np.uint8)
        comp = (extra[..., :3] * (extra[..., 3:] / 255.0) + bg * (1 - extra[..., 3:] / 255.0)).astype(np.uint8)
        Image.fromarray(comp).save(p)
main()
