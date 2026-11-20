#include "map_gen.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../../shared/terrain.h"

namespace e0 {
namespace {

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smooth(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

inline uint32_t hash2(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (s + 0x165667B1u) * 0xC2B2AE35u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float vnoise(float x, float y, uint32_t s) {
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float fx = x - ix, fy = y - iy, ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    auto h = [&](int a, int b) { return (hash2((uint32_t)a, (uint32_t)b, s) & 0xFFFF) / 65535.0f; };
    return mixf(mixf(h(ix, iy), h(ix + 1, iy), ux), mixf(h(ix, iy + 1), h(ix + 1, iy + 1), ux), uy);
}
float fbm(float x, float y, uint32_t s, int oct) {
    float a = 0.5f, sum = 0, norm = 0;
    for (int i = 0; i < oct; ++i) { sum += a * vnoise(x, y, s + (uint32_t)i * 101u); norm += a; x *= 2.03f; y *= 2.03f; a *= 0.5f; }
    return sum / norm;
}

// Papel de pergaminho náutico em (u, v) em [0,1]: textura orgânica, manchas de maresia, vincos de dobra no mapa grande.
void paper(float u, float v, uint32_t seed, bool circle, bool folds, float& r, float& g, float& b) {
    float blot = fbm(u * 3.2f, v * 3.2f, seed, 4);
    float mid  = fbm(u * 10.5f, v * 10.5f, seed + 7u, 3);
    float fiber = vnoise(u * 260.0f, v * 90.0f, seed + 13u) * 0.6f + vnoise(u * 90.0f, v * 300.0f, seed + 17u) * 0.4f;
    float grain = vnoise(u * 680.0f, v * 680.0f, seed + 23u);
    if (folds) {   // mapa grande: carta náutica antiga em tom pergaminho quente-oceânico (com manchas suaves de idade e vincos de dobra)
        float k = 0.91f + 0.10f * blot + 0.04f * (mid - 0.5f) + 0.038f * (fiber - 0.5f) + 0.025f * (grain - 0.5f);
        // leve tintura marinha antiga no mar aberto (pergaminho levemente esverdeado/sepia, nada de amarelo berrante)
        r = 216.0f * k * (0.97f + 0.04f * blot);
        g = 200.0f * k * (0.985f + 0.025f * blot);
        b = 160.0f * k * (0.95f + 0.06f * blot);
        // 2 manchas sutis de umidade/sal
        for (int i = 0; i < 2; ++i) {
            float cx = 0.14f + 0.72f * (hash2((uint32_t)i, 11u, seed) & 0xFFFF) / 65535.0f;
            float cy = 0.16f + 0.68f * (hash2((uint32_t)i, 22u, seed) & 0xFFFF) / 65535.0f;
            float rad = 0.08f + 0.09f * (hash2((uint32_t)i, 33u, seed) & 0xFFFF) / 65535.0f;
            float d = std::sqrt((u - cx) * (u - cx) + (v - cy) * (v - cy)) + 0.015f * (vnoise(u * 32.0f, v * 32.0f, seed + 31u) - 0.5f);
            float ring = smooth(rad - 0.014f, rad, d) * (1.0f - smooth(rad, rad + 0.014f, d));
            float fill = 1.0f - smooth(rad - 0.025f, rad, d);
            float dark = 0.045f * ring + 0.014f * fill;
            r *= 1.0f - dark; g *= 1.0f - dark * 1.12f; b *= 1.0f - dark * 1.38f;
        }
        // vincos sutis de mapa dobrado (2 dobras verticais + 1 horizontal)
        float f1 = u - 0.335f - 0.004f * (vnoise(v * 18.0f, 3.0f, seed) - 0.5f);
        float f2 = u - 0.665f - 0.004f * (vnoise(v * 18.0f, 9.0f, seed) - 0.5f);
        float f3 = v - 0.500f - 0.004f * (vnoise(u * 18.0f, 7.0f, seed) - 0.5f);
        auto crease = [](float f) { return std::exp(-std::fabs(f) * 260.0f) * (f < 0 ? 0.075f : -0.025f); };
        float c = crease(f1) + crease(f2) + crease(f3);
        r *= 1.0f - c; g *= 1.0f - c; b *= 1.0f - c * 1.06f;
        // leve escurecimento natural perto das margens
        float ed = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
        float vig = (1.0f - smooth(0.0f, 0.14f, ed)) * 0.14f;
        r *= 1.0f - vig * 0.85f; g *= 1.0f - vig * 1.0f; b *= 1.0f - vig * 1.25f;
        r = std::max(0.0f, std::min(255.0f, r)); g = std::max(0.0f, std::min(255.0f, g)); b = std::max(0.0f, std::min(255.0f, b));
        return;
    }
    float k = 0.90f + 0.12f * blot + 0.035f * (mid - 0.5f) + 0.04f * (fiber - 0.5f) + 0.03f * (grain - 0.5f);
    r = 224.0f * k; g = 204.0f * k * (0.985f + 0.03f * blot); b = 158.0f * k * (0.94f + 0.07f * blot);
    // bordas escurecidas no minimapa
    float edge;
    if (circle) { float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2.0f; edge = smooth(0.86f, 1.0f, d + 0.04f * (blot - 0.5f)); }
    else { float d = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v)); edge = 1.0f - smooth(0.0f, 0.055f + 0.03f * mid, d + 0.015f * (blot - 0.5f)); }
    float burn = edge * (0.45f + 0.25f * mid);
    r *= 1.0f - burn * 0.42f; g *= 1.0f - burn * 0.55f; b *= 1.0f - burn * 0.72f;
    r = std::max(0.0f, std::min(255.0f, r)); g = std::max(0.0f, std::min(255.0f, g)); b = std::max(0.0f, std::min(255.0f, b));
}

void blend(std::vector<uint8_t>& px, int res, int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= res || y >= res || a <= 0) return;
    uint8_t* p = &px[((size_t)y * res + x) * 4];
    if (p[3] == 0) return;   // fora do círculo do minimapa
    p[0] = (uint8_t)mixf(p[0], r, a); p[1] = (uint8_t)mixf(p[1], g, a); p[2] = (uint8_t)mixf(p[2], b, a);
}

void fillTri(std::vector<uint8_t>& px, int res, float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
    int minx = (int)std::floor(std::min(x0, std::min(x1, x2))), maxx = (int)std::ceil(std::max(x0, std::max(x1, x2)));
    int miny = (int)std::floor(std::min(y0, std::min(y1, y2))), maxy = (int)std::ceil(std::max(y0, std::max(y1, y2)));
    auto edge = [](float ax, float ay, float bx, float by, float cx, float cy) { return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax); };
    float area = edge(x0, y0, x1, y1, x2, y2);
    if (std::fabs(area) < 1e-4f) return;
    for (int y = miny; y <= maxy; ++y) for (int x = minx; x <= maxx; ++x) {
        float px_ = x + 0.5f, py_ = y + 0.5f;
        float w0 = edge(x1, y1, x2, y2, px_, py_) / area, w1 = edge(x2, y2, x0, y0, px_, py_) / area, w2 = edge(x0, y0, x1, y1, px_, py_) / area;
        float m = std::min(w0, std::min(w1, w2));
        if (m >= -0.04f) blend(px, res, x, y, r, g, b, a * smooth(-0.04f, 0.05f, m));
    }
}

