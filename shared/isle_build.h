// Isolated - MELHORIAS DA ILHA (header-only, compartilhado servidor/cliente): estruturas que a TRIPULAÇÃO dona ergue numa ilha dominada.
//
//  * Só nas ilhas do oceano (shared/conquest.h) e só a tripulação DONA constrói/demole; quem toma a ilha herda tudo.
//  * Paga com materiais da mochila de quem constrói (o servidor confere e desconta). Demolir devolve metade.
//    Cada custo cabe numa mochila (kInvMaxKg 40 kg: madeira 1,5 kg, pedra 2 kg).
//  * Posição livre na ilha (girando de 45 em 45 graus), em terra seca e quase plana, sem árvores/pedras/ruínas, longe do mastro e sem
//    sobrepor outra estrutura. A mesma validação roda no app (prévia verde/vermelha) e no servidor (autoridade).
//  * MURALHA: uma compra só e ela CERCA A ILHA TODA (paliçada seguindo a costa, calculada da ilha: computeIsleWall), já com PORTÃO e um
//    CAIS de madeira para o mar (o navio chamado perto dele atraca na frente do cais). O cais é chão caminhável (supportHeightAt).
//  * Efeitos: muralha bloqueia a passagem (colisão em stepPlayer; só se entra pelo portão), torre de vigia deixa a dominação dos atacantes
//    mais lenta, marcenaria e armazém aumentam a renda da ilha, fogueira ilumina à noite.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "terrain.h"
#include "inventory.h"

