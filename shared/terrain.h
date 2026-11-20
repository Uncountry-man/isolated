// Isolated - OCEANO ABERTO: layout das ilhas e "Terreno" global (header-only).
//
//  * O mar é aberto: um quadrado de ±4,95 km dividido em setores de 1100 m (9x9). O setor (0,0) é a ILHA INICIAL (a do arquivo do mundo, semente salva).
//    Cada outro setor tem, deterministicamente (hash inteiro da semente do mundo + coordenadas do setor): nada (~45%), uma ILHA PEQUENA (1 ou poucos biomas e
//    recursos específicos, ver IslandKind) ou, raramente (~2%), uma ILHA GRANDE completa como a inicial. Os 8 setores em volta da ilha inicial sempre têm uma ilha pequena.
//  * Cada ilha é um `World` (grade de 640 m) com ORIGEM deslocada no mundo; objetos e consultas usam coordenadas globais. As ilhas NÃO vão em arquivo: o servidor e
//    os clientes as geram da semente (só inteiros => idênticas em qualquer CPU). O cliente gera sob demanda, perto do jogador.
//  * `Terrain` junta tudo para a simulação compartilhada (stepPlayer, navio): altura/bioma/colisão em qualquer ponto; fora das ilhas é mar fundo.
//  * Id global de objeto (gid) = (slot << 20) | índice na ilha. O slot 0 é a ilha inicial, então o gid dela é o próprio índice (compatível com o que já existia).
#pragma once
#include <memory>
#include <cstdint>

#include "world.h"