void disc(std::vector<uint8_t>& px, int res, float cx, float cy, float rad, float r, float g, float b, float a) {
    for (int y = (int)(cy - rad - 1); y <= (int)(cy + rad + 1); ++y) for (int x = (int)(cx - rad - 1); x <= (int)(cx + rad + 1); ++x) {
        float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
        blend(px, res, x, y, r, g, b, a * (1.0f - smooth(rad - 0.7f, rad + 0.5f, d)));
    }
}

}  // namespace

void renderMapTexture(const MapView& V, const std::vector<MapMark>& marks, std::vector<uint8_t>& out, uint32_t seed) {
    const int res = V.res;
    out.assign((size_t)res * res * 4, 255);
    const e0net::Terrain& T = e0net::terrain();
    const float mPerPx = 2.0f * V.half / (float)res;
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            uint8_t* o = &out[((size_t)y * res + x) * 4];
            float u = (x + 0.5f) / res, v = (y + 0.5f) / res;
            if (V.circle && std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) > 0.5f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            float r, g, b; paper(u, v, seed, V.circle, V.folds, r, g, b);
            float wx = V.cx + (u - 0.5f) * 2.0f * V.half, wz = V.cz + (v - 0.5f) * 2.0f * V.half;
            float dist = std::sqrt((wx - V.cx) * (wx - V.cx) + (wz - V.cz) * (wz - V.cz));
            float reveal = 1.0f - smooth(V.viewRadius * V.fadeFrac, V.viewRadius, dist);   // o que está longe se apaga no papel (área limitada)
            if (reveal > 0.0f && T.hasMain()) {
                float h0 = T.heightAt(wx, wz);
                float cover = 0.0f;
                if (h0 > -3.0f && h0 < 3.0f) {   // perto da costa: 4 subamostras para a borda da tinta ficar suave
                    const float s = 0.25f * mPerPx;
                    cover = ((T.heightAt(wx - s, wz - s) > 0.0f) + (T.heightAt(wx + s, wz - s) > 0.0f) + (T.heightAt(wx - s, wz + s) > 0.0f) + (T.heightAt(wx + s, wz + s) > 0.0f)) * 0.25f;
                } else cover = h0 > 0.0f ? 1.0f : 0.0f;
                if (cover <= 0.0f && h0 < 0.0f && h0 > -9.0f) {   // curvas de nível do raso (a cada 3 m), finas e sépia; o raso muito perto da costa tem um halo
                    float d3 = -h0 / 2.0f, f = d3 - std::floor(d3);
                    float line = (1.0f - smooth(0.0f, 0.10f, f)) * 0.42f + (f > 0.90f ? smooth(0.90f, 1.0f, f) * 0.42f : 0.0f);
                    float halo = (1.0f - smooth(0.0f, 1.2f, -h0)) * 0.30f;
                    float a = std::max(line, halo) * reveal;
                    r = mixf(r, 96.0f, a); g = mixf(g, 66.0f, a); b = mixf(b, 38.0f, a);
                }
                if (cover > 0.0f) {   // terra: tinta preta, com a granulação do papel aparecendo um pouco
                    float grain = vnoise(u * 520.0f, v * 520.0f, seed + 91u);
                    float ink = 0.90f + 0.10f * grain;
                    float ir = mixf(r * 0.20f, 20.0f, 0.8f) * ink, ig = mixf(g * 0.18f, 15.0f, 0.8f) * ink, ib = mixf(b * 0.16f, 11.0f, 0.8f) * ink;
                    float a = cover * reveal;
                    r = mixf(r, ir, a); g = mixf(g, ig, a); b = mixf(b, ib, a);
                }
            }
            o[0] = (uint8_t)std::max(0.0f, std::min(255.0f, r)); o[1] = (uint8_t)std::max(0.0f, std::min(255.0f, g)); o[2] = (uint8_t)std::max(0.0f, std::min(255.0f, b)); o[3] = 255;
        }
    }
    // marcadores (cores de tinta: vermelho-sangue para você, azul-ferrete para o navio, sépia para os outros)
    const float pxPerM = (float)res / (2.0f * V.half);
    const float sz = std::max(5.0f, res * 0.030f);
    for (const MapMark& m : marks) {
        float mx = (m.x - V.cx) * pxPerM + res * 0.5f, my = (m.z - V.cz) * pxPerM + res * 0.5f;
        if (std::sqrt((m.x - V.cx) * (m.x - V.cx) + (m.z - V.cz) * (m.z - V.cz)) > V.viewRadius) continue;
        float fx = std::sin(m.yaw), fz = -std::cos(m.yaw);   // frente no mundo: (sin, -cos); na textura z cresce para baixo
        if (m.kind == 0) {   // seta: ponta na frente, duas asas atrás, com contorno claro para destacar sobre a tinta
            float s = sz * 1.15f, rx = -fz, rz = fx;
            float tx = mx + fx * s * 1.1f, ty = my + fz * s * 1.1f;
            float lx = mx - fx * s * 0.8f + rx * s * 0.75f, ly = my - fz * s * 0.8f + rz * s * 0.75f;
            float ex = mx - fx * s * 0.8f - rx * s * 0.75f, ey = my - fz * s * 0.8f - rz * s * 0.75f;
            float nx = mx - fx * s * 0.25f, ny = my - fz * s * 0.25f;
            fillTri(out, res, tx + fx * 1.6f, ty + fz * 1.6f, lx - rx * 1.4f - fx * 1.4f, ly - rz * 1.4f - fz * 1.4f, ex + rx * 1.4f - fx * 1.4f, ey + rz * 1.4f - fz * 1.4f, 238, 224, 190, 0.95f);   // contorno claro
            fillTri(out, res, tx, ty, lx, ly, nx, ny, 150, 24, 18, 1.0f);
            fillTri(out, res, tx, ty, nx, ny, ex, ey, 112, 14, 10, 1.0f);
        } else if (m.kind == 1) {   // navio: casco em losango com mastro
            float s = sz * 1.2f, rx = -fz, rz = fx;
            disc(out, res, mx, my, s * 0.95f, 238, 224, 190, 0.9f);
            fillTri(out, res, mx + fx * s, my + fz * s, mx + rx * s * 0.55f, my + rz * s * 0.55f, mx - fx * s * 0.8f, my - fz * s * 0.8f, 30, 52, 86, 1.0f);
            fillTri(out, res, mx + fx * s, my + fz * s, mx - fx * s * 0.8f, my - fz * s * 0.8f, mx - rx * s * 0.55f, my - rz * s * 0.55f, 22, 40, 70, 1.0f);
        } else {
            disc(out, res, mx, my, sz * 0.62f, 238, 224, 190, 0.95f);
            disc(out, res, mx, my, sz * 0.42f, 110, 60, 20, 1.0f);
        }
    }
}