namespace e0net {

enum StructKind : uint8_t { SkWall = 0, SkTower, SkWorkshop, SkStorehouse, SkCampfire, SkKindCount };   // SkWall = muralha em volta da ilha (+ portão + cais)
constexpr int kIsleMaxStructs = 40;          // estruturas por ilha
constexpr float kStructPoleClear = 3.5f;     // distância mínima do mastro (o mastro continua acessível)
constexpr float kTowerSlowdown = 0.15f;      // cada torre (até kTowerMax) tira 15% da velocidade de dominação dos atacantes
constexpr int kTowerMax = 2;

struct SBox { float x0, x1, z0, z1; };   // caixa de colisão no espaço da estrutura (m); +z = frente
struct StructDef {
    const char* name; const char* desc;
    float hx, hz;            // meia largura / meia profundidade do chão ocupado
    float height;            // altura (desenho e prévia)
    float maxStep;           // desnível máximo do terreno sob a base
    uint8_t cost[3], costN[3];
    int income;              // moedas a mais por pagamento da renda
    int maxPer;              // máximo por ilha
    int nBox; SBox box[4];
};
inline const StructDef& structDef(int k) {
    using namespace item;
    static const StructDef d[SkKindCount] = {
        {"MURALHA", "CERCA A ILHA TODA, COM PORT\xC3O E CAIS PARA OS NAVIOS", 2.0f, 2.0f, 3.6f, 99.0f, {Wood, Rope, Stone}, {20, 4, 2}, 0, 1, 0, {}},
        {"TORRE DE VIGIA", "ATACANTES DOMINAM 15% MAIS DEVAGAR (AT\xC9 2)", 1.5f, 1.5f, 6.8f, 1.0f, {Wood, Rope, 0}, {10, 2, 0}, 0, 4, 4,
         {{-1.45f, -1.05f, -1.45f, -1.05f}, {1.05f, 1.45f, -1.45f, -1.05f}, {-1.45f, -1.05f, 1.05f, 1.45f}, {1.05f, 1.45f, 1.05f, 1.45f}}},
        {"MARCENARIA", "OFICINA DE MADEIRA: +25 MOEDAS NA RENDA", 3.2f, 2.5f, 4.2f, 1.0f, {Wood, Stone, Rope}, {14, 4, 2}, 25, 2, 4,
         {{-3.2f, 3.2f, -2.5f, -2.15f}, {-3.2f, -2.85f, -2.5f, 0.6f}, {2.85f, 3.2f, -2.5f, 0.6f}, {-1.3f, 1.3f, -1.1f, -0.3f}}},
        {"ARMAZ\xC9M", "GALP\xC3O DE MANTIMENTOS: +15 MOEDAS NA RENDA", 2.6f, 2.1f, 4.0f, 1.0f, {Wood, Stone, 0}, {12, 5, 0}, 15, 2, 1,
         {{-2.6f, 2.6f, -2.1f, 2.1f}}},
        {"FOGUEIRA", "ILUMINA A ILHA \xC0 NOITE", 0.7f, 0.7f, 0.9f, 0.6f, {Wood, Stone, 0}, {3, 3, 0}, 0, 6, 1,
         {{-0.55f, 0.55f, -0.55f, 0.55f}}},
    };
    return d[k >= 0 && k < SkKindCount ? k : 0];
}

struct IsleStruct { uint16_t id = 0; uint8_t kind = 0, rot = 0; float x = 0, y = 0, z = 0; };   // rot 0..7 (x 45 graus); y = base (o ponto mais alto do chão sob ela)

// estado (servidor: autoridade; cliente: cópia do MsgStructSync). gen muda a cada alteração da ilha (o renderer refaz a malha).
inline std::vector<IsleStruct>& isleStructs(int slot) { static std::vector<IsleStruct> v[kIslandSlots]; return v[slot >= 0 && slot < kIslandSlots ? slot : 0]; }
inline uint32_t& isleStructGen(int slot) { static uint32_t g[kIslandSlots] = {}; return g[slot >= 0 && slot < kIslandSlots ? slot : 0]; }

inline float structYaw(uint8_t rot) { return (float)(rot & 7) * 0.78539816f; }
inline void structToLocal(float yaw, float cx, float cz, float wx, float wz, float& lx, float& lz) {
    const float c = std::cos(yaw), s = std::sin(yaw), dx = wx - cx, dz = wz - cz;
    lx = dx * c - dz * s; lz = dx * s + dz * c;
}
inline void structToWorld(float yaw, float cx, float cz, float lx, float lz, float& wx, float& wz) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    wx = cx + lx * c + lz * s; wz = cz - lx * s + lz * c;
}
inline void structPushBox(float& lx, float& lz, const SBox& b, float r) {
    const float cx = std::max(b.x0, std::min(b.x1, lx)), cz = std::max(b.z0, std::min(b.z1, lz));
    const float dx = lx - cx, dz = lz - cz, d2 = dx * dx + dz * dz;
    if (d2 >= r * r) return;
    if (d2 > 1e-6f) { const float d = std::sqrt(d2), k = (r - d) / d; lx += dx * k; lz += dz * k; return; }
    const float dl = lx - b.x0, dr = b.x1 - lx, db = lz - b.z0, dt = b.z1 - lz, m = std::min(std::min(dl, dr), std::min(db, dt));
    if (m == dl) lx = b.x0 - r; else if (m == dr) lx = b.x1 + r; else if (m == db) lz = b.z0 - r; else lz = b.z1 + r;
}
// ---- MURALHA: anel em volta da ilha (calculado da própria ilha: o mesmo no servidor e no app) ----
constexpr float kWallLandH = 1.3f;    // a muralha corre onde o chão passa desta altura (vindo do mar), 1,2 m para dentro
constexpr float kWallSeg = 4.0f;      // comprimento de cada trecho
constexpr float kWallR = 0.30f;       // meia espessura (colisão)
constexpr float kPierHalfW = 1.6f;    // meia largura do cais
constexpr float kPierDeckY = 1.05f;   // altura do tabuado do cais
struct IsleWall {
    bool ok = false; const World* w = nullptr;
    std::vector<float> x, z;          // pontos do anel (fechado: o trecho i vai do ponto i ao i+1)
    int gate = -1;                    // trecho do portão (aberto)
    float gx = 0, gz = 0, gy = 0, nx = 0, nz = 1, half = 2.0f;   // centro do portão, chão, normal para o MAR, meia largura do vão
    float p0 = 0, p1 = 0;             // cais: de (g + n*p0) até (g + n*p1)
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0;   // caixa do anel (corte rápido)
};
inline bool computeIsleWall(const World& w, IsleWall& o) {
    o = IsleWall(); o.w = &w;
    const float cx = w.oxcm * 0.01f, cz = w.ozcm * 0.01f;
    constexpr int NR = 360;
    float rad[NR], org[NR]; bool has[NR]; int nHas = 0;
    for (int i = 0; i < NR; ++i) {
        const float a = 6.2831853f * i / NR, dx = std::sin(a), dz = -std::cos(a);
        has[i] = false; rad[i] = 0;
        for (float r = World::kHalf - 6.0f; r > 6.0f; r -= 0.5f)
            if (w.heightAt(cx + dx * r, cz + dz * r) >= kWallLandH) { rad[i] = r - 1.2f; has[i] = true; ++nHas; break; }
    }
    if (nHas < NR / 3) return false;
    for (int i = 0; i < NR; ++i) if (!has[i]) {   // raio sem terra: interpola entre os vizinhos que têm
        int a = i, b = i, da = 0, db = 0;
        while (!has[a]) { a = (a + NR - 1) % NR; ++da; }
        while (!has[b]) { b = (b + 1) % NR; ++db; }
        rad[i] = rad[a] + (rad[b] - rad[a]) * (float)da / (float)(da + db);
    }
    std::copy(rad, rad + NR, org);
    for (int pass = 0; pass < 3; ++pass) {   // suaviza (sem empurrar para fora da terra)
        float t[NR];
        for (int i = 0; i < NR; ++i) { float s = 0; for (int k = -2; k <= 2; ++k) s += rad[(i + k + NR) % NR]; t[i] = std::min(s / 5.0f, org[i] + 0.5f); }
        std::copy(t, t + NR, rad);
    }
    std::vector<float> px(NR), pz(NR); float L = 0;
    for (int i = 0; i < NR; ++i) { const float a = 6.2831853f * i / NR; px[i] = cx + std::sin(a) * rad[i]; pz[i] = cz - std::cos(a) * rad[i]; }
    for (int i = 0; i < NR; ++i) { const int j = (i + 1) % NR; L += std::hypot(px[j] - px[i], pz[j] - pz[i]); }
    const int n = std::max(12, (int)std::lround(L / kWallSeg)); const float step = L / (float)n;
    float acc = 0; int seg = 0; float segLen = std::hypot(px[1] - px[0], pz[1] - pz[0]);
    for (int k = 0; k < n; ++k) {   // reamostra o contorno em trechos iguais
        const float target = step * (float)k;
        while (acc + segLen < target && seg < NR - 1) { acc += segLen; ++seg; const int j = (seg + 1) % NR; segLen = std::hypot(px[j] - px[seg], pz[j] - pz[seg]); }
        const int j = (seg + 1) % NR; const float t = segLen > 1e-4f ? (target - acc) / segLen : 0.0f;
        o.x.push_back(px[seg] + (px[j] - px[seg]) * t); o.z.push_back(pz[seg] + (pz[j] - pz[seg]) * t);
    }
    // PORTÃO + CAIS: no trecho com o mar fundo mais perto (o navio chega perto)
    float best = 1e9f; int bi = 0; float bs0 = 0, bd = 60.0f, bnx = 0, bnz = 1;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float mx = (o.x[i] + o.x[j]) * 0.5f, mz = (o.z[i] + o.z[j]) * 0.5f, ex = o.x[j] - o.x[i], ez = o.z[j] - o.z[i], el = std::hypot(ex, ez) + 1e-5f;
        float nx = ez / el, nz = -ex / el;
        if (nx * (mx - cx) + nz * (mz - cz) < 0) { nx = -nx; nz = -nz; }
        float s0 = -1, d = -1;
        for (float s = 0; s <= 60.0f; s += 1.0f) {
            const float h = w.heightAt(mx + nx * s, mz + nz * s);
            if (s0 < 0 && h < 0.6f) s0 = s;
            if (h < -2.4f) { d = s; break; }
        }
        if (s0 < 0) continue;
        const float score = d < 0 ? 100.0f + s0 : d;
        if (score < best) { best = score; bi = i; bs0 = s0; bd = d < 0 ? 60.0f : d; bnx = nx; bnz = nz; }
    }
    if (best > 1e8f) return false;
    const int bj = (bi + 1) % n;
    o.gate = bi; o.gx = (o.x[bi] + o.x[bj]) * 0.5f; o.gz = (o.z[bi] + o.z[bj]) * 0.5f; o.gy = w.heightAt(o.gx, o.gz);
    o.nx = bnx; o.nz = bnz; o.half = std::hypot(o.x[bj] - o.x[bi], o.z[bj] - o.z[bi]) * 0.5f;
    o.p0 = std::max(1.5f, bs0 - 1.0f); o.p1 = std::min(o.p0 + 45.0f, std::max(o.p0 + 8.0f, bd + 6.0f));
    o.x0 = o.z0 = 1e9f; o.x1 = o.z1 = -1e9f;
    for (int i = 0; i < n; ++i) { o.x0 = std::min(o.x0, o.x[i]); o.x1 = std::max(o.x1, o.x[i]); o.z0 = std::min(o.z0, o.z[i]); o.z1 = std::max(o.z1, o.z[i]); }
    o.ok = true;
    return true;
}
inline bool hasStruct(int slot, int kind) { for (const IsleStruct& s : isleStructs(slot)) if (s.kind == kind) return true; return false; }
// geometria do anel da ilha (calculada 1x por ilha carregada; existe mesmo sem muralha: prévia)
inline const IsleWall* wallGeomOf(int slot) {
    static IsleWall cache[kIslandSlots];
    const World* w = terrain().world(slot);
    if (slot < 1 || slot >= kIslandSlots || !w) return nullptr;
    IsleWall& c = cache[slot];
    if (c.w != w) computeIsleWall(*w, c);
    return c.ok ? &c : nullptr;
}
inline const IsleWall* isleWallOf(int slot) { return slot >= 1 && slot < kIslandSlots && hasStruct(slot, SkWall) ? wallGeomOf(slot) : nullptr; }   // só se a muralha foi erguida
inline bool pierAt(const IsleWall& W, float x, float z) {
    const float dx = x - W.gx, dz = z - W.gz, u = dx * W.nx + dz * W.nz, v = -dx * W.nz + dz * W.nx;
    return u >= W.p0 - 0.5f && u <= W.p1 && std::fabs(v) <= kPierHalfW;
}
inline void pushFromSeg(float& x, float& z, float ax, float az, float bx, float bz, float r) {
    const float ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    const float t = l2 > 1e-6f ? std::max(0.0f, std::min(1.0f, ((x - ax) * ex + (z - az) * ez) / l2)) : 0.0f;
    const float qx = ax + ex * t, qz = az + ez * t, dx = x - qx, dz = z - qz, d2 = dx * dx + dz * dz;
    if (d2 >= r * r) return;
    if (d2 > 1e-8f) { const float d = std::sqrt(d2), k = (r - d) / d; x += dx * k; z += dz * k; }
    else { const float l = std::sqrt(l2) + 1e-6f; x += -ez / l * r; z += ex / l * r; }
}