namespace e0net {

constexpr int kOceanN = 4;                                   // setores de -4 a +4 em cada eixo
constexpr int kOceanSide = 2 * kOceanN + 1;                  // 9
constexpr int kIslandSlots = kOceanSide * kOceanSide;        // 81 (slot 0 = ilha inicial)
constexpr float kSectorM = 1100.0f;
constexpr int32_t kSectorCm = 110000;
constexpr float kOceanHalf = kSectorM * (kOceanN + 0.5f);    // 4950 m: limite do mar navegável
constexpr float kDeepSeaH = -40.0f;                          // altura do fundo fora das ilhas (m)
constexpr uint32_t kOceanLayoutVersion = 1;                  // mudar o layout => incrementar (o servidor e o app precisam ser recompilados juntos)

inline uint32_t oceanHash(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

constexpr int kMainSector = kOceanN * kOceanSide + kOceanN;  // índice linear do setor (0,0)
inline int slotOfSector(int sx, int sz) {                    // -> slot 0..80 (setor inicial = 0), -1 fora do mar
    if (sx < -kOceanN || sx > kOceanN || sz < -kOceanN || sz > kOceanN) return -1;
    int lin = (sx + kOceanN) * kOceanSide + (sz + kOceanN);
    return lin == kMainSector ? 0 : (lin < kMainSector ? lin + 1 : lin);
}
inline void sectorOfSlot(int slot, int& sx, int& sz) {
    int lin = slot == 0 ? kMainSector : (slot <= kMainSector ? slot - 1 : slot);
    sx = lin / kOceanSide - kOceanN; sz = lin % kOceanSide - kOceanN;
}

struct IslandSpec {
    bool exists = false;
    int slot = -1, sx = 0, sz = 0;
    uint8_t kind = IslandBig;
    uint32_t seed = 0;
    int32_t oxcm = 0, ozcm = 0;    // origem (centro da grade da ilha) em cm
};

// Layout do oceano: puro (só inteiros). `mainSeed` = semente da ilha inicial.
inline IslandSpec islandSpec(uint32_t mainSeed, int sx, int sz) {
    IslandSpec s; s.sx = sx; s.sz = sz; s.slot = slotOfSector(sx, sz);
    if (s.slot < 0) return s;
    if (s.slot == 0) { s.exists = true; s.kind = IslandBig; s.seed = mainSeed; return s; }
    uint32_t h = oceanHash(mainSeed ^ 0x0CEA11u, (uint32_t)(sx + 100), (uint32_t)(sz + 100));
    const bool near = sx >= -1 && sx <= 1 && sz >= -1 && sz <= 1;
    uint32_t roll = h % 100u;
    bool big = !near && roll < 2, small = near || (roll >= 2 && roll < 52);
    if (!big && !small) return s;
    s.exists = true;
    s.kind = big ? (uint8_t)IslandBig : (uint8_t)(1 + (h >> 8) % (IslandKindCount - 1));
    s.seed = oceanHash(h, 0xBEEFu, (uint32_t)s.slot);
    s.oxcm = sx * kSectorCm + (int32_t)((h >> 12) % 36001u) - 18000;
    s.ozcm = sz * kSectorCm + (int32_t)(oceanHash(h, 0x77u, 0u) % 36001u) - 18000;
    return s;
}

inline const char* islandKindName(uint8_t k) {
    static const char* n[IslandKindCount] = {"ILHA GRANDE", "ATOL", "ILHOTA DE PRAIA", "ILHOTA DE FLORESTA", "AFLORAMENTO DE PEDRA", "CAMPINA", "RUINAS"};
    return n[k < IslandKindCount ? k : 0];
}

// gid <-> (slot, índice)
inline uint32_t makeGid(int slot, uint32_t idx) { return ((uint32_t)slot << 20) | idx; }
inline int gidSlot(uint32_t gid) { return (int)(gid >> 20); }
inline uint32_t gidIdx(uint32_t gid) { return gid & 0xFFFFFu; }

class Terrain {
public:
    void setMain(const World* w) {
        uint32_t sd = w ? w->seed : 0;
        if (sd != mainSeed_) for (int i = 1; i < kIslandSlots; ++i) isl_[i] = nullptr;   // outra semente de mundo => outro oceano
        isl_[0] = w; mainSeed_ = sd;
    }
    void clearIslands() { for (int i = 1; i < kIslandSlots; ++i) isl_[i] = nullptr; }
    void setIsland(int slot, const World* w) { if (slot > 0 && slot < kIslandSlots) isl_[slot] = w; }
    const World* world(int slot) const { return slot >= 0 && slot < kIslandSlots ? isl_[slot] : nullptr; }
    const World* mainWorld() const { return isl_[0]; }
    uint32_t mainSeed() const { return mainSeed_; }
    bool hasMain() const { return isl_[0] != nullptr; }

    // slot da ilha cuja grade contém (x, z) (ou -1). Setores de 1100 m com ilhas de grade de 640 m + jitter de 180 m nunca se sobrepõem.
    int slotAt(float x, float z) const {
        int sx = (int)std::floor(x / kSectorM + 0.5f), sz = (int)std::floor(z / kSectorM + 0.5f);
        int slot = slotOfSector(sx, sz);
        if (slot < 0) return -1;
        const World* w = isl_[slot];
        if (!w) return -1;
        float lx = x - w->oxcm * 0.01f, lz = z - w->ozcm * 0.01f;
        return (lx > -World::kHalf && lx < World::kHalf && lz > -World::kHalf && lz < World::kHalf) ? slot : -1;
    }
    float heightAt(float x, float z) const { int s = slotAt(x, z); return s < 0 ? kDeepSeaH : isl_[s]->heightAt(x, z); }
    uint8_t biomeAt(float x, float z) const { int s = slotAt(x, z); return s < 0 ? (uint8_t)BiomeSeaFloor : isl_[s]->biomeAt(x, z); }
    bool blocksShot(float x, float y, float z) const { int s = slotAt(x, z); return s >= 0 && isl_[s]->blocksShot(x, y, z); }
    void pushOut(float& x, float& z, float r, float feetY = -1e9f) const { int s = slotAt(x, z); if (s >= 0) isl_[s]->pushOut(x, z, r, feetY); }
    static constexpr float limit() { return kOceanHalf - 10.0f; }   // limite de movimento (m)

private:
    const World* isl_[kIslandSlots] = {};
    uint32_t mainSeed_ = 0;
};

inline Terrain& terrain() { static Terrain t; return t; }

// Cópia IMUTÁVEL das ilhas para outra thread (ex.: MapWorker do mapa): segura as ilhas vivas (shared_ptr) enquanto a thread
// principal as troca/descarrega. Mesma consulta de altura do Terrain.
struct TerrainSnapshot {
    std::shared_ptr<const World> isl[kIslandSlots];
    bool hasMain() const { return isl[0] != nullptr; }
    float heightAt(float x, float z) const {
        int sx = (int)std::floor(x / kSectorM + 0.5f), sz = (int)std::floor(z / kSectorM + 0.5f);
        int slot = slotOfSector(sx, sz);
        if (slot < 0 || !isl[slot]) return kDeepSeaH;
        const World* w = isl[slot].get();
        float lx = x - w->oxcm * 0.01f, lz = z - w->ozcm * 0.01f;
        if (!(lx > -World::kHalf && lx < World::kHalf && lz > -World::kHalf && lz < World::kHalf)) return kDeepSeaH;
        return w->heightAt(x, z);
    }
    uint8_t biomeAt(float x, float z) const {
        int sx = (int)std::floor(x / kSectorM + 0.5f), sz = (int)std::floor(z / kSectorM + 0.5f);
        int slot = slotOfSector(sx, sz);
        if (slot < 0 || !isl[slot]) return (uint8_t)BiomeSeaFloor;
        const World* w = isl[slot].get();
        float lx = x - w->oxcm * 0.01f, lz = z - w->ozcm * 0.01f;
        if (!(lx > -World::kHalf && lx < World::kHalf && lz > -World::kHalf && lz < World::kHalf)) return (uint8_t)BiomeSeaFloor;
        return w->biomeAt(x, z);
    }
};

}  // namespace e0net
