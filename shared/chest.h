// Isolated - BAÚ DOS NAVIOS: conteúdo e regras (header-only, usado pelo servidor, pelo cliente offline e pelos testes).
// Cada navio tem 1 baú de kChestSlots lugares, compartilhado por todos que estão a bordo. O servidor é a autoridade; o conteúdo vive só na memória dele.
#pragma once
#include <algorithm>
#include <cstdint>

namespace e0net {

constexpr int kChestSlots = 24;
constexpr int kItemCount = 22;   // ids de item 1..17 (ver Engine::ItemDef em engine.cpp; 16 = BACAMARTE, 17 = MUNICAO)

// Pilha máxima por item. ESPELHA Engine::itemDef(id).maxStack (o cliente confere isto ao iniciar e avisa no log se divergir).
inline uint16_t itemMaxStack(int id) {
    static const uint16_t k[kItemCount] = {1, 50, 50, 20, 20, 10, 30, 1, 1, 30, 20, 1, 1, 20, 20, 30, 1, 30, 10, 10, 10, 10};
    return id > 0 && id < kItemCount ? k[id] : 0;
}

struct ChestSlot { uint8_t id = 0; uint16_t n = 0; };
struct Chest {
    ChestSlot s[kChestSlots];
    uint16_t ver = 0;   // sobe a cada mudança; o cliente ignora sincronizações mais antigas
};

// Guarda até n do item (completa pilhas, depois slots vazios). Devolve quantos coube.
inline int chestPut(Chest& c, int id, int n) {
    const int mx = itemMaxStack(id);
    if (mx <= 0 || n <= 0) return 0;
    int left = n;
    for (auto& s : c.s) if (s.id == id && s.n < mx && left > 0) { int k = std::min(left, mx - (int)s.n); s.n += (uint16_t)k; left -= k; }
    for (auto& s : c.s) if (!s.id && left > 0) { int k = std::min(left, mx); s = {(uint8_t)id, (uint16_t)k}; left -= k; }
    if (left != n) ++c.ver;
    return n - left;
}
// Tira até n do slot. Devolve quantos saíram (0 = slot vazio/inválido); id recebe o item.
inline int chestTake(Chest& c, int slot, int n, uint8_t& id) {
    if (slot < 0 || slot >= kChestSlots || n <= 0 || !c.s[slot].id) return 0;
    ChestSlot& s = c.s[slot];
    int k = std::min(n, (int)s.n);
    id = s.id; s.n -= (uint16_t)k;
    if (!s.n) s = {};
    ++c.ver;
    return k;
}
inline int chestCount(const Chest& c) { int k = 0; for (const auto& s : c.s) if (s.id) ++k; return k; }

}  // namespace e0net