void renderPaper(int res, bool circle, bool folds, std::vector<uint8_t>& out, uint32_t seed) {
    out.assign((size_t)res * res * 4, 255);
    for (int y = 0; y < res; ++y) for (int x = 0; x < res; ++x) {
        uint8_t* o = &out[((size_t)y * res + x) * 4];
        float u = (x + 0.5f) / res, v = (y + 0.5f) / res;
        if (circle && std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) > 0.5f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        float r, g, b; paper(u, v, seed, circle, folds, r, g, b);
        o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; o[3] = 255;
    }
}

template <class TT>
static void renderInkT(const TT& T, const MapView& V, float revealX, float revealZ, std::vector<uint8_t>& out, uint32_t seed);

void renderInk(const MapView& V, float revealX, float revealZ, std::vector<uint8_t>& out, uint32_t seed, const e0net::TerrainSnapshot* snap) {
    if (snap) renderInkT(*snap, V, revealX, revealZ, out, seed);
    else renderInkT(e0net::terrain(), V, revealX, revealZ, out, seed);
}

template <class TT>
static void renderInkT(const TT& T, const MapView& V, float revealX, float revealZ, std::vector<uint8_t>& out, uint32_t seed) {
    const int res = V.res;
    out.assign((size_t)res * res * 4, 0);
    if (!T.hasMain()) return;
    const float mPerPx = 2.0f * V.half / (float)res;
    const float eps = std::max(2.0f, mPerPx * 0.85f);
    // Paleta de aquarela cartográfica por bioma (estilo Sea of Thieves / AC Black Flag: terra quente sobre oceano azul-petróleo envelhecido)
    static const float kBio[7][3] = {
        {126.0f, 146.0f, 134.0f},  // 0 SeaFloor
        {226.0f, 198.0f, 136.0f},  // 1 Sand (praia dourada quente)
        {178.0f, 142.0f,  92.0f},  // 2 Dirt (terra ocre)
        {152.0f, 162.0f,  96.0f},  // 3 Grass (oliva-primavera envelhecido)
        { 94.0f, 118.0f,  68.0f},  // 4 Forest (pinho profundo)
        {138.0f, 124.0f, 102.0f},  // 5 Rock (rocha/penhasco sépia)
        {192.0f, 174.0f, 138.0f},  // 6 Pad (ruínas/construções)
    };
    for (int y = 0; y < res; ++y) for (int x = 0; x < res; ++x) {
        float u = (x + 0.5f) / res, v = (y + 0.5f) / res;
        float wx = V.cx + (u - 0.5f) * 2.0f * V.half, wz = V.cz + (v - 0.5f) * 2.0f * V.half;
        float dist = std::sqrt((wx - revealX) * (wx - revealX) + (wz - revealZ) * (wz - revealZ));
        // borda orgânica de aquarela na fronteira de exploração (no mapa grande)
        float edgeWobble = (V.res >= 512) ? (vnoise(wx * 0.0045f, wz * 0.0045f, seed + 503u) - 0.5f) * V.viewRadius * 0.16f : 0.0f;
        float reveal = 1.0f - smooth(V.viewRadius * V.fadeFrac, V.viewRadius, dist + edgeWobble);
        if (reveal <= 0.0f) continue;

        float h0 = T.heightAt(wx, wz);
        uint8_t* o = &out[((size_t)y * res + x) * 4];

        // Subamostragem suave na linha de costa
        float cover;
        const float band = std::max(2.5f, mPerPx * 0.65f);
        if (h0 > -band && h0 < band) {
            const float s = 0.28f * mPerPx;
            cover = ((T.heightAt(wx - s, wz - s) > 0.0f) + (T.heightAt(wx + s, wz - s) > 0.0f) +
                     (T.heightAt(wx - s, wz + s) > 0.0f) + (T.heightAt(wx + s, wz + s) > 0.0f)) * 0.25f;
        } else cover = h0 > 0.0f ? 1.0f : 0.0f;

        if (cover > 0.0f) {
            // Aquarela cartográfica por bioma (filtragem 4-pontos suave para transição orgânica sem bloquinhos de 2m no zoom)
            const float bs = std::max(1.4f, mPerPx * 0.55f);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            int forestCnt = 0;
            const float ox[4] = {-bs, bs, -bs, bs}, oz[4] = {-bs, -bs, bs, bs};
            for (int k = 0; k < 4; ++k) {
                uint8_t bk = T.biomeAt(wx + ox[k], wz + oz[k]);
                if (bk == 0 || bk >= 7) bk = 1;
                if (bk == 4) ++forestCnt;
                r += kBio[bk][0]; g += kBio[bk][1]; b += kBio[bk][2];
            }
            r *= 0.25f; g *= 0.25f; b *= 0.25f;
            // relevo direcional (hillshade NW -> SE)
            float hx1 = T.heightAt(wx + eps, wz), hx0 = T.heightAt(wx - eps, wz);
            float hz1 = T.heightAt(wx, wz + eps), hz0 = T.heightAt(wx, wz - eps);
            float dx = (hx1 - hx0) / (2.0f * eps), dz = (hz1 - hz0) / (2.0f * eps);
            float shade = clamp01(0.55f + (-dx * 0.72f - dz * 0.72f) * 0.85f);
            float hLight = 0.76f + 0.42f * shade + clamp01(h0 / 28.0f) * 0.14f;
            r *= hLight; g *= hLight; b *= hLight;

            // textura orgânica de aquarela + pontilhado sutil de árvores nas florestas
            float grain = vnoise(wx * 0.09f, wz * 0.09f, seed + 91u);
            float fine  = vnoise(wx * 0.28f, wz * 0.28f, seed + 137u);
            float wash  = 0.91f + 0.16f * grain;
            r *= wash; g *= wash; b *= wash;
            if (forestCnt >= 2 && fine > 0.65f) {   // copas sutis em tinta oliva-escura
                float tk = (fine - 0.65f) * 1.45f * ((float)forestCnt * 0.25f);
                r = mixf(r, 54.0f, tk * 0.48f); g = mixf(g, 66.0f, tk * 0.48f); b = mixf(b, 40.0f, tk * 0.48f);
            }

            // curvas de nível topográficas a cada 4 m na terra (tinta sépia fina)
            if (h0 > 1.8f) {
                float c4 = h0 / 4.0f, cf = c4 - std::floor(c4);
                float cl = (1.0f - smooth(0.0f, 0.09f, cf)) + (cf > 0.91f ? smooth(0.91f, 1.0f, cf) : 0.0f);
                if (cl > 0.01f) { r = mixf(r, 58.0f, cl * 0.42f); g = mixf(g, 40.0f, cl * 0.42f); b = mixf(b, 22.0f, cl * 0.42f); }
            }

            // contorno nítido de costa em tinta nanquim/sépia escura (nas bordas da ilha)
            float minNb = std::min(std::min(hx0, hx1), std::min(hz0, hz1));
            float coast = (cover < 0.99f ? 0.90f : 0.0f);
            if (minNb <= 0.15f || h0 < 0.85f) coast = std::max(coast, 1.0f - smooth(0.0f, 1.15f, std::max(0.0f, h0)));
            if (coast > 0.02f) {
                r = mixf(r, 34.0f, coast * 0.88f); g = mixf(g, 22.0f, coast * 0.88f); b = mixf(b, 12.0f, coast * 0.88f);
            }

            o[0] = (uint8_t)std::max(0.0f, std::min(255.0f, r));
            o[1] = (uint8_t)std::max(0.0f, std::min(255.0f, g));
            o[2] = (uint8_t)std::max(0.0f, std::min(255.0f, b));
            o[3] = (uint8_t)(255.0f * std::max(cover, coast * 0.7f) * reveal * 0.96f);
        } else if (h0 < 0.0f && h0 > -16.0f) {
            // Batimetria náutica no raso (transição suave do turquesa claro da costa até o verde-petróleo do mar aberto + anéis finos)
            float depth = -h0;
            float shallow = 1.0f - smooth(0.0f, 15.0f, depth);
            float step = mPerPx > 5.0f ? 3.2f : 2.2f;
            float dRing = depth / step, f = dRing - std::floor(dRing);
            float wRing = mPerPx > 5.0f ? 0.16f : 0.11f;
            float line = (1.0f - smooth(0.0f, wRing, f)) + (f > (1.0f - wRing) ? smooth(1.0f - wRing, 1.0f, f) : 0.0f);
            float ripple = line * (1.0f - smooth(1.5f, 14.0f, depth)) * 0.44f;
            float rimDark = (1.0f - smooth(0.0f, 1.5f, depth)) * 0.62f;
            float baseA = mixf(0.36f, 0.44f, shallow);
            float a = std::max(baseA, std::max(ripple, rimDark)) * reveal;
            if (a > 0.01f) {
                // cor base: mistura de mar profundo (azul-petróleo antigo) com raso (turquesa-espuma claro)
                float wr = mixf(74.0f, 148.0f, shallow), wg = mixf(114.0f, 184.0f, shallow), wb = mixf(118.0f, 174.0f, shallow);
                float kInk = clamp01(std::max(ripple, rimDark) / std::max(0.01f, a));
                float rr = mixf(wr, 48.0f, kInk), gg = mixf(wg, 44.0f, kInk), bb = mixf(wb, 34.0f, kInk);
                o[0] = (uint8_t)rr; o[1] = (uint8_t)gg; o[2] = (uint8_t)bb;
                o[3] = (uint8_t)std::min(255.0f, 255.0f * a);
            }
        } else {
            // Mar aberto revelado: aguada azul-petróleo/ardósia envelhecida (contraste real com ilhas e pergaminho inexplorado) + ondinhas náuticas
            float curr = fbm(wx * 0.0055f, wz * 0.0055f, seed + 211u, 3);
            float seaA = (0.31f + 0.11f * curr) * reveal;
            // micro-ondas cartográficas esparsas em células de 180 x 120 m (visíveis no mapa grande)
            float waveInk = 0.0f;
            if (V.res >= 512) {
                const float cw = 180.0f, ch = 120.0f;
                int gx = (int)std::floor(wx / cw), gz = (int)std::floor(wz / ch);
                uint32_t hh = hash2((uint32_t)gx, (uint32_t)gz, seed + 409u);
                if ((hh & 3u) == 0u) {
                    float ox = (0.28f + 0.44f * ((hh >> 8) & 0xFF) / 255.0f) * cw;
                    float oz = (0.28f + 0.44f * ((hh >> 16) & 0xFF) / 255.0f) * ch;
                    float lx = (wx - (float)gx * cw) - ox;
                    float lz = (wz - (float)gz * ch) - oz;
                    const float hw = 34.0f;
                    if (std::fabs(lx) < hw && std::fabs(lz) < 16.0f) {
                        float ph = (lx + hw) / (2.0f * hw) * 3.14159265f * 2.0f;
                        float curve = -std::fabs(std::sin(ph)) * 5.2f + 2.0f;
                        float env = 1.0f - smooth(hw * 0.72f, hw, std::fabs(lx));
                        float lw = std::max(2.6f, mPerPx * 1.15f);
                        float d1 = std::fabs(lz - curve);
                        float d2 = std::fabs((lz - 7.4f) - curve * 0.72f);
                        float s1 = (1.0f - smooth(0.0f, lw, d1)) * env;
                        float s2 = (1.0f - smooth(0.0f, lw, d2)) * env * (1.0f - smooth(hw * 0.45f, hw * 0.78f, std::fabs(lx)));
                        waveInk = std::max(s1, s2 * 0.72f) * 0.34f * reveal;
                    }
                }
            }
            // transição suave para pergaminho sépia cru nas bordas de mares inexplorados
            float fogEdge = smooth(V.viewRadius * 0.76f, V.viewRadius * 0.96f, dist + edgeWobble) * reveal;
            float rr = mixf(70.0f, 134.0f, fogEdge), gg = mixf(110.0f, 108.0f, fogEdge), bb = mixf(114.0f, 74.0f, fogEdge);
            if (waveInk > 0.01f) {
                rr = mixf(rr, 42.0f, 0.78f); gg = mixf(gg, 62.0f, 0.78f); bb = mixf(bb, 64.0f, 0.78f);
            }
            o[0] = (uint8_t)rr; o[1] = (uint8_t)gg; o[2] = (uint8_t)bb;
            o[3] = (uint8_t)std::min(255.0f, 255.0f * std::max(std::max(seaA, waveInk), fogEdge * 0.20f));
        }
    }
}

