// Isolated - NAVIOS: estado, física do casco (autoridade = servidor; o cliente roda o MESMO código para prever) e geometria do convés.
// Header-only. Há kMaxShips navios no mar, um de cada tipo (ShipKind). Tudo que muda de um para o outro está em ShipDef (kShipDefs).
// Modelos: assets/ship.e0s (tools/export_ship.py), assets/galleon.e0s (tools/export_galleon.py), boat.e0s e brig.e0s (export_boat/export_brig.py): linha d'água em y = 0, proa em -Z local.
//
// Espaço do navio: (a, b) = (distância para a PROA, distância para a DIREITA/estibordo) a partir da origem do modelo.
//   mundo = (x + sin(yaw)*a + cos(yaw)*b ,  z - cos(yaw)*a + sin(yaw)*b)        (yaw 0 = proa para -Z, igual aos jogadores)
// Quem está a bordo (PlayerState.ship != 0) guarda a posição em (sa, sb) = espaço do navio: o convés se move e o jogador vai junto sem
// nenhuma correção de rede; x/z do mundo são derivados da pose do navio a cada passo.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "terrain.h"

namespace e0net {

constexpr int kMaxShips = 4;
enum ShipKind : uint8_t { ShipSloop = 0, ShipGalleon = 1, ShipBoat = 2, ShipBrig = 3 };

// Medidas do navio original. As do galeão saem destas por galleonA/kGalB/kGalY: o MESMO mapeamento de tools/export_galleon.py (mudou lá, mude aqui).
constexpr float kShipDeckY = 1.01f;          // altura do convés acima da água (m): topo das tábuas (y 5.16 do modelo, linha d'água em 3.9, escala 0.8)
constexpr float kShipBoardDist = 5.5f;       // distância máx. (m) do casco para embarcar
constexpr float kDeckAMin = -7.6f, kDeckAMax = 7.3f;
constexpr float kShipMidB = -0.1f;           // o modelo não é centrado em b = 0: mastros, escadas e vão ficam em torno daqui
constexpr float kDeckWallA = -7.45f;         // entre as escadas o convés vai até a parede sob o toldo (a = -7,8), menos o raio do jogador
constexpr float kHelmA = -9.5f;              // onde o timoneiro fica (b = 0): no TOLDO da popa (o leme "lá em cima"), de frente para a proa
constexpr float kHelmWheelA = -8.55f;        // posição do timão (roda) à frente do timoneiro
constexpr float kHelmWheelY = 4.28f;         // altura do centro da roda (m acima da água)
constexpr float kHelmReach = 2.3f;           // distância máx. (no plano) do ponto do leme para assumi-lo
constexpr float kTerraceY = 3.36f;           // altura do toldo da popa (convés do leme); o convés principal fica em kShipDeckY
// Escadas laterais: rampa que passa pelo meio de cada degrau do modelo (a -8,0 y 1,41; -8,4 y 1,80; -8,8 y 2,20; -9,2 y 2,60; -9,65 y 3,0): medido na malha.
constexpr float kStairA0 = -7.6f, kStairA1 = -10.0f;
constexpr float kStairB0 = 1.9f, kStairB1 = 2.95f;    // |b - midB|: entre a parede central e a amurada
// Baú do convés, encostado na parede sob o toldo, no vão entre as escadas. Colisão = caixa; desenho em Renderer::buildChest.
constexpr float kChestA = -7.4f, kChestB = kShipMidB, kChestHalfA = 0.35f, kChestHalfB = 0.55f;
constexpr float kChestReach = 1.1f;          // distância máx. (m) da caixa do baú para interagir
constexpr float kNoFloor = -1e9f;

struct ShipDef {
    const char* name;
    float deckY, terraceY;                       // convés principal e toldo da popa
    float aMin, aMax, midB, wallA;               // limites do CENTRO do jogador no convés; wallA = até onde vai entre as escadas
    float stairA0, stairA1, stairB0, stairB1;    // rampa das escadas
    float terBackA, terBackHalfB, terMidA, terMidHalfB;   // toldo: faixa de trás (até terBackA) e miolo (até terMidA)
    float hwMid, hwTaperA, hwEnd;                // meia-largura do convés: hwMid até hwTaperA, afunila até hwEnd em aMax
    float helmA, helmReach, wheelA, wheelY, wheelScale;
    float chestA, chestB, chestHalfA, chestHalfB, chestReach;
    float lenA0, lenA1, hullHalfB, leaveA;       // caixa do casco (embarcar / sair); leaveA = até onde em a se cai na água ao sair
    int nMasts; float masts[3][3];               // a, b, raio do mastro + raio do jogador
    float hull[9][2];                            // meia-boca na linha d'água em cada a: o casco é SÓLIDO para quem está na água
    float land[12][2];                           // pontos do casco testados contra a terra e contra o outro navio
    float draftH;                                // o casco encalha onde o fundo é mais raso que isto (y do terreno)
    float maxFwd, maxRev, accel, brake, drag, turnRate, rudderRate;   // m/s, m/s, m/s², m/s², 1/s, rad/s com o leme todo (acima de 2,5 m/s), 1/s
    float muzzleA, muzzleB;                      // de onde a bala sai: |a| máx. e b do costado
    float rangeMin, rangeMax;                    // alcance do canhão a partir do centro do navio (m)
    float camDist, camUp, gunHeight;             // câmera do leme (m atrás e acima) e altura da vista de canhoneiro
    float wakeA, wakeB, clearR;                  // escala da esteira em relação ao navio original; folga em volta para nascer
    bool hasCannons;                             // false: sem botão CANHAO e o servidor recusa o disparo
    int maxHp;                                   // VIDA do casco: cada bala de canhão que cai no casco tira até kShipShellDamage; em 0 o navio AFUNDA
};

constexpr ShipDef kShipSloop = {
    "NAVIO", kShipDeckY, kTerraceY,
    kDeckAMin, kDeckAMax, kShipMidB, kDeckWallA,
    kStairA0, kStairA1, kStairB0, kStairB1,
    -11.02f, 3.12f, -8.2f, 1.5f,
    2.85f, 5.4f, 2.0f,
    kHelmA, kHelmReach, kHelmWheelA, kHelmWheelY, 1.0f,
    kChestA, kChestB, kChestHalfA, kChestHalfB, kChestReach,
    -14.0f, 12.0f, 4.0f, 6.0f,
    2, {{-5.3f, -0.1f, 0.62f}, {1.25f, -0.1f, 0.62f}, {0, 0, 0}},
    {{-15.9f, 1.2f}, {-14.0f, 3.0f}, {-12.0f, 3.55f}, {5.4f, 3.65f}, {7.2f, 3.0f}, {8.8f, 2.1f}, {10.4f, 1.75f}, {12.0f, 1.0f}, {13.3f, 0.0f}},
    {{-14.5f, 0.0f}, {-14.0f, 2.6f}, {-14.0f, -2.6f}, {-8.0f, 3.6f}, {-8.0f, -3.6f}, {0.0f, 3.9f}, {0.0f, -3.9f}, {8.0f, 3.4f}, {8.0f, -3.4f}, {12.5f, 1.6f}, {12.5f, -1.6f}, {15.5f, 0.0f}},
    -2.0f,
    8.0f, 2.5f, 1.7f, 3.2f, 0.28f, 0.55f, 1.8f,
    4.0f, 3.3f,
    12.0f, 110.0f,
    52.0f, 10.0f, 140.0f,
    1.0f, 1.0f, 24.0f,
    true,
    250,                                         // ~10 balas
};

// Mapeamento navio original -> galeão (igual a F, SB e SY de tools/export_galleon.py): as pontas crescem 1,3x e o meio do casco [-4, 5] vira [-11, 11].
constexpr float kGalB = 1.35f, kGalY = 1.3f;
constexpr float galleonA(float a) { return a < -4.0f ? 1.3f * (a + 4.0f) - 11.0f : (a > 5.0f ? 1.3f * (a - 5.0f) + 11.0f : -11.0f + (a + 4.0f) * (22.0f / 9.0f)); }
constexpr ShipDef makeGalleon() {
    ShipDef d = kShipSloop;
    d.name = "GALEAO";
    d.deckY = 1.34f; d.terraceY = 4.43f;         // medidos na malha, em cima das tábuas (galleon_raw: 1,32 / 4,40 + 2 cm)
    d.aMin = galleonA(kDeckAMin); d.aMax = galleonA(kDeckAMax); d.midB = kShipMidB * kGalB; d.wallA = galleonA(-7.8f) + 0.35f;
    d.stairA0 = galleonA(kStairA0); d.stairA1 = -19.0f;   // degraus da malha: a -16,2 y 1,83; -16,75 y 2,34; -17,3 y 2,86; -17,85 y 3,37; -18,4 y 3,89
    d.stairB0 = kStairB0 * kGalB; d.stairB1 = kStairB1 * kGalB;
    d.terBackA = galleonA(-11.02f); d.terBackHalfB = 3.12f * kGalB; d.terMidA = galleonA(-8.2f); d.terMidHalfB = 1.5f * kGalB;
    d.hwMid = 2.85f * kGalB; d.hwTaperA = galleonA(5.4f); d.hwEnd = 2.0f * kGalB;
    d.helmA = galleonA(kHelmA); d.helmReach = 3.0f; d.wheelA = galleonA(kHelmWheelA); d.wheelY = d.terraceY + (kHelmWheelY - kTerraceY) * kGalY; d.wheelScale = 1.3f;
    d.chestHalfA = 0.45f; d.chestHalfB = 0.72f; d.chestA = galleonA(-7.8f) + 0.2f + d.chestHalfA; d.chestB = d.midB; d.chestReach = 1.3f;
    d.lenA0 = galleonA(-14.0f); d.lenA1 = galleonA(12.0f); d.hullHalfB = 4.0f * kGalB; d.leaveA = 14.0f;
    d.nMasts = 3;
    d.masts[0][0] = 0.2f; d.masts[1][0] = 11.2f; d.masts[2][0] = -11.3f;   // MAST_MAIN, MAST_FORE, MAST_MIZZEN do exportador
    d.masts[0][2] = 0.8f; d.masts[1][2] = 0.75f; d.masts[2][2] = 0.75f;
    for (int i = 0; i < 3; ++i) d.masts[i][1] = d.midB;
    for (int i = 0; i < 9; ++i) { d.hull[i][0] = galleonA(d.hull[i][0]); d.hull[i][1] *= kGalB; }
    for (int i = 0; i < 12; ++i) { d.land[i][0] = galleonA(d.land[i][0]); d.land[i][1] *= kGalB; }
    d.draftH = -2.6f;
    d.maxFwd = 7.0f; d.maxRev = 2.2f; d.accel = 1.1f; d.brake = 2.4f; d.turnRate = 0.36f; d.rudderRate = 1.4f;   // pesado: mais lento para ganhar velocidade e para virar
    d.muzzleA = 9.0f; d.muzzleB = 4.7f;
    d.rangeMin = 18.0f; d.rangeMax = 140.0f;
    d.camDist = 74.0f; d.camUp = 15.0f; d.gunHeight = 190.0f;
    d.wakeA = 1.65f; d.wakeB = kGalB; d.clearR = 34.0f;
    d.maxHp = 400;                               // ~16 balas
    return d;
}
// Mapeamento navio original -> chalupa (igual a F, SB e SY de tools/export_boat.py): as pontas encolhem para 0,62 e o meio do casco [-4, 5] vira [-2,2; 2,2].
constexpr float kBoatB = 0.7f;
constexpr float boatA(float a) { return a < -4.0f ? 0.62f * (a + 4.0f) - 2.2f : (a > 5.0f ? 0.62f * (a - 5.0f) + 2.2f : -2.2f + (a + 4.0f) * (4.4f / 9.0f)); }
constexpr ShipDef makeBoat() {
    ShipDef d = kShipSloop;
    d.name = "CHALUPA";
    d.deckY = 0.65f; d.terraceY = 2.12f;         // medidos na malha, em cima das tábuas (boat_raw: 0,63 / 2,10 + 2 cm)
    d.aMin = boatA(kDeckAMin); d.aMax = boatA(kDeckAMax); d.midB = -0.07f; d.wallA = boatA(-7.8f) + 0.35f;
    d.stairA0 = boatA(kStairA0); d.stairA1 = -6.0f;   // degraus da malha: a -4,69 y 0,87; -4,96 y 1,12; -5,2 y 1,36; -5,5 y 1,61; -5,73 y 1,85
    d.stairB0 = kStairB0 * kBoatB; d.stairB1 = kStairB1 * kBoatB;
    d.terBackA = boatA(-11.02f); d.terBackHalfB = 3.12f * kBoatB; d.terMidA = boatA(-8.2f); d.terMidHalfB = 1.5f * kBoatB;
    d.hwMid = 2.85f * kBoatB; d.hwTaperA = boatA(5.4f); d.hwEnd = 2.0f * kBoatB;
    d.helmA = boatA(kHelmA); d.helmReach = 1.6f; d.wheelA = boatA(kHelmWheelA); d.wheelY = d.terraceY + 0.69f; d.wheelScale = 0.72f;
    d.chestHalfA = 0.28f; d.chestHalfB = 0.44f; d.chestA = boatA(-7.8f) + 0.12f + d.chestHalfA; d.chestB = d.midB; d.chestReach = 0.95f;
    d.lenA0 = boatA(-14.0f); d.lenA1 = boatA(12.0f); d.hullHalfB = 4.0f * kBoatB; d.leaveA = 2.5f;
    d.nMasts = 1; d.masts[0][0] = 0.7f; d.masts[0][1] = d.midB; d.masts[0][2] = 0.6f;   // MAST_A do exportador
    for (int i = 0; i < 9; ++i) { d.hull[i][0] = boatA(d.hull[i][0]); d.hull[i][1] *= kBoatB; }
    for (int i = 0; i < 12; ++i) { d.land[i][0] = boatA(d.land[i][0]); d.land[i][1] *= kBoatB; }
    d.draftH = -1.7f;                            // quilha em y -1,59: chega bem mais perto da praia que os outros
    d.maxFwd = 9.0f; d.maxRev = 3.0f; d.accel = 2.6f; d.brake = 4.5f; d.drag = 0.32f; d.turnRate = 0.95f; d.rudderRate = 2.6f;   // leve: rápida e ágil
    d.muzzleA = 0.0f; d.muzzleB = 0.0f; d.rangeMin = 0.0f; d.rangeMax = 0.0f; d.hasCannons = false;
    d.camDist = 24.0f; d.camUp = 6.0f;
    d.wakeA = 0.47f; d.wakeB = kBoatB; d.clearR = 12.0f;
    d.maxHp = 100;                               // 4 balas
    return d;
}
// Mapeamento navio original -> BERGANTIM (igual a F, SB e SY de tools/export_brig.py): pontas iguais (S 1), meio do casco [-4, 5] vira [-7, 7], boca 0,95, altura 1.
constexpr float kBrigB = 0.95f;
constexpr float brigA(float a) { return a < -4.0f ? a - 3.0f : (a > 5.0f ? a + 2.0f : -7.0f + (a + 4.0f) * (14.0f / 9.0f)); }
constexpr ShipDef makeBrig() {
    ShipDef d = kShipSloop;
    d.name = "BERGANTIM";
    d.deckY = 1.03f; d.terraceY = 3.41f;         // tábuas do exportador (1,0125 / 3,39) + 2 cm
    d.aMin = brigA(kDeckAMin); d.aMax = brigA(kDeckAMax); d.midB = kShipMidB * kBrigB; d.wallA = brigA(-7.8f) + 0.35f;
    d.stairA0 = brigA(kStairA0); d.stairA1 = brigA(kStairA1);   // pontas com escala 1: os degraus são os do navio original, só deslocados
    d.stairB0 = kStairB0 * kBrigB; d.stairB1 = kStairB1 * kBrigB;
    d.terBackA = brigA(-11.02f); d.terBackHalfB = 3.12f * kBrigB; d.terMidA = brigA(-8.2f); d.terMidHalfB = 1.5f * kBrigB;
    d.hwMid = 2.85f * kBrigB; d.hwTaperA = brigA(5.4f); d.hwEnd = 2.0f * kBrigB;
    d.helmA = brigA(kHelmA); d.wheelA = brigA(kHelmWheelA); d.wheelY = d.terraceY + (kHelmWheelY - kTerraceY);
    d.chestHalfA = 0.35f; d.chestHalfB = 0.52f; d.chestA = brigA(-7.8f) + 0.05f + d.chestHalfA; d.chestB = d.midB;
    d.lenA0 = brigA(-14.0f); d.lenA1 = brigA(12.0f); d.hullHalfB = 4.0f * kBrigB; d.leaveA = 9.0f;
    d.nMasts = 2;
    d.masts[0][0] = 5.6f; d.masts[1][0] = -4.2f;   // MAST_FORE e MAST_MAIN do exportador
    d.masts[0][1] = d.masts[1][1] = d.midB; d.masts[0][2] = d.masts[1][2] = 0.62f;
    for (int i = 0; i < 9; ++i) { d.hull[i][0] = brigA(d.hull[i][0]); d.hull[i][1] *= kBrigB; }
    for (int i = 0; i < 12; ++i) { d.land[i][0] = brigA(d.land[i][0]); d.land[i][1] *= kBrigB; }
    d.draftH = -2.1f;
    d.maxFwd = 9.5f; d.maxRev = 2.8f; d.accel = 2.0f; d.brake = 3.6f; d.drag = 0.26f; d.turnRate = 0.62f; d.rudderRate = 2.0f;   // casco esguio: o mais rápido com canhões
    d.muzzleA = 6.5f; d.muzzleB = 3.3f;
    d.rangeMin = 14.0f; d.rangeMax = 125.0f;
    d.camDist = 58.0f; d.camUp = 11.0f; d.gunHeight = 155.0f;
    d.wakeA = 1.17f; d.wakeB = kBrigB; d.clearR = 26.0f;
    d.maxHp = 300;                               // 12 balas
    return d;
}
inline constexpr ShipDef kShipDefs[kMaxShips] = {kShipSloop, makeGalleon(), makeBoat(), makeBrig()};
inline const ShipDef& shipDef(int kind) { return kShipDefs[kind >= 0 && kind < kMaxShips ? kind : 0]; }

inline float shipDeckHalfW(const ShipDef& D, float a) {
    if (a <= D.hwTaperA) return D.hwMid;
    if (a >= D.aMax) return D.hwEnd;
    return D.hwMid - (D.hwMid - D.hwEnd) * (a - D.hwTaperA) / (D.aMax - D.hwTaperA);   // afunila na proa
}

// Distância (no plano) de (a, b) até a caixa do baú
inline float shipChestDist(const ShipDef& D, float a, float b) {
    float da = std::max(0.0f, std::fabs(a - D.chestA) - D.chestHalfA), db = std::max(0.0f, std::fabs(b - D.chestB) - D.chestHalfB);
    return std::sqrt(da * da + db * db);
}
// Tira o centro do jogador da caixa do baú (só existe no nível do convés: em cima fica o toldo) pelo lado mais perto
inline void shipChestPush(const ShipDef& D, float& a, float& b) {
    constexpr float r = 0.35f;
    float da = a - D.chestA, db = b - D.chestB, pa = D.chestHalfA + r - std::fabs(da), pb = D.chestHalfB + r - std::fabs(db);
    if (pa <= 0.0f || pb <= 0.0f) return;
    if (a < D.wallA || pa <= pb) a = D.chestA + D.chestHalfA + r;   // nunca empurra para a parede
    else b = D.chestB + (db >= 0.0f ? 1.0f : -1.0f) * (D.chestHalfB + r);
}

inline void shipPushCircle(float oa, float ob, float r, float& a, float& b) {
    float da = a - oa, db = b - ob, d2 = da * da + db * db;
    if (d2 >= r * r) return;
    float d = std::sqrt(d2);
    if (d < 1e-4f) { da = 0.0f; db = 1.0f; d = 1.0f; }
    a = oa + da / d * r; b = ob + db / d * r;
}

// Leva (a, b) para um ponto válido do convés principal (embarcar, voltar para o chão)
inline void shipClampDeck(const ShipDef& D, float& a, float& b) {
    for (int it = 0; it < 2; ++it) {
        a = std::max(D.aMin, std::min(D.aMax, a));
        float hw = shipDeckHalfW(D, a);
        b = std::max(-hw, std::min(hw, b));
        for (int i = 0; i < D.nMasts; ++i) shipPushCircle(D.masts[i][0], D.masts[i][1], D.masts[i][2], a, b);
    }
    a = std::max(D.aMin, std::min(D.aMax, a));
    if (std::fabs(b - D.midB) < D.stairB0) a = std::max(a, D.wallA);   // entre as escadas a parede vem antes
    float hw = shipDeckHalfW(D, a);
    b = std::max(-hw, std::min(hw, b));
    shipChestPush(D, a, b);
}

// Chão do navio em (a, b): convés principal, ESCADAS dos dois lados (rampa) e TOLDO da popa com o leme. kNoFloor = parede/fora do casco.
// Um passo para cima só vale se o degrau for <= 0,6 m (shipResolve): sem escada não se sobe ao toldo.
inline float shipFloor(const ShipDef& D, float a, float b) {
    const float ab = std::fabs(b), sb = std::fabs(b - D.midB);
    if (sb >= D.stairB0 && sb <= D.stairB1 && a <= D.stairA0 && a >= D.stairA1)
        return D.deckY + (D.terraceY - D.deckY) * (D.stairA0 - a) / (D.stairA0 - D.stairA1);
    if (a >= D.terBackA && a <= D.stairA1 && ab <= D.terBackHalfB) return D.terraceY;     // faixa de trás do toldo
    if (a > D.stairA1 && a <= D.terMidA && ab <= D.terMidHalfB) return D.terraceY;       // miolo do toldo (o timão fica aqui)
    if (sb < D.stairB0 && a < D.wallA - 0.02f) return kNoFloor;                           // parede sob o toldo, entre as escadas
    if (a >= D.aMin - 0.02f && a <= D.aMax + 0.02f && ab <= shipDeckHalfW(D, a) + 0.02f) return D.deckY; // convés principal (tolerância: o ponto passa por ida e volta mundo<->navio)
    return kNoFloor;
}
// Tira (a, b) de dentro de mastros/pedestal do timão/baú; true se o ponto resultante é chão alcançável a partir da altura curY.
inline bool shipResolve(const ShipDef& D, float& a, float& b, float curY) {
    for (int i = 0; i < D.nMasts; ++i) shipPushCircle(D.masts[i][0], D.masts[i][1], D.masts[i][2], a, b);
    shipPushCircle(D.wheelA, 0.0f, 0.62f * D.wheelScale, a, b);
    if (curY < D.deckY + 1.2f) shipChestPush(D, a, b);
    float f = shipFloor(D, a, b);
    return f > kNoFloor * 0.5f && f <= curY + 0.6f;
}

struct ShipState {
    bool exists = false;
    uint8_t kind = ShipSloop;      // tipo = índice em shipState(i)
    float x = 0, z = 0, yaw = 0;   // pose do casco (origem do modelo)
    float speed = 0;               // m/s ao longo da proa (negativo = ré)
    float steer = 0;               // leme atual -1..1 (+ = estibordo/direita)
    float cmdSteer = 0;            // leme pedido pelo timoneiro
    int8_t cmdThrottle = 0;        // 1 acelera, -1 ré, 0 solto
    int helmer = -1;               // id do jogador no leme (-1 = vago)
    int owner = -1;                // id do jogador DONO desta instância (quem chamou; -1 = sem dono: offline/testes). Só o dono assume o leme e abre o baú
    uint8_t gen = 0;               // incrementa a cada novo spawn (o cliente "teleporta" o desenho)
    int16_t hp = 0;                // vida do casco (0..ShipDef.maxHp); o servidor decide
    float sinkT = 0;               // 0 = boiando; > 0 = AFUNDANDO há sinkT s (some em kShipSinkTime; ninguém embarca)
};
// ---- Vida e naufrágio (servidor autoritativo) ----
constexpr int kShipShellDamage = 25;         // dano de uma bala que cai DENTRO do casco
constexpr float kShipHitMargin = 2.0f;       // a bala ainda pega até 2 m fora do casco (o dano cai até a metade na borda)
constexpr float kShipSinkTime = 8.0f;        // s afundando até sumir
constexpr float kShipCleanupSec = 60.0f;     // a cada tantos s o servidor some com os navios SEM NINGUÉM (leme, canhão, a bordo ou encostado no casco)
constexpr float kShipRespawnTime = 60.0f;    // s depois de sumir até renascer com vida cheia (servidor: --ship-respawn S)
inline bool shipAfloat(const ShipState& S) { return S.exists && S.sinkT <= 0.0f; }
// Pose do naufrágio (desenho): desce (m), aderna (roll, rad) e afunda de popa (pitch, rad) conforme o tempo afundando
inline void shipSinkPose(const ShipDef& D, float sinkT, float& dy, float& pitch, float& roll) {
    float k = std::max(0.0f, std::min(1.0f, sinkT / kShipSinkTime));
    dy = -(0.6f + 30.0f * D.wakeA) * k * k * (0.4f + 0.6f * k);
    roll = 0.55f * std::sqrt(k);
    pitch = 0.22f * k;   // + = popa para baixo
    if (sinkT <= 0.0f) dy = pitch = roll = 0.0f;
}
inline ShipState& shipState(int i) {
    static ShipState s[kMaxShips];
    s[i].kind = (uint8_t)i;
    return s[i];
}

inline void shipLocalToWorld(const ShipState& S, float a, float b, float& wx, float& wz) {
    float sy = std::sin(S.yaw), cy = std::cos(S.yaw);
    wx = S.x + sy * a + cy * b;
    wz = S.z - cy * a + sy * b;
}
inline void shipWorldToLocal(const ShipState& S, float wx, float wz, float& a, float& b) {
    float sy = std::sin(S.yaw), cy = std::cos(S.yaw), dx = wx - S.x, dz = wz - S.z;
    a = dx * sy - dz * cy;
    b = dx * cy + dz * sy;
}

// Meia-boca do casco (m) na linha d'água em cada a
inline float shipHullHalfB(const ShipDef& D, float a) {
    const auto& T = D.hull;
    if (a <= T[0][0]) return T[0][1];
    for (int i = 1; i < 9; ++i) if (a <= T[i][0]) return T[i - 1][1] + (T[i][1] - T[i - 1][1]) * (a - T[i - 1][0]) / (T[i][0] - T[i - 1][0]);
    return 0.0f;
}
// Dano de uma bala que caiu em (wx, wz) no navio S: kShipShellDamage dentro do casco, cai até a metade a kShipHitMargin m dele; 0 = errou (ou já afundando)
inline int shipShellDamage(const ShipState& S, float wx, float wz) {
    if (!shipAfloat(S)) return 0;
    const ShipDef& D = shipDef(S.kind);
    float a, b; shipWorldToLocal(S, wx, wz, a, b);
    float da = std::max(0.0f, std::max(D.hull[0][0] - a, a - D.hull[8][0]));
    float hb = shipHullHalfB(D, std::max(D.hull[0][0], std::min(D.hull[8][0], a)));
    float db = std::max(0.0f, std::fabs(b) - hb), d = std::sqrt(da * da + db * db);
    if (d > kShipHitMargin) return 0;
    return std::max(1, (int)std::lround(kShipShellDamage * (1.0f - 0.5f * d / kShipHitMargin)));
}
// Empurra um ponto do mundo (centro de um jogador de raio r) para FORA do casco pelo caminho mais curto. true se mexeu.
inline bool shipHullPush(const ShipState& S, float& wx, float& wz, float r) {
    if (!shipAfloat(S)) return false;
    const ShipDef& D = shipDef(S.kind);
    float a, b; shipWorldToLocal(S, wx, wz, a, b);
    const float a0 = D.hull[0][0] - r, a1 = D.hull[8][0] + r;
    if (a < a0 || a > a1) return false;
    float hw = shipHullHalfB(D, std::max(D.hull[0][0], std::min(D.hull[8][0], a))) + r;
    if (std::fabs(b) >= hw) return false;
    float dB = hw - std::fabs(b), dA0 = a - a0, dA1 = a1 - a;
    if (dB <= dA0 && dB <= dA1) b = (b >= 0.0f ? 1.0f : -1.0f) * hw;
    else if (dA0 < dA1) a = a0;
    else a = a1;
    shipLocalToWorld(S, a, b, wx, wz);
    return true;
}
inline void shipHullPushAll(float& wx, float& wz, float r) { for (int i = 0; i < kMaxShips; ++i) shipHullPush(shipState(i), wx, wz, r); }

// Pontos do casco (a, b) testados contra a terra: proa, popa e laterais
inline bool shipHullHitsLand(const ShipDef& D, const Terrain* W, float x, float z, float yaw) {
    if (!W) return false;
    const float sy = std::sin(yaw), cy = std::cos(yaw), lim = Terrain::limit() - 6.0f;
    for (const auto& q : D.land) {
        float wx = x + sy * q[0] + cy * q[1], wz = z - cy * q[0] + sy * q[1];
        if (std::fabs(wx) > lim || std::fabs(wz) > lim) return true;
        if (W->heightAt(wx, wz) > D.draftH) return true;
    }
    return false;
}
inline bool shipPointInHull(const ShipState& O, float wx, float wz) {
    const ShipDef& D = shipDef(O.kind);
    float a, b; shipWorldToLocal(O, wx, wz, a, b);
    return a >= D.hull[0][0] && a <= D.hull[8][0] && std::fabs(b) < shipHullHalfB(D, a);
}
// O casco de S na pose (x, z, yaw) entraria no outro navio? (os pontos de cada um contra o casco do outro)
inline bool shipHitsShip(const ShipState& S, float x, float z, float yaw) {
    const ShipDef& D = shipDef(S.kind);
    ShipState me = S; me.x = x; me.z = z; me.yaw = yaw;
    for (int i = 0; i < kMaxShips; ++i) {
        const ShipState& O = shipState(i);
        if (i == S.kind || !shipAfloat(O)) continue;   // o que está afundando não bloqueia ninguém
        const ShipDef& DO = shipDef(O.kind);
        for (const auto& q : D.land) { float wx, wz; shipLocalToWorld(me, q[0], q[1], wx, wz); if (shipPointInHull(O, wx, wz)) return true; }
        for (const auto& q : DO.land) { float wx, wz; shipLocalToWorld(O, q[0], q[1], wx, wz); if (shipPointInHull(me, wx, wz)) return true; }
    }
    return false;
}

inline float shipWrap(float a) {
    const float pi = 3.14159265358979f;
    a = std::fmod(a + pi, 2.0f * pi);
    if (a < 0) a += 2.0f * pi;
    return a - pi;
}

// Dinâmica do navio. Chamada pelo servidor a cada tick e pelo cliente a cada frame (predição).
inline void stepShip(ShipState& S, float dt, const Terrain* W) {
    if (!S.exists || !(dt > 0.0f)) return;
    const ShipDef& D = shipDef(S.kind);
    dt = std::min(dt, 0.1f);
    if (S.sinkT > 0.0f) {   // AFUNDANDO: sem leme nem acelerador, perde a velocidade e vai descendo (o desenho usa shipSinkPose)
        S.sinkT += dt; S.cmdSteer = 0.0f; S.cmdThrottle = 0; S.helmer = -1;
        S.speed *= std::exp(-0.7f * dt); S.steer *= std::exp(-1.5f * dt);
        S.yaw = shipWrap(S.yaw + S.steer * D.turnRate * 0.3f * dt);
        S.x += std::sin(S.yaw) * S.speed * dt; S.z -= std::cos(S.yaw) * S.speed * dt;
        return;
    }
    float dr = S.cmdSteer - S.steer, mr = D.rudderRate * dt;
    S.steer += std::max(-mr, std::min(mr, dr));
    if (S.cmdThrottle > 0) S.speed += (S.speed < 0 ? D.brake : D.accel) * dt;
    else if (S.cmdThrottle < 0) S.speed -= (S.speed > 0 ? D.brake : D.accel) * dt;
    else {
        S.speed *= std::exp(-D.drag * dt);
        float dec = 0.10f * dt;
        S.speed = std::fabs(S.speed) <= dec ? 0.0f : S.speed - (S.speed > 0 ? dec : -dec);
    }
    S.speed = std::max(-D.maxRev, std::min(D.maxFwd, S.speed));
    float turn = S.steer * D.turnRate * std::min(1.0f, std::fabs(S.speed) / 2.5f) * (S.speed >= 0 ? 1.0f : -1.0f);
    float ny = shipWrap(S.yaw + turn * dt);
    float nx = S.x + std::sin(ny) * S.speed * dt, nz = S.z - std::cos(ny) * S.speed * dt;
    auto blocked = [&](float x, float z, float yaw) { return shipHullHitsLand(D, W, x, z, yaw) || shipHitsShip(S, x, z, yaw); };
    if (!blocked(nx, nz, ny)) { S.x = nx; S.z = nz; S.yaw = ny; }
    else if (std::fabs(shipWrap(ny - S.yaw)) > 1e-6f && !blocked(S.x, S.z, ny)) { S.yaw = ny; S.speed *= 0.6f; }   // encostou: gira no lugar
    else S.speed = 0.0f;                                                              // encalhou: para (a ré tira o navio)
}

// O ponto (x, z) com a proa para `yaw` serve para o navio `kind` nascer? Casco todo e folga em volta em água funda, longe de terra e dos outros navios.
template <class T>
inline bool shipSpotOk(int kind, const T& W, float x, float z, float yaw) {
    const float pi = 3.14159265358979f;
    const ShipDef& D = shipDef(kind);
    const float deep = D.draftH - 0.3f;
    const float sy = std::sin(yaw), cy = std::cos(yaw), lim = Terrain::limit() - 14.0f;
    for (const auto& q : D.land) {
        float wx = x + sy * q[0] + cy * q[1] * 1.25f, wz = z - cy * q[0] + sy * q[1] * 1.25f;
        if (std::fabs(wx) > lim || std::fabs(wz) > lim || W.heightAt(wx, wz) > deep) return false;
    }
    // mar aberto: folga em volta do centro (nada de baía apertada: o navio precisa de espaço para manobrar)
    for (int j = 0; j < 8; ++j) {
        float aj = (float)j * (pi * 0.25f), wx = x + std::sin(aj) * D.clearR, wz = z - std::cos(aj) * D.clearR;
        if (std::fabs(wx) > lim || std::fabs(wz) > lim || W.heightAt(wx, wz) > deep) return false;
    }
    for (int i = 0; i < kMaxShips; ++i) {
        const ShipState& O = shipState(i);
        if (i != kind && O.exists && std::hypot(O.x - x, O.z - z) < 75.0f) return false;
    }
    return true;
}

// Acha água funda e ABERTA para o navio perto de (nx, nz): anéis crescentes, de 16 direções; o casco todo (e a folga em volta) precisa estar em fundo mais baixo que o calado.
// O casco fica de lado para quem pediu (tangente ao anel de busca). Fica longe do outro navio.
template <class T>
inline bool shipFindSpawn(int kind, const T& W, float nx, float nz, float minR, float& ox, float& oz, float& oyaw) {
    const float pi = 3.14159265358979f;
    for (float r = minR; r <= 700.0f; r += 6.0f) {
        for (int k = 0; k < 16; ++k) {
            float ang = (float)k * (2.0f * pi / 16.0f);
            float x = nx + std::sin(ang) * r, z = nz - std::cos(ang) * r;
            float yaw = shipWrap(ang + pi * 0.5f);   // casco tangente ao anel (de lado para quem pediu)
            if (shipSpotOk(kind, W, x, z, yaw)) { ox = x; oz = z; oyaw = yaw; return true; }
        }
    }
    return false;
}

// CHAMAR O NAVIO (tela NAVEGAÇÃO): acha o mar aberto mais perto À FRENTE de (px, pz) olhando para `heading` (mesma convenção do yaw: frente = (sin, -cos)).
// Procura de kSummonMinDist a kSummonMaxDist m em passadas de 6 m, com desvios laterais de 0/±10/±20 m; o casco fica de lado (proa para a direita de quem chamou).
constexpr float kSummonMinDist = 30.0f, kSummonMaxDist = 420.0f;
template <class T>
inline bool shipFindAhead(int kind, const T& W, float px, float pz, float heading, float& ox, float& oz, float& oyaw) {
    const float pi = 3.14159265358979f, fx = std::sin(heading), fz = -std::cos(heading), rx = std::cos(heading), rz = std::sin(heading);
    static const float kLat[5] = {0.0f, 10.0f, -10.0f, 20.0f, -20.0f};
    for (float d = kSummonMinDist; d <= kSummonMaxDist; d += 6.0f) {
        for (float lat : kLat) {
            const float x = px + fx * d + rx * lat, z = pz + fz * d + rz * lat, yaw = shipWrap(heading + pi * 0.5f);
            if (shipSpotOk(kind, W, x, z, yaw)) { ox = x; oz = z; oyaw = yaw; return true; }
        }
    }
    return false;
}

}  // namespace e0net