// Colisão do jogador com as estruturas da ilha em que ele está (stepPlayer: servidor e predição).
inline void collideStructs(float& x, float& z, float r) {
    const int slot = terrain().slotAt(x, z);
    if (slot < 1) return;
    for (const IsleStruct& s : isleStructs(slot)) {
        const StructDef& D = structDef(s.kind);
        const float ex = std::max(D.hx, D.hz) + r + 0.5f;
        if (std::fabs(x - s.x) > ex || std::fabs(z - s.z) > ex) continue;
        const float yaw = structYaw(s.rot);
        float lx, lz; structToLocal(yaw, s.x, s.z, x, z, lx, lz);
        for (int b = 0; b < D.nBox; ++b) structPushBox(lx, lz, D.box[b], r);
        structToWorld(yaw, s.x, s.z, lx, lz, x, z);
    }
    if (const IsleWall* W = isleWallOf(slot)) {   // MURALHA: trechos do anel (o do portão é aberto)
        if (x < W->x0 - 4.0f || x > W->x1 + 4.0f || z < W->z0 - 4.0f || z > W->z1 + 4.0f) return;
        const int n = (int)W->x.size();
        for (int i = 0; i < n; ++i) {
            if (i == W->gate) continue;
            const int j = (i + 1) % n;
            if (std::fabs(x - W->x[i]) > kWallSeg + 1.5f || std::fabs(z - W->z[i]) > kWallSeg + 1.5f) continue;
            pushFromSeg(x, z, W->x[i], W->z[i], W->x[j], W->z[j], kWallR + r);
        }
    }
}
// tabuado do cais (chão caminhável sobre a água); -1e9 = não está no cais
inline float isleFloorAt(float x, float z) {
    const int slot = terrain().slotAt(x, z);
    if (slot < 1) return -1e9f;
    const IsleWall* W = isleWallOf(slot);
    return W && pierAt(*W, x, z) ? kPierDeckY : -1e9f;
}

