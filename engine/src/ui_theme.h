// Isolated — Tema Visual Oficial: "Carta Nautica & Carvalho"
// Paleta e constantes unificadas para TODAS as interfaces (HUD, Inventario, Craft, Bau, Loja,
// Personagem, Tripulacao, Bandeira, Navegacao, Mapa, Construcao, Morte, Loading).
// Casa diretamente com o Menu Principal (Pirata One / Cinzel, ouro velho, creme pergaminho, madeira).
#pragma once

namespace e0 {
namespace theme {

struct Col { float r, g, b; };

// ---- Tipografia (pergaminho / creme / latão / tinta sépia) ----
inline constexpr Col kCream       = {0.94f, 0.89f, 0.78f};   // texto principal sobre fundos escuros
inline constexpr Col kCreamSoft   = {0.80f, 0.74f, 0.63f};   // texto secundário / descrições
inline constexpr Col kCreamMuted  = {0.58f, 0.52f, 0.43f};   // rótulos discretos / metadados
inline constexpr Col kBrass       = {0.88f, 0.70f, 0.34f};   // títulos / destaques em latão envelhecido
inline constexpr Col kBrassBright = {0.97f, 0.82f, 0.46f};   // destaque ativo / números importantes
inline constexpr Col kBrassDim    = {0.56f, 0.43f, 0.21f};   // fios / bordas sutis
inline constexpr Col kInk         = {0.15f, 0.09f, 0.04f};   // tinta sépia escura (texto sobre latão ou pergaminho)
inline constexpr Col kInkSoft     = {0.32f, 0.22f, 0.12f};   // texto secundário sobre pergaminho

// ---- Estados semânticos (rústicos, nunca neon) ----
inline constexpr Col kGreenText   = {0.52f, 0.84f, 0.56f};   // positivo / online / fabricável / equipado
inline constexpr Col kRedText     = {0.92f, 0.46f, 0.38f};   // alerta / falta recurso / perigo
inline constexpr Col kAmberText   = {0.96f, 0.74f, 0.34f};   // aviso / moeda / destaque quente
inline constexpr Col kWaterText   = {0.56f, 0.80f, 0.86f};   // náutico / navio / distância

// ---- Fundos de madeira/couro para casos com renderer.rect / uiRoundRect ----
inline constexpr Col kWoodDeep    = {0.09f, 0.07f, 0.05f};
inline constexpr Col kWoodMid     = {0.15f, 0.11f, 0.08f};
inline constexpr Col kWoodWarm    = {0.22f, 0.16f, 0.11f};
inline constexpr Col kWaxRed      = {0.52f, 0.14f, 0.11f};
inline constexpr Col kSeaGreen    = {0.16f, 0.34f, 0.22f};

// ---- Estilos do shader uiWoodPanel (modo 13) ----
enum PanelStyle : int {
    PanelWood      = 0,  // painel principal de carvalho escuro + moldura latão + rebites nos cantos
    PanelCard      = 1,  // cartão interno de couro/madeira + borda bronze sutil
    PanelCardSel   = 2,  // cartão selecionado (borda dupla em latão iluminado)
    PanelSlot      = 3,  // slot rebaixado escuro (inventário / craft / baú)
    PanelSlotHot   = 4,  // slot de acesso rápido / selecionado (moldura latão)
    PanelParchment = 5,  // pergaminho envelhecido com fibras e contorno tinta sépia
    PanelBgWood    = 6,  // fundo de tela cheia em carvalho noturno com vinheta quente
    PanelBgSea     = 7,  // fundo de tela cheia azul-abissal quente (vitrine 3D Personagem)
};

// ---- Variantes do shader uiButton (modo 14) ----
enum BtnStyle : int {
    BtnBrass    = 0,  // ação principal: latão batido (usar texto kInk)
    BtnWood     = 1,  // ação secundária: carvalho escuro + moldura latão (usar texto kCream / kBrass)
    BtnCrimson  = 2,  // perigo / sair / descartar: lacre carmesim escuro (usar texto kCream / kRedText)
    BtnMoss     = 3,  // equipado / confirmado / positivo: verde-musgo náutico (usar texto kCream)
    BtnDisabled = 4,  // indisponível / apagado (usar texto kCreamMuted)
    BtnCrimsonHi= 5,  // confirmação armada de perigo (carmesim aceso + borda latão)
};

}  // namespace theme
}  // namespace e0