struct MapWorker::Impl {
    struct Job { bool pending = false, paper = false, circle = false, folds = false; int res = 0; MapView v; float rx = 0, rz = 0;
                 std::shared_ptr<const e0net::TerrainSnapshot> snap; };
    struct Out { bool ready = false; MapView v; std::vector<uint8_t> px; };
    Job jobs[4]; Out outs[4]; bool running[4] = {false, false, false, false};
    std::mutex m; std::condition_variable cv; bool quit = false; std::thread th;
    void loop() {
        for (;;) {
            int slot = -1; Job j;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { if (quit) return true; for (int i = 0; i < 4; ++i) if (jobs[i].pending) return true; return false; });
                if (quit) return;
                for (int i = 0; i < 4; ++i) if (jobs[i].pending) { slot = i; break; }
                j = jobs[slot]; jobs[slot].pending = false; running[slot] = true;
            }
            std::vector<uint8_t> px;
            if (j.paper) renderPaper(j.res, j.circle, j.folds, px);
            else if (j.snap) renderInk(j.v, j.rx, j.rz, px, 0x5EED1234u, j.snap.get());
            else px.assign((size_t)j.v.res * j.v.res * 4, 0);   // sem cópia das ilhas: tinta vazia (nunca lê o Terrain global daqui)
            std::lock_guard<std::mutex> l(m);
            outs[slot].px.swap(px); outs[slot].v = j.v; outs[slot].v.res = j.paper ? j.res : j.v.res; outs[slot].ready = true; running[slot] = false;
        }
    }
};
MapWorker::MapWorker() : d_(new Impl) { d_->th = std::thread([this] { d_->loop(); }); }
MapWorker::~MapWorker() { { std::lock_guard<std::mutex> l(d_->m); d_->quit = true; } d_->cv.notify_all(); if (d_->th.joinable()) d_->th.join(); delete d_; }
void MapWorker::requestPaper(int slot, int res, bool circle, bool folds) {
    { std::lock_guard<std::mutex> l(d_->m); Impl::Job& j = d_->jobs[slot]; j = Impl::Job(); j.pending = true; j.paper = true; j.res = res; j.circle = circle; j.folds = folds; }
    d_->cv.notify_one();
}
void MapWorker::requestInk(int slot, const MapView& v, float rx, float rz, std::shared_ptr<const e0net::TerrainSnapshot> snap) {
    { std::lock_guard<std::mutex> l(d_->m); Impl::Job& j = d_->jobs[slot]; j = Impl::Job(); j.pending = true; j.v = v; j.rx = rx; j.rz = rz; j.snap = std::move(snap); }
    d_->cv.notify_one();
}
bool MapWorker::take(int slot, MapView& v, std::vector<uint8_t>& px) {
    std::lock_guard<std::mutex> l(d_->m);
    Impl::Out& o = d_->outs[slot];
    if (!o.ready) return false;
    o.ready = false; v = o.v; px.swap(o.px);
    return true;
}
bool MapWorker::busy(int slot) const { std::lock_guard<std::mutex> l(d_->m); return d_->jobs[slot].pending || d_->running[slot]; }

}  // namespace e0
