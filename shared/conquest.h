// Isolated - DOMINAÇÃO DE ILHAS (coop entre tripulantes; header-only, compartilhado servidor/cliente).
//
//  * Cada ilha do OCEANO ABERTO (slot >= 1; a ilha inicial é porto livre e não pode ser dominada) tem um MASTRO num ponto seco e plano
//    perto do centro (conquestPoint: busca só com inteiros => o mesmo ponto no servidor e em qualquer celular).
//  * Quem domina é a TRIPULAÇÃO: tripulantes vivos, a pé, a até kCapRadius do mastro. Jogador sem tripulação não conta.
//  * O mastro tem UMA bandeira (poleTeam) a uma altura 0..1 (prog). A bandeira só sobe para a tripulação que está sozinha na área;
//    a de outra tripulação precisa ser ARRIADA primeiro (prog -> 0). Ilha LIVRE: sobe direto (sem luta). Ilha DOMINADA com defensores
//    (tripulantes da dona) na área = CONTESTADA (nada anda): os atacantes precisam vencê-los. Duas tripulações atacando = contestada.
//  * Sem atacantes, o mastro volta sozinho ao normal (bandeira da dona no alto / ilha livre sem bandeira).
//  * Ilha dominada rende moedas para o COFRE da tripulação a cada kIslandPayoutSec. Dominação vale até alguém tomar (salva no servidor).
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "terrain.h"

namespace e0net {

constexpr float kCapRadius = 9.0f;          // m (horizontal) do mastro: área de dominação
constexpr float kCapMaxDy = 5.0f;           // diferença de altura máxima (m)
constexpr float kCapRaiseSec = 40.0f;       // 1 tripulante: hastear a bandeira (0 -> 100%)
constexpr float kCapLowerSec = 30.0f;       // 1 tripulante: arriar a bandeira de outra tripulação (100% -> 0)
constexpr float kCapDecaySec = 60.0f;       // ninguém na área: o mastro volta ao normal nesse tempo
constexpr float kCapShieldSec = 300.0f;     // recém-dominada: protegida por 5 min
constexpr int kIslandPayoutSec = 1200;      // renda a cada 20 min (relógio real; continua contando com o servidor desligado)
constexpr float kCapNearRadius = 260.0f;    // o servidor manda o estado do mastro (MsgIslandCap) a quem está até aqui
constexpr int kCapMaxCount = 3;             // 3+ tripulantes = velocidade máxima
inline float capSpeedMul(int n) { return n <= 1 ? 1.0f : std::min(1.0f + 0.5f * (float)(std::min(n, kCapMaxCount) - 1), 2.0f); }
inline bool islandConquerable(int slot) { return slot >= 1 && slot < kIslandSlots; }
inline int islandIncome(uint8_t kind) { return kind == IslandBig ? 200 : 60; }   // moedas por pagamento

enum CapState : uint8_t {
    CapIdle = 0,        // ninguém disputando
    CapRaising = 1,     // hasteando a bandeira de uma tripulação
    CapLowering = 2,    // arriando a bandeira de outra tripulação
    CapContested = 3,   // tripulações diferentes na área: parado
    CapRestoring = 4,   // voltando ao normal (sem atacantes)
    CapShielded = 5,    // recém-dominada: ninguém toma
};
enum IslandEventKind : uint8_t { IeCaptured = 1, IeIncome = 2, IeUnderAttack = 3, IeLost = 4 };

// Nome curto de uma ilha para avisos ("ATOL 12"): o tipo + o número do slot (único no mar).
inline std::string islandLabel(int slot, uint8_t kind) { return std::string(islandKindName(kind)) + " " + std::to_string(slot); }

// Ponto do mastro (cm, coordenadas GLOBAIS): o vértice seco e plano mais perto do centro da grade da ilha, longe de obstáculos.
// Só inteiros (mesmo resultado em qualquer CPU). false = ilha sem lugar (não dominável).
inline bool conquestPoint(const World& w, int32_t& xcm, int32_t& zcm) {
    const int N = World::kVerts, C = World::kCells / 2;
    std::vector<int32_t> obs;   // obstáculos (x, z, raio em cm, locais)
    for (const WorldObject& o : w.objects) {
        if (World::isCollectible(o.type)) continue;
        obs.push_back(o.xcm - w.oxcm); obs.push_back(o.zcm - w.ozcm);
        obs.push_back(World::isBuilding(o.type) ? 1400 : (o.type == ObjLog ? 450 : 320));
    }
    auto H = [&](int ix, int iz) { return (int32_t)w.hcm[(size_t)iz * N + ix]; };
    for (int pass = 0; pass < 2; ++pass) {
        for (int r = 0; r < C - 3; ++r) {
            int64_t best = -1; int bx = 0, bz = 0;
            for (int iz = C - r; iz <= C + r; ++iz)
                for (int ix = C - r; ix <= C + r; ix += (iz == C - r || iz == C + r || r == 0) ? 1 : 2 * r) {   // só o contorno do anel
                    if (ix < 2 || iz < 2 || ix > N - 3 || iz > N - 3) continue;
                    const int32_t h = H(ix, iz);
                    if (h < 80 || h > 2600) continue;
                    bool ok = true;
                    for (int dz = -1; dz <= 1 && ok; ++dz)
                        for (int dx = -1; dx <= 1 && ok; ++dx) if (std::abs(H(ix + dx, iz + dz) - h) > (pass == 0 ? 70 : 140) || H(ix + dx, iz + dz) < 40) ok = false;
                    const uint8_t b = w.biome[(size_t)iz * N + ix];
                    if (!ok || b == BiomeSeaFloor || b == BiomePad || (pass == 0 && b == BiomeRock)) continue;
                    const int32_t x = ix * 200 - World::kHalfCm, z = iz * 200 - World::kHalfCm;
                    for (size_t k = 0; k < obs.size() && ok; k += 3) {
                        const int64_t dx = obs[k] - x, dz = obs[k + 1] - z, rr = obs[k + 2] + 250;
                        if (dx * dx + dz * dz < rr * rr) ok = false;
                    }
                    if (!ok) continue;
                    const int64_t d2 = (int64_t)x * x + (int64_t)z * z;
                    if (best < 0 || d2 < best) { best = d2; bx = x; bz = z; }
                }
            if (best >= 0) { xcm = bx + w.oxcm; zcm = bz + w.ozcm; return true; }
        }
    }
    return false;
}

}  // namespace e0net
