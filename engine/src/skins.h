// Isolated - SKINS do personagem (tela PERSONAGEM do menu principal). Os personagens são PIRATAS SEMI-REALISTAS (corpo/rosto low-poly do mastjie +
// tricórnio da AliceCassie, CC0, sem chapéu, repintados por tools/export_pirates.py e presos ao esqueleto do Y Bot): modelo 0 = HOMEM (capitão), 1 = MULHER (corsária). Cada skin = um modelo +
// um MATERIAL aplicado no shader do personagem (kCharMain em renderer.cpp): textura original, ou a textura em tons de uma cor (prata, ouro, fantasma)
// com reflexo de metal e/ou brilho próprio. O servidor só repassa o número (PlayerState.skin).
// Textos em Latin-1 (a fonte 5x7 desenha os acentos): "\xC3" = Ã, "\xC1" = Á etc. Cuidado com o escape seguido de letra a-f: separe as strings.
// Skin nova: acrescente NO FIM (o número vai na rede e no menu.cfg) e aumente e0net::kSkinCount em shared/protocol.h (protocolo novo).
#pragma once
#include "vecmath.h"
#include "../../shared/protocol.h"

namespace e0 {

constexpr int kCharModels = 4;   // 0 = Tomás (assets/pirate_m*), 1 = Isabela (pirate_f*), 2 = pirata gordo (pirate_g*), 3 = corsária de casaco (pirate_c*)
enum SkinRarity { RarCommon = 0, RarRare = 1, RarEpic = 2, RarLegend = 3 };

struct SkinDef {
    const char* name;      // nome do personagem (MAIÚSCULAS, Latin-1)
    const char* title;     // apelido ("O LOBO DO CAIS")
    const char* desc;      // história curta (até ~5 linhas no painel)
    int rarity;            // SkinRarity
    int model;             // 0 homem, 1 mulher
    const char* origin;    // de onde vem (mostrado como ORIGEM)
    Vec3 tint;             // cor do material / do brilho
    float tintK;           // 0 = textura original; 1 = só tons de `tint` (mantém o desenho da roupa pelo claro/escuro)
    float metal;           // 0..1: reflexo forte e tingido
    float glow;            // 0..1: brilho próprio na cor `tint` (pulsa devagar)
};

inline const SkinDef* skinTable() {
    static const SkinDef k[e0net::kSkinCount] = {
        {"TOM\xC1S VIEIRA", "O LOBO DO CAIS",
         "CRESCEU NAS DOCAS CARREGANDO A CARGA DOS OUTROS. NO DIA EM QUE ROUBOU O PRIMEIRO NAVIO, JUROU QUE NUNCA MAIS TERIA PATR\xC3" "O.",
         RarCommon, 0, "BA\xCD" "A DE TODOS OS SANTOS", {1, 1, 1}, 0, 0, 0},
        {"ISABELA DUARTE", "A ROSA NEGRA",
         "FUGIU DE UM CASAMENTO ARRANJADO ESCONDIDA NUM NAVIO MERCANTE E TOMOU O LEME NA PRIMEIRA TEMPESTADE. NENHUM MAR A ASSUSTA.",
         RarCommon, 1, "ILHAS DE CABO VERDE", {1, 1, 1}, 0, 0, 0},
        {"BARTOLOMEU SOUSA", "O BARRIL",
         "COZINHEIRO DE BORDO QUE VIROU PIRATA QUANDO O CAPIT\xC3" "O RECLAMOU DO ENSOPADO. AGUENTA RUM, CANH\xC3" "O E TEMPESTADE SEM PERDER O APETITE.",
         RarCommon, 2, "PORTO DO RECIFE", {1, 1, 1}, 0, 0, 0},
        {"LEONOR VALENTE", "A DAMA DO TRIC\xD3RNIO",
         "FILHA DE UM ALMIRANTE, TROCOU OS BAILES PELO CONV\xC9" "S E HOJE CA\xC7" "A OS NAVIOS DA FROTA DO PR\xD3PRIO PAI. NUNCA PERDE UMA ABORDAGEM.",
         RarRare, 3, "COSTA DO CARIBE", {1, 1, 1}, 0, 0, 0},
    };
    return k;
}
inline const SkinDef& skinDef(int i) { return skinTable()[i >= 0 && i < e0net::kSkinCount ? i : 0]; }
inline const char* rarityName(int r) {
    static const char* k[4] = {"COMUM", "RARO", "\xC9" "PICO", "LEND\xC1" "RIO"};
    return k[r < 0 ? 0 : (r > 3 ? 3 : r)];
}
inline Vec3 rarityColor(int r) {
    static const Vec3 k[4] = {{0.72f, 0.72f, 0.70f}, {0.30f, 0.62f, 1.00f}, {0.72f, 0.40f, 1.00f}, {1.00f, 0.76f, 0.25f}};
    return k[r < 0 ? 0 : (r > 3 ? 3 : r)];
}

}  // namespace e0
