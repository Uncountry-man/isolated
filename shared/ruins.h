// Isolated - RUÍNAS (cabana, galpão, torre): geometria COMPARTILHADA entre o desenho (terrain_renderer) e a colisão (World::pushOut).
// As paredes quebradas são sorteadas por hash da posição do objeto; desenho e colisão chamam as MESMAS funções, então o que se vê é o que bloqueia.
// Só usa + - * / e comparações (nada de sin/cos/rand): resultado idêntico em qualquer CPU. Coordenadas LOCAIS (antes de girar/escalar).
#pragma once
#include <algorithm>
#include <cstdint>

namespace e0net {

inline uint32_t ruinHash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
inline float ruinRand(uint32_t seed, int i) { return (ruinHash(seed, (uint32_t)i) & 0xFFFF) / 65535.0f; }
inline uint32_t ruinSeed(int32_t xcm, int32_t zcm) { return ruinHash((uint32_t)xcm, (uint32_t)zcm); }

struct RuinDims { float hx, hz, wh; };
inline RuinDims ruinDims(int kind) { return kind == 0 ? RuinDims{2.3f, 1.8f, 2.4f} : (kind == 1 ? RuinDims{3.4f, 2.4f, 3.0f} : RuinDims{1.5f, 1.5f, 6.0f}); }

// Parede quebrada de (x0,z0) a (x1,z1) (alinhada a um eixo): trechos de ~0,55 m com altura em passeio aleatório e brechas.
// f(k, r, bx0, bz0, bx1, bz1, h, cx, cz) para cada trecho em pé (caixa [bx0,bx1]x[bz0,bz1], altura h; (cx,cz) = centro do trecho; r = sorteio do trecho).
template <class F>
void ruinWallSegs(uint32_t seed, int salt, float x0, float z0, float x1, float z1, float maxH, float th, float gapChance, F&& f) {
    const float dx = x1 - x0, dz = z1 - z0;
    const bool alongX = (dx < 0 ? -dx : dx) >= (dz < 0 ? -dz : dz);
    const float len = alongX ? (dx < 0 ? -dx : dx) : (dz < 0 ? -dz : dz);
    const int n = std::max(2, (int)(len / 0.55f));
    float h = maxH * (0.5f + 0.5f * ruinRand(seed, salt));
    for (int k = 0; k < n; ++k) {
        const float t0 = (float)k / n, t1 = (float)(k + 1) / n;
        const float r = ruinRand(seed, salt * 31 + k + 7);
        h = std::min(maxH, std::max(maxH * 0.10f, h + (r - 0.55f) * maxH * 0.75f));
        const float hh = (k > 0 && k < n - 1 && ruinRand(seed, salt * 57 + k) < gapChance) ? 0.0f : h;
        if (hh < 0.05f) continue;
        const float ax0 = x0 + dx * t0, az0 = z0 + dz * t0, ax1 = x0 + dx * t1, az1 = z0 + dz * t1;
        if (alongX) f(k, r, std::min(ax0, ax1), az0 - th, std::max(ax0, ax1), az0 + th, hh, (ax0 + ax1) * 0.5f, az0);
        else f(k, r, ax0 - th, std::min(az0, az1), ax0 + th, std::max(az0, az1), hh, ax0, (az0 + az1) * 0.5f);
    }
}

// As 4 paredes (cabana/galpão) com os parâmetros usados no desenho.
template <class F>
void ruinWalls(int kind, uint32_t seed, F&& f) {
    const RuinDims d = ruinDims(kind);
    ruinWallSegs(seed, 1, -d.hx, -d.hz, d.hx, -d.hz, d.wh * 0.95f, 0.11f, 0.12f, f);
    ruinWallSegs(seed, 2, -d.hx, d.hz, d.hx, d.hz, d.wh * 0.45f, 0.11f, 0.30f, f);
    ruinWallSegs(seed, 3, -d.hx, -d.hz, -d.hx, d.hz, d.wh * 0.70f, 0.11f, 0.20f, f);
    ruinWallSegs(seed, 4, d.hx, -d.hz, d.hx, d.hz, d.wh * 0.55f, 0.11f, 0.25f, f);
}
inline float ruinPillarH(int kind, uint32_t seed, int i) { return ruinDims(kind).wh * (0.25f + 0.75f * ruinRand(seed, 500 + i)); }
inline float ruinChimneyH(uint32_t seed) { return 1.2f + 1.3f * ruinRand(seed, 600); }
inline float ruinPostH(uint32_t seed, int i) {   // altura dos 4 postes da torre
    static const float k[4] = {0.88f, 0.30f, 0.84f, 0.14f};
    return ruinDims(2).wh * k[i] * (0.85f + 0.3f * ruinRand(seed, 800 + i));
}

struct RuinBox { float x0, z0, x1, z1; };
constexpr float kRuinSolidH = 0.45f;   // só o que passa de 45 cm bloqueia (entulho baixo se atravessa)
constexpr int kMaxRuinBoxes = 48;

// Caixas sólidas (locais) da ruína: trechos de parede em pé (vizinhos viram uma caixa só), pilares, chaminé, postes da torre.
inline int ruinSolidBoxes(int kind, uint32_t seed, RuinBox* out, int cap) {
    int n = 0;
    auto add = [&](float x0, float z0, float x1, float z1) { if (n < cap) out[n++] = {x0, z0, x1, z1}; };
    if (kind == 2) {
        static const float px[4] = {-0.8f, 0.8f, -0.8f, 0.8f}, pz[4] = {-0.8f, -0.8f, 0.8f, 0.8f};
        for (int i = 0; i < 4; ++i) if (ruinPostH(seed, i) >= kRuinSolidH) add(px[i] - 0.14f, pz[i] - 0.14f, px[i] + 0.14f, pz[i] + 0.14f);
        return n;
    }
    const RuinDims d = ruinDims(kind);
    int first = n; (void)first;
    int lastSalt = -1; float lastX1 = 0, lastZ1 = 0; int runIdx = -1;
    auto seg = [&](int salt, float bx0, float bz0, float bx1, float bz1, float h) {
        if (h < kRuinSolidH) { runIdx = -1; return; }
        const bool contiguous = runIdx >= 0 && lastSalt == salt && (bx0 - lastX1 < 0.02f && bx0 - lastX1 > -0.02f && bz0 - lastZ1 < 0.3f && bz0 - lastZ1 > -0.3f);
        const bool contiguousZ = runIdx >= 0 && lastSalt == salt && (bz0 - lastZ1 < 0.02f && bz0 - lastZ1 > -0.02f && bx0 - lastX1 < 0.3f && bx0 - lastX1 > -0.3f);
        if (contiguous || contiguousZ) {
            out[runIdx].x0 = std::min(out[runIdx].x0, bx0); out[runIdx].z0 = std::min(out[runIdx].z0, bz0);
            out[runIdx].x1 = std::max(out[runIdx].x1, bx1); out[runIdx].z1 = std::max(out[runIdx].z1, bz1);
        } else if (n < cap) { runIdx = n; out[n++] = {bx0, bz0, bx1, bz1}; }
        lastSalt = salt; lastX1 = bx1; lastZ1 = bz1;
    };
    // cada parede (salt) é percorrida em ordem, então trechos consecutivos em pé se fundem
    struct W { int salt; float x0, z0, x1, z1, mh, gap; };
    const W ws[4] = {{1, -d.hx, -d.hz, d.hx, -d.hz, d.wh * 0.95f, 0.12f}, {2, -d.hx, d.hz, d.hx, d.hz, d.wh * 0.45f, 0.30f},
                     {3, -d.hx, -d.hz, -d.hx, d.hz, d.wh * 0.70f, 0.20f}, {4, d.hx, -d.hz, d.hx, d.hz, d.wh * 0.55f, 0.25f}};
    for (const W& w : ws) {
        runIdx = -1;
        ruinWallSegs(seed, w.salt, w.x0, w.z0, w.x1, w.z1, w.mh, 0.11f, w.gap,
                     [&](int, float, float bx0, float bz0, float bx1, float bz1, float h, float, float) { seg(w.salt, bx0, bz0, bx1, bz1, h); });
        // um trecho de brecha entre dois em pé quebra a sequência: ruinWallSegs só chama f para trechos em pé, então confira a emenda pela posição (feito em seg)
    }
    for (int i = 0; i < 4; ++i) {
        const float cx = (i & 1) ? d.hx : -d.hx, cz = (i & 2) ? d.hz : -d.hz;
        if (ruinPillarH(kind, seed, i) >= kRuinSolidH) add(cx - 0.16f, cz - 0.16f, cx + 0.16f, cz + 0.16f);
    }
    if (kind == 0 && ruinChimneyH(seed) >= kRuinSolidH) add(1.0f, -0.6f, 1.6f, 0.0f);
    return n;
}

}  // namespace e0net