enum StructRes : uint8_t { SrOk = 1, SrNoCrew, SrNotOwner, SrNotHere, SrSlope, SrWater, SrBlocked, SrOverlap, SrPole, SrLimit, SrKindLimit, SrCost, SrPeople, SrNotFound, SrSlow, SrWallBlocked };
inline const char* structResText(uint8_t r) {
    switch (r) {
        case SrOk: return "OK";
        case SrNoCrew: return "S\xD3 PARA QUEM \xC9 DE UMA TRIPULA\xC7\xC3O";
        case SrNotOwner: return "A ILHA N\xC3O \xC9 DA SUA TRIPULA\xC7\xC3O";
        case SrNotHere: return "FORA DA ILHA";
        case SrSlope: return "TERRENO MUITO INCLINADO";
        case SrWater: return "PRECISA DE TERRA SECA";
        case SrBlocked: return "\xC1RVORE, PEDRA OU RU\xCDNA NO CAMINHO";
        case SrOverlap: return "EM CIMA DE OUTRA ESTRUTURA OU DA MURALHA";
        case SrPole: return "MUITO PERTO DO MASTRO";
        case SrLimit: return "A ILHA J\xC1 TEM O M\xC1XIMO DE ESTRUTURAS";
        case SrKindLimit: return "LIMITE DESSA ESTRUTURA NA ILHA";
        case SrCost: return "FALTAM MATERIAIS NA MOCHILA";
        case SrPeople: return "TEM ALGU\xC9M NO LOCAL";
        case SrNotFound: return "ESTRUTURA N\xC3O ENCONTRADA";
        case SrSlow: return "ESPERE UM POUCO";
        case SrWallBlocked: return "TIRE AS ESTRUTURAS DO CAMINHO DA MURALHA";
        default: return "N\xC3O DEU";
    }
}
inline bool structHasCost(const InvSlot* inv, int kind) {
    const StructDef& D = structDef(kind);
    for (int k = 0; k < 3; ++k) if (D.cost[k] && invCount(inv, D.cost[k]) < D.costN[k]) return false;
    return true;
}
inline int structCountKind(int slot, int kind) { int n = 0; for (const IsleStruct& s : isleStructs(slot)) if (s.kind == kind) ++n; return n; }
inline int structTowers(int slot) { return std::min(kTowerMax, structCountKind(slot, SkTower)); }
inline int structIncome(int slot) { int n = 0; for (const IsleStruct& s : isleStructs(slot)) n += structDef(s.kind).income; return n; }

