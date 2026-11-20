// Isolated - INVENTÁRIO compartilhado (header-only): regras do inventário do jogador usadas pelo SERVIDOR (autoridade, persistência) e pelo
// CLIENTE (predição local / modo offline). Mesmo código dos dois lados => o layout dos slots fica idêntico e a ressincronização é rara.
// 30 slots: 0..24 = mochila, 25..29 = barra de acesso rápido (hotbar). Peso máximo kInvMaxKg. Ids dos itens = Engine::ItemDef (engine.cpp).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "chest.h"

namespace e0net {

constexpr int kInvSlots = 30, kInvHotFirst = 25;
constexpr float kInvMaxKg = 40.0f;
// Ids dos itens (ESPELHA o enum de Engine; o cliente confere pilha e peso ao iniciar)
namespace item {
enum : uint8_t { None = 0, Stick, Pebble, Wood, Stone, Coconut, Frond, Hatchet, Pickaxe, Gold, Diamond, Torch, Dagger, Rope, Cloth, Iron, Gun, Ammo, PotHeal, PotFull, PotSpeed, PotShield };
}
static_assert(item::PotShield + 1 == kItemCount, "kItemCount");

// Peso (kg) por unidade. ESPELHA Engine::itemDef(id).kg.
inline float itemKg(int id) {
    static const float k[kItemCount] = {0.0f, 0.05f, 0.10f, 1.50f, 2.00f, 0.60f, 0.10f, 1.20f, 2.00f, 0.40f, 0.25f, 0.60f, 0.50f, 0.20f, 0.10f, 0.80f, 3.50f, 0.05f, 0.30f, 0.30f, 0.30f, 0.30f};
    return id > 0 && id < kItemCount ? k[id] : 0.0f;
}
inline bool itemValid(int id) { return id > 0 && id < kItemCount; }
inline bool itemConsumable(int id) { return id == item::Coconut || (id >= item::PotHeal && id <= item::PotShield); }

struct InvSlot { uint8_t id = 0; uint16_t n = 0; };
inline bool operator==(const InvSlot& a, const InvSlot& b) { return a.id == b.id && a.n == b.n; }
inline bool operator!=(const InvSlot& a, const InvSlot& b) { return !(a == b); }

// ---- operações (s = array de kInvSlots) ----
inline float invWeight(const InvSlot* s) { float t = 0; for (int i = 0; i < kInvSlots; ++i) t += itemKg(s[i].id) * s[i].n; return t; }
inline int invCount(const InvSlot* s, int id) { int c = 0; for (int i = 0; i < kInvSlots; ++i) if (s[i].id == id) c += s[i].n; return c; }
// Cabe mais n do item? (pilhas incompletas ou slot vazio + peso)
inline bool invFits(const InvSlot* s, int id, int n) {
    if (!itemValid(id) || n <= 0) return n == 0;
    const int mx = itemMaxStack(id); int room = 0;
    for (int i = 0; i < kInvSlots; ++i) { if (s[i].id == id) room += mx - s[i].n; else if (!s[i].id) room += mx; }
    return room >= n && invWeight(s) + itemKg(id) * n <= kInvMaxKg + 1e-3f;
}
// Adiciona (completa pilhas, depois slots vazios em ordem). Devolve o que NÃO coube (não olha o peso: use invFits antes).
inline int invAdd(InvSlot* s, int id, int n) {
    if (!itemValid(id) || n <= 0) return n < 0 ? 0 : n;
    const int mx = itemMaxStack(id);
    for (int i = 0; i < kInvSlots && n > 0; ++i) if (s[i].id == id && s[i].n < mx) { int k = std::min(n, mx - (int)s[i].n); s[i].n = (uint16_t)(s[i].n + k); n -= k; }
    for (int i = 0; i < kInvSlots && n > 0; ++i) if (!s[i].id) { int k = std::min(n, mx); s[i].id = (uint8_t)id; s[i].n = (uint16_t)k; n -= k; }
    return n;
}
// Ferramentas (pilha 1) vão primeiro para a barra de acesso rápido (como pegar do baú). Devolve o que não coube.
inline int invAddPreferHot(InvSlot* s, int id, int n) {
    if (itemValid(id) && itemMaxStack(id) == 1)
        for (int i = kInvHotFirst; i < kInvSlots && n > 0; ++i) if (!s[i].id) { s[i].id = (uint8_t)id; s[i].n = 1; --n; }
    return invAdd(s, id, n);
}
// Quanto do item cabe (peso + espaço), até n.
inline int invRoomFor(const InvSlot* s, int id, int n) { while (n > 0 && !invFits(s, id, n)) --n; return n; }
// Tira n do item: mochila primeiro (0..24), depois o acesso rápido. Devolve quanto tirou.
inline int invRemove(InvSlot* s, int id, int n) {
    int took = 0;
    for (int i = 0; i < kInvSlots && n > 0; ++i) if (s[i].id == id) { int k = std::min(n, (int)s[i].n); s[i].n = (uint16_t)(s[i].n - k); n -= k; took += k; if (!s[i].n) s[i] = {}; }
    return took;
}
// Tira n do item de um slot específico (slot -1 = de qualquer lugar). Só tira se houver tudo. true = tirou.
inline bool invTake(InvSlot* s, int slot, int id, int n) {
    if (!itemValid(id) || n <= 0) return false;
    if (slot >= 0 && slot < kInvSlots && s[slot].id == id && s[slot].n >= n) { s[slot].n = (uint16_t)(s[slot].n - n); if (!s[slot].n) s[slot] = {}; return true; }
    if (invCount(s, id) < n) return false;
    invRemove(s, id, n); return true;
}
// Arrastar/mover: vazio = move; mesmo item = junta (o resto fica); outro item = troca.
inline void invMove(InvSlot* s, int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= kInvSlots || to >= kInvSlots || !s[from].id) return;
    InvSlot &a = s[from], &b = s[to];
    if (!b.id) { b = a; a = {}; }
    else if (b.id == a.id) {
        int k = std::min((int)a.n, (int)itemMaxStack(a.id) - (int)b.n); if (k < 0) k = 0;
        b.n = (uint16_t)(b.n + k); a.n = (uint16_t)(a.n - k);
        if (!a.n) a = {};
    } else std::swap(a, b);
}
// Dividir: metade da pilha vai para o slot vazio `to` (to < 0 = primeiro vazio). true = dividiu.
inline bool invSplit(InvSlot* s, int from, int to) {
    if (from < 0 || from >= kInvSlots || s[from].n < 2) return false;
    if (to < 0) for (int i = 0; i < kInvSlots; ++i) if (!s[i].id) { to = i; break; }
    if (to < 0 || to >= kInvSlots || s[to].id) return false;
    const uint16_t half = (uint16_t)(s[from].n / 2);
    s[to].id = s[from].id; s[to].n = half; s[from].n = (uint16_t)(s[from].n - half);
    return true;
}
// Organizar: só a mochila (0..24): junta pilhas e ordena por tipo.
inline void invSort(InvSlot* s) {
    int cnt[kItemCount] = {};
    for (int i = 0; i < kInvHotFirst; ++i) if (itemValid(s[i].id)) cnt[s[i].id] += s[i].n;
    InvSlot tmp[kInvHotFirst]; int o = 0;
    for (int id = 1; id < kItemCount; ++id)
        for (int n = cnt[id]; n > 0 && o < kInvHotFirst;) { int k = std::min(n, (int)itemMaxStack(id)); tmp[o].id = (uint8_t)id; tmp[o].n = (uint16_t)k; ++o; n -= k; }
    for (int i = 0; i < kInvHotFirst; ++i) s[i] = i < o ? tmp[i] : InvSlot{};
}
inline void invClear(InvSlot* s) { for (int i = 0; i < kInvSlots; ++i) s[i] = {}; }
// Kit inicial (conta nova): 1 adaga (acesso rápido 1) e 3 cocos (acesso rápido 2). Nada mais.
inline void invStarterKit(InvSlot* s) {
    invClear(s);
    s[kInvHotFirst].id = item::Dagger;   s[kInvHotFirst].n = 1;
    s[kInvHotFirst + 1].id = item::Coconut; s[kInvHotFirst + 1].n = 3;
}
// MORTE: perde TUDO menos a adaga (se tinha uma, fica com 1). As moedas não entram aqui (são compradas; ficam na conta).
inline void invDeathLoss(InvSlot* s) {
    bool dagger = false;
    for (int i = 0; i < kInvSlots; ++i) if (s[i].id == item::Dagger && s[i].n > 0) { dagger = true; break; }
    invClear(s);
    if (dagger) { s[kInvHotFirst].id = item::Dagger; s[kInvHotFirst].n = 1; }
}
// Saneia (vindo do disco/rede): ids e quantidades válidos.
inline void invSanitize(InvSlot* s) { for (int i = 0; i < kInvSlots; ++i) if (!itemValid(s[i].id) || s[i].n == 0 || s[i].n > itemMaxStack(s[i].id)) s[i] = {}; }