// dois retângulos girados se sobrepõem (eixos separadores); shrink > 0 deixa encostar
inline bool structOverlap(const IsleStruct& a, const IsleStruct& b, float shrink) {
    const float ya = structYaw(a.rot), yb = structYaw(b.rot);
    const StructDef& A = structDef(a.kind); const StructDef& B = structDef(b.kind);
    const float ax[2][2] = {{std::cos(ya), -std::sin(ya)}, {std::sin(ya), std::cos(ya)}}, bx[2][2] = {{std::cos(yb), -std::sin(yb)}, {std::sin(yb), std::cos(yb)}};
    const float ah[2] = {A.hx - shrink, A.hz - shrink}, bh[2] = {B.hx - shrink, B.hz - shrink};
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float* axes[4] = {ax[0], ax[1], bx[0], bx[1]};
    for (const float* n : axes) {
        const float ra = ah[0] * std::fabs(ax[0][0] * n[0] + ax[0][1] * n[1]) + ah[1] * std::fabs(ax[1][0] * n[0] + ax[1][1] * n[1]);
        const float rb = bh[0] * std::fabs(bx[0][0] * n[0] + bx[0][1] * n[1]) + bh[1] * std::fabs(bx[1][0] * n[0] + bx[1][1] * n[1]);
        if (std::fabs(dx * n[0] + dz * n[1]) > ra + rb) return false;
    }
    return true;
}