// ---- CRAFT (receitas: a MESMA tabela no servidor e na tela de craft do cliente) ----
struct Recipe { int out, outN; int in[3], inN[3]; float time; };   // time = segundos de fabricação (barra de progresso no cliente)
inline const Recipe* recipeTable(int& n) {
    using namespace item;
    static const Recipe kR[11] = {
        {Rope,    1, {Frond, 0, 0},           {3, 0, 0}, 3.0f},
        {Hatchet, 1, {Wood, Stone, Rope},     {1, 1, 1}, 8.0f},
        {Pickaxe, 1, {Wood, Stone, Rope},     {2, 2, 1}, 10.0f},
        {Torch,   1, {Wood, Cloth, 0},        {1, 1, 0}, 4.0f},
        {Dagger,  1, {Stone, Wood, Rope},     {1, 1, 1}, 6.0f},
        {Ammo,    5, {Iron, Pebble, 0},       {1, 2, 0}, 4.0f},      // MUNICAO x5: chumbo (ferro) + pólvora
        {Gun,     1, {Iron, Wood, Gold},      {3, 2, 1}, 15.0f},     // BACAMARTE
        {PotHeal,   1, {Coconut, Frond, 0},       {2, 2, 0}, 4.0f},
        {PotFull,   1, {Coconut, Frond, Gold},    {3, 2, 1}, 8.0f},
        {PotSpeed,  1, {Frond, Coconut, Iron},    {3, 1, 1}, 6.0f},
        {PotShield, 1, {Diamond, Frond, Iron},    {1, 2, 1}, 10.0f},
    };
    n = 11; return kR;
}
inline bool invCanCraft(const InvSlot* s, const Recipe& r) { for (int k = 0; k < 3; ++k) if (r.in[k] && invCount(s, r.in[k]) < r.inN[k]) return false; return true; }
// Fabrica a receita i: 0 = ok, 1 = faltam ingredientes, 2 = não cabe (nada muda), 3 = receita inválida.
inline int invCraft(InvSlot* s, int i) {
    int n; const Recipe* R = recipeTable(n);
    if (i < 0 || i >= n) return 3;
    const Recipe& r = R[i];
    if (!invCanCraft(s, r)) return 1;
    InvSlot bak[kInvSlots]; std::memcpy(bak, s, sizeof bak);
    for (int k = 0; k < 3; ++k) if (r.in[k]) invRemove(s, r.in[k], r.inN[k]);
    if (!invFits(s, r.out, r.outN)) { std::memcpy(s, bak, sizeof bak); return 2; }
    invAdd(s, r.out, r.outN);
    return 0;
}

}  // namespace e0net