// a estrutura (retângulo) encosta no anel da muralha, no vão do portão ou no cais?
inline bool structHitsWall(const IsleWall& W, uint8_t kind, uint8_t rot, float x, float z) {
    const StructDef& D = structDef(kind);
    const float yaw = structYaw(rot), m = 0.9f, R = std::hypot(D.hx, D.hz) + m;
    const int n = (int)W.x.size();
    auto inside = [&](float px, float pz) { float lx, lz; structToLocal(yaw, x, z, px, pz, lx, lz); return std::fabs(lx) < D.hx + m && std::fabs(lz) < D.hz + m; };
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        if (std::fabs(W.x[i] - x) > R + kWallSeg || std::fabs(W.z[i] - z) > R + kWallSeg) continue;
        for (int k = 0; k <= 4; ++k) if (inside(W.x[i] + (W.x[j] - W.x[i]) * k / 4.0f, W.z[i] + (W.z[j] - W.z[i]) * k / 4.0f)) return true;
    }
    for (float u = W.p0 - 3.0f; u <= W.p1; u += 1.0f) for (int v = -1; v <= 1; ++v)   // cais e a frente do portão
        if (inside(W.gx + W.nx * u - W.nz * kPierHalfW * v, W.gz + W.nz * u + W.nx * kPierHalfW * v)) return true;
    return false;
}
// A MURALHA pode ser erguida? (ilha com contorno, sem outra muralha, nenhuma estrutura no caminho do anel/cais)
inline uint8_t wallCheckPlace(int slot) {
    const IsleWall* W = wallGeomOf(slot);
    if (!W) return SrNotHere;
    if (hasStruct(slot, SkWall)) return SrKindLimit;
    if ((int)isleStructs(slot).size() >= kIsleMaxStructs) return SrLimit;
    for (const IsleStruct& s : isleStructs(slot)) if (structHitsWall(*W, s.kind, s.rot, s.x, s.z)) return SrWallBlocked;
    return SrOk;
}

// O lugar serve? (terreno, objetos do mundo, mastro, outras estruturas e limites). Não confere tripulação, materiais nem jogadores (só o servidor).
// outY = base da estrutura (o ponto mais alto do chão sob ela).
inline uint8_t structCheckPlace(const World& w, int slot, uint8_t kind, uint8_t rot, float x, float z, float poleX, float poleZ, float& outY, uint16_t ignoreId = 0) {
    if (kind >= SkKindCount || !std::isfinite(x) || !std::isfinite(z)) return SrNotHere;
    const StructDef& D = structDef(kind);
    const float yaw = structYaw(rot);
    float lo = 1e9f, hi = -1e9f;
    for (int iz = 0; iz <= 2; ++iz) for (int ix = 0; ix <= 4; ++ix) {
        float wx, wz; structToWorld(yaw, x, z, -D.hx + 2.0f * D.hx * ix / 4.0f, -D.hz + 2.0f * D.hz * iz / 2.0f, wx, wz);
        if (terrain().slotAt(wx, wz) != slot) return SrNotHere;
        const float h = w.heightAt(wx, wz);
        lo = std::min(lo, h); hi = std::max(hi, h);
    }
    if (lo < 0.35f) return SrWater;
    if (hi - lo > D.maxStep) return SrSlope;
    outY = hi;
    for (const WorldObject& o : w.objects) {
        if (World::isCollectible(o.type)) continue;
        const float ox = o.xcm * 0.01f, oz = o.zcm * 0.01f;
        const float r = World::isBuilding(o.type) ? 5.0f : (o.type == ObjLog ? 2.4f : (o.type == ObjRock ? 1.1f : 0.6f));
        if (std::fabs(ox - x) > D.hx + D.hz + r || std::fabs(oz - z) > D.hx + D.hz + r) continue;
        float lx, lz; structToLocal(yaw, x, z, ox, oz, lx, lz);
        if (std::fabs(lx) < D.hx + r && std::fabs(lz) < D.hz + r) return SrBlocked;
    }
    {   // mastro: distância do mastro ao retângulo
        float lx, lz; structToLocal(yaw, x, z, poleX, poleZ, lx, lz);
        const float qx = std::max(0.0f, std::fabs(lx) - D.hx), qz = std::max(0.0f, std::fabs(lz) - D.hz);
        if (qx * qx + qz * qz < kStructPoleClear * kStructPoleClear) return SrPole;
    }
    if (const IsleWall* W = isleWallOf(slot)) if (structHitsWall(*W, kind, rot, x, z)) return SrOverlap;
    IsleStruct me; me.kind = kind; me.rot = rot; me.x = x; me.z = z;
    int n = 0, nk = 0;
    for (const IsleStruct& s : isleStructs(slot)) {
        if (s.id == ignoreId && ignoreId) continue;
        ++n; if (s.kind == kind) ++nk;
        if (structOverlap(me, s, 0.06f)) return SrOverlap;
    }
    if (n >= kIsleMaxStructs) return SrLimit;
    if (nk >= D.maxPer) return SrKindLimit;
    return SrOk;
}

}  // namespace e0net
