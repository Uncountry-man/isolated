#include "terrain_renderer.h"
#include "../../shared/ruins.h"
#include "props_data.h"

#include <GLES3/gl3.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdint>
#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated", __VA_ARGS__)

using namespace e0net;

namespace e0 {

namespace {

constexpr int kChunkCells = 32, kChunkVerts = 33, kChunksPerSide = World::kCells / kChunkCells;  // 15
constexpr int kSkirtVerts = 4 * kChunkVerts;
constexpr float kSkirtDepth = 6.0f;

struct Vtx {  // 20 bytes
    float x, y, z;
    int8_t nx, ny, nz, nw;
    uint8_t r, g, b, a;
};

uint32_t hash2(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
int8_t packN(float v) { return (int8_t)std::lround(std::fmax(-1.0f, std::fmin(1.0f, v)) * 127.0f); }
uint8_t packC(float v) { return (uint8_t)std::lround(std::fmax(0.0f, std::fmin(1.0f, v)) * 255.0f); }

struct Col { float r, g, b; };

// ---- planos do frustum (Gribb/Hartmann) ----
struct Frustum {
    float p[6][4];
    explicit Frustum(const Mat4& M) {
        const float* m = M.m;
        auto row = [&](int i, float* o) { o[0] = m[i]; o[1] = m[4 + i]; o[2] = m[8 + i]; o[3] = m[12 + i]; };
        float r0[4], r1[4], r2[4], r3[4];
        row(0, r0); row(1, r1); row(2, r2); row(3, r3);
        for (int k = 0; k < 4; ++k) {
            p[0][k] = r3[k] + r0[k]; p[1][k] = r3[k] - r0[k];
            p[2][k] = r3[k] + r1[k]; p[3][k] = r3[k] - r1[k];
            p[4][k] = r3[k] + r2[k]; p[5][k] = r3[k] - r2[k];
        }
        for (auto& pl : p) { float l = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]); for (float& v : pl) v /= l; }
    }
    bool sphere(float x, float y, float z, float r) const {
        for (const auto& pl : p) if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < -r) return false;
        return true;
    }
};

// ---- construtor de malhas de objetos (sombreamento chapado) ----
struct Builder {
    std::vector<Vtx>& out;
    float s;
    Vec3 ax, ay, az;              // eixos X, Y, Z da base ortonormal 3D
    float ox, oy, oz;             // posição no mundo
    Vec3 center;                  // centro (mundo) p/ orientar as normais para fora
    uint8_t alpha = 255;          // <255 = material brilhante no shader (252 = ouro, 240 = diamante)

    Builder(std::vector<Vtx>& out_, float s_, float ca, float sa, float ox_, float oy_, float oz_)
        : out(out_), s(s_), ax{ca, 0.0f, -sa}, ay{0.0f, 1.0f, 0.0f}, az{sa, 0.0f, ca}, ox(ox_), oy(oy_), oz(oz_), center{} {}

    Vec3 T(const Vec3& p) const {
        return {
            ox + (ax.x * p.x + ay.x * p.y + az.x * p.z) * s,
            oy + (ax.y * p.x + ay.y * p.y + az.y * p.z) * s,
            oz + (ax.z * p.x + ay.z * p.y + az.z * p.z) * s
        };
    }
    Vec3 Tn(const Vec3& n) const {
        return {
            ax.x * n.x + ay.x * n.y + az.x * n.z,
            ax.y * n.x + ay.y * n.y + az.y * n.z,
            ax.z * n.x + ay.z * n.y + az.z * n.z
        };
    }
    // Árvores: as raízes (base do tronco, até ~1 m de altura) acompanham o relevo em volta, em vez de ficarem no nível do centro (flutuando em barrancos).
    const World* conform = nullptr;
    void prop(const PropModel& m, float tint = 1.0f) {
        for (size_t i = 0; i < m.count; ++i) {
            const PropVertex& pv = m.vertices[i];
            Vec3 p = T({pv.x, pv.y, pv.z});
            if (conform && pv.y < 1.25f) {
                float t = std::min(1.0f, std::max(0.0f, (pv.y - 0.35f) / 0.9f));
                float wgt = 1.0f - t * t * (3.0f - 2.0f * t);                  // 1 no chão, 0 a ~1,1 m
                float gnd = conform->heightAt(p.x, p.z);
                float want = gnd - 0.30f + pv.y * s;                           // o bulbo da raiz fica ~30 cm enterrado no chão local (antes flutuava sobre o relevo)
                p.y += (want - p.y) * wgt;
            }
            Vec3 n = Tn({pv.nx / 127.0f, pv.ny / 127.0f, pv.nz / 127.0f});
            out.push_back({
                p.x, p.y, p.z,
                packN(n.x), packN(n.y), packN(n.z), 0,
                packC((pv.r / 255.0f) * tint),
                packC((pv.g / 255.0f) * tint),
                packC((pv.b / 255.0f) * tint),
                255
            });
        }
    }
    void tri(const Vec3& la, const Vec3& lb, const Vec3& lc, const Col& c, float shade = 1.0f) {
        Vec3 a = T(la), b = T(lb), cc = T(lc);
        Vec3 n = normalize(cross(b - a, cc - a));
        Vec3 mid = (a + b + cc) * (1.0f / 3.0f);
        if (dot(n, mid - center) < 0) { n = n * -1.0f; std::swap(b, cc); }
        for (const Vec3* v : {&a, &b, &cc})
            out.push_back({v->x, v->y, v->z, packN(n.x), packN(n.y), packN(n.z), 0, packC(c.r * shade), packC(c.g * shade), packC(c.b * shade), alpha});
    }
    void quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Col& col) { tri(a, b, c, col); tri(a, c, d, col); }
    // caixa alinhada ao eixo local [x0,x1]x[y0,y1]x[z0,z1]; topo/base opcionais
    void box(float x0, float y0, float z0, float x1, float y1, float z1, const Col& c, bool top = true, bool bottom = false) {
        Vec3 v[8] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
        quad(v[0], v[1], v[5], v[4], c); quad(v[1], v[2], v[6], v[5], c); quad(v[2], v[3], v[7], v[6], c); quad(v[3], v[0], v[4], v[7], c);
        if (top) quad(v[4], v[5], v[6], v[7], c);
        if (bottom) quad(v[0], v[1], v[2], v[3], c);
    }
    // cone de N lados (base em y0 com raio r, ponta em y1)
    void cone(float cx, float cz, float y0, float y1, float r, int sides, const Col& c) {
        for (int i = 0; i < sides; ++i) {
            float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            tri({cx + r * std::cos(a0), y0, cz + r * std::sin(a0)}, {cx + r * std::cos(a1), y0, cz + r * std::sin(a1)}, {cx, y1, cz}, c, 0.92f + 0.08f * (i & 1));
        }
    }
    void prism(float cx, float cz, float y0, float y1, float r, int sides, const Col& c) {  // tronco
        for (int i = 0; i < sides; ++i) {
            float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            Vec3 b0{cx + r * std::cos(a0), y0, cz + r * std::sin(a0)}, b1{cx + r * std::cos(a1), y0, cz + r * std::sin(a1)};
            Vec3 t0{b0.x, y1, b0.z}, t1{b1.x, y1, b1.z};
            quad(b0, b1, t1, t0, c);
        }
    }
    // cilindro fino de `sides` lados entre a e b (locais), raios ra -> rb (galhos, gravetos)
    void seg(const Vec3& a, const Vec3& b, float ra, float rb, int sides, const Col& c) {
        Vec3 axis = normalize(b - a);
        Vec3 up = std::fabs(axis.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 u = normalize(cross(axis, up)), v = cross(axis, u);
        Vec3 saved = center;
        center = T((a + b) * 0.5f);
        for (int i = 0; i < sides; ++i) {
            float a0 = 6.2831853f * i / sides, a1 = 6.2831853f * (i + 1) / sides;
            Vec3 d0 = u * std::cos(a0) + v * std::sin(a0), d1 = u * std::cos(a1) + v * std::sin(a1);
            float sh = 0.9f + 0.1f * (i & 1);
            quad(a + d0 * ra, a + d1 * ra, b + d1 * rb, b + d0 * rb, {c.r * sh, c.g * sh, c.b * sh});
        }
        center = saved;
    }
    // pedra / copa: icosaedro deformado por hash, centrado em (cx,cy,cz), semi-eixos (rx,ry,rz)
    void blob(float cx, float cy, float cz, float rx, float ry, float rz, uint32_t seed, float jitter, const Col& c, float darkBottom) {
        static const float t = 1.6180339f;
        static const float V[12][3] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
        static const int F[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                     {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        Vec3 P[12];
        for (int i = 0; i < 12; ++i) {
            float l = std::sqrt(V[i][0] * V[i][0] + V[i][1] * V[i][1] + V[i][2] * V[i][2]);
            float j = 1.0f - jitter + 2.0f * jitter * ((hash2(seed, (uint32_t)i) & 0xFFFF) / 65535.0f);
            P[i] = {cx + V[i][0] / l * rx * j, cy + V[i][1] / l * ry * j, cz + V[i][2] / l * rz * j};
        }
        for (int f = 0; f < 20; ++f) {
            float yc = (P[F[f][0]].y + P[F[f][1]].y + P[F[f][2]].y) / 3.0f;
            tri(P[F[f][0]], P[F[f][1]], P[F[f][2]], c, yc < cy ? darkBottom : 1.0f);
        }
    }
};

inline Vec3 terrainNormal(const World& w, float x, float z, float eps = 0.35f) {
    float hL = w.heightAt(x - eps, z);
    float hR = w.heightAt(x + eps, z);
    float hD = w.heightAt(x, z - eps);
    float hU = w.heightAt(x, z + eps);
    float dh_dx = (hR - hL) / (2.0f * eps);
    float dh_dz = (hU - hD) / (2.0f * eps);
    return normalize(Vec3{-dh_dx, 1.0f, -dh_dz});
}

// ---- RUÍNAS: cabana, galpão e torre são gerados SÓ como ruínas (o gerador do mundo não muda: mesmos tipos/posições/hash; só o visual e a colisão) ----
// Paredes quebradas (alturas em degraus, brechas), pilares/postes partidos, telhado e vigas caídos, entulho e uma laje no chão. Aleatoriedade por hash
// da posição (visual, só no cliente). `kind`: 0 cabana, 1 galpão, 2 torre.
float rr(uint32_t seed, int i) { return ruinRand(seed, i); }   // idêntico ao usado pela colisão (shared/ruins.h)

void rbox(Builder& b, float x0, float y0, float z0, float x1, float y1, float z1, const Col& c, bool top = true) {
    Vec3 saved = b.center;
    b.center = b.T({(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f});   // normais para fora DESTA caixa (as paredes são vistas por dentro também)
    b.box(x0, y0, z0, x1, y1, z1, c, top, true);
    b.center = saved;
}

// parede quebrada de (x0,z0) a (x1,z1) (alinhada a um eixo): trechos de ~0,55 m com altura em passeio aleatório e brechas
void ruinWall(Builder& b, float x0, float z0, float x1, float z1, float maxH, float th, uint32_t seed, int salt, const Col& col, float gapChance) {
    const bool alongX = std::fabs(x1 - x0) >= std::fabs(z1 - z0);
    (void)alongX;
    ruinWallSegs(seed, salt, x0, z0, x1, z1, maxH, th, gapChance,
                 [&](int k, float r, float bx0, float bz0, float bx1, float bz1, float hh, float cx, float cz) {
        float sh = 0.82f + 0.3f * rr(seed, salt * 91 + k);
        Col c{col.r * sh, col.g * sh, col.b * sh};
        rbox(b, bx0, 0.0f, bz0, bx1, hh, bz1, c);
        if (hh > 0.5f && rr(seed, salt * 13 + k) < 0.35f)   // farpa: ponta quebrada mais alta e fina
            rbox(b, cx - 0.07f, hh, cz - th * 0.6f, cx + 0.07f, hh + 0.25f + 0.3f * r, cz + th * 0.6f, c);
    });
}

void buildRuin(Builder& b, const WorldObject& o, uint32_t seed, float hx, float hz, float wh, const Col& wall, int kind) {
    (void)o;
    const Col slab{0.36f, 0.34f, 0.31f}, rubble{0.42f, 0.40f, 0.37f}, wood{0.33f, 0.24f, 0.15f}, roof{0.33f, 0.16f, 0.13f};
    b.center = b.T({0, 0.5f, 0});
    // laje/fundação rachada no chão (pedaços)
    if (kind != 2) {
        for (int i = 0; i < 6; ++i) {
            float px0 = -hx + (2.0f * hx) * (i % 3) / 3.0f, pz0 = -hz + (2.0f * hz) * (i / 3) / 2.0f;
            float w = 2.0f * hx / 3.0f - 0.05f, d = hz - 0.05f;
            if (rr(seed, 400 + i) < 0.22f) continue;     // pedaço sumido
            float sh = 0.85f + 0.25f * rr(seed, 410 + i);
            rbox(b, px0, -0.02f, pz0, px0 + w, 0.07f + 0.04f * rr(seed, 420 + i), pz0 + d, {slab.r * sh, slab.g * sh, slab.b * sh});
        }
    } else {
        rbox(b, -1.4f, -0.05f, -1.4f, 1.4f, 0.14f, 1.4f, slab);
    }
    if (kind != 2) {
        // 4 paredes, cada uma quebrada de um jeito; a de trás é a mais alta
        ruinWall(b, -hx, -hz, hx, -hz, wh * 0.95f, 0.11f, seed, 1, wall, 0.12f);
        ruinWall(b, -hx, hz, hx, hz, wh * 0.45f, 0.11f, seed, 2, wall, 0.30f);
        ruinWall(b, -hx, -hz, -hx, hz, wh * 0.70f, 0.11f, seed, 3, wall, 0.20f);
        ruinWall(b, hx, -hz, hx, hz, wh * 0.55f, 0.11f, seed, 4, wall, 0.25f);
        // cantos: pilares partidos em alturas diferentes
        for (int i = 0; i < 4; ++i) {
            float cx = (i & 1) ? hx : -hx, cz = (i & 2) ? hz : -hz, ph = ruinPillarH(kind, seed, i);
            float sh = 0.8f + 0.25f * rr(seed, 510 + i);
            rbox(b, cx - 0.16f, 0.0f, cz - 0.16f, cx + 0.16f, ph, cz + 0.16f, {wall.r * sh * 0.85f, wall.g * sh * 0.85f, wall.b * sh * 0.85f});
        }
        if (kind == 0) {   // cabana: chaminé de pedra meio desmoronada + batente da porta
            float ch = ruinChimneyH(seed);
            rbox(b, 1.0f, 0.0f, -0.6f, 1.6f, ch, 0.0f, {0.45f, 0.42f, 0.40f});
            rbox(b, 1.15f, ch, -0.5f, 1.5f, ch + 0.25f, -0.1f, {0.45f, 0.42f, 0.40f});
            rbox(b, -0.50f, 0.0f, hz - 0.06f, -0.40f, 1.8f, hz + 0.06f, wood);
            rbox(b, 0.40f, 0.0f, hz - 0.06f, 0.50f, 0.9f, hz + 0.06f, wood);
        } else {           // galpão: portão de ferro caído e chapas
            rbox(b, -1.2f, 0.05f, hz + 0.3f, 1.2f, 0.12f, hz + 2.3f, {0.30f, 0.30f, 0.30f});
        }
        // telhado desabado: duas águas que despencaram (uma meio apoiada na parede de trás, outra no chão)
        float rh = 0.9f;
        b.center = b.T({0, 0.8f, -hz * 0.3f});
        b.quad({-hx - 0.2f, wh * 0.85f, -hz - 0.2f}, {hx * 0.3f, wh * 0.85f, -hz - 0.2f}, {hx * 0.3f, 0.45f, 0.1f}, {-hx - 0.2f, 0.45f, 0.1f}, roof);
        b.center = b.T({hx * 0.4f, 0.3f, hz * 0.4f});
        b.quad({hx * 0.1f, 0.35f, 0.2f}, {hx + 0.3f, 0.2f, 0.4f}, {hx + 0.3f, 0.18f, hz + 0.3f}, {hx * 0.1f, 0.30f, hz + 0.1f}, {roof.r * 0.9f, roof.g * 0.9f, roof.b * 0.9f});
        (void)rh;
        // vigas caídas (cilindros): algumas apoiadas nas paredes, outras no chão
        for (int i = 0; i < 5; ++i) {
            float bx = (rr(seed, 700 + i) - 0.5f) * 1.6f * hx, bz = (rr(seed, 710 + i) - 0.5f) * 1.6f * hz;
            float ang = rr(seed, 720 + i) * 6.2831853f, ln = 1.2f + 1.6f * rr(seed, 730 + i), lift = rr(seed, 740 + i) < 0.4f ? 0.6f : 0.12f;
            b.seg({bx - std::cos(ang) * ln * 0.5f, 0.10f, bz - std::sin(ang) * ln * 0.5f}, {bx + std::cos(ang) * ln * 0.5f, lift, bz + std::sin(ang) * ln * 0.5f}, 0.07f, 0.06f, 6, wood);
        }
    } else {
        // torre: 4 postes partidos; só dois altos seguram um pedaço da plataforma; escada/vigas caídas no chão
        const float px[4] = {-0.8f, 0.8f, -0.8f, 0.8f}, pz[4] = {-0.8f, -0.8f, 0.8f, 0.8f};
        float ph[4];
        for (int i = 0; i < 4; ++i) {
            ph[i] = ruinPostH(seed, i);
            rbox(b, px[i] - 0.14f, 0.0f, pz[i] - 0.14f, px[i] + 0.14f, ph[i], pz[i] + 0.14f, wall);
            rbox(b, px[i] - 0.08f, ph[i], pz[i] - 0.08f, px[i] + 0.08f, ph[i] + 0.35f, pz[i] + 0.20f, wall);   // ponta lascada
        }
        float py = std::min(ph[0], ph[2]) - 0.2f;
        rbox(b, -1.1f, py, -1.1f, 0.2f, py + 0.28f, 1.1f, wood);             // pedaço da plataforma (lado dos postes altos)
        rbox(b, -1.1f, py + 0.28f, -1.1f, -0.95f, py + 1.0f, 1.1f, wood);     // resto do guarda-corpo
        for (int i = 0; i < 3; ++i) {   // travessas diagonais restantes
            float y0 = 0.8f + 1.2f * i;
            if (y0 > ph[0] - 0.6f) break;
            b.seg({-0.8f, y0, -0.8f}, {-0.8f, y0 + 1.3f, 0.8f}, 0.05f, 0.05f, 5, wood);
        }
        // plataforma caída no chão: tábuas e vigas espalhadas
        for (int i = 0; i < 7; ++i) {
            float bx = (rr(seed, 900 + i) - 0.5f) * 3.6f, bz = (rr(seed, 910 + i) - 0.5f) * 3.6f, ang = rr(seed, 920 + i) * 6.2831853f, ln = 0.9f + 1.6f * rr(seed, 930 + i);
            b.seg({bx - std::cos(ang) * ln * 0.5f, 0.10f, bz - std::sin(ang) * ln * 0.5f}, {bx + std::cos(ang) * ln * 0.5f, 0.10f + 0.3f * rr(seed, 940 + i), bz + std::sin(ang) * ln * 0.5f}, 0.07f, 0.06f, 6, wood);
        }
    }
    // entulho: pedras e tábuas ao redor
    for (int i = 0; i < 9; ++i) {
        float ang = rr(seed, 1000 + i) * 6.2831853f, rad = (0.4f + 0.75f * rr(seed, 1010 + i)) * std::max(hx, hz) * (kind == 2 ? 1.6f : 1.05f);
        float sz = 0.14f + 0.22f * rr(seed, 1020 + i);
        float sh = 0.8f + 0.3f * rr(seed, 1030 + i);
        b.blob(std::cos(ang) * rad * (hx / std::max(hx, hz)), sz * 0.45f, std::sin(ang) * rad * (hz / std::max(hx, hz)), sz, sz * 0.7f, sz * 0.9f, seed + (uint32_t)i, 0.25f,
               {rubble.r * sh, rubble.g * sh, rubble.b * sh}, 0.7f);
    }
}

// ---- MINÉRIOS: ouro (pepitas amarelas) e diamante (cristais azuis) saindo da superfície da pedra ----
// Os pontos de saída são vértices da parte de cima do modelo da pedra (escolhidos por hash); o brilho vem do alfa dos vértices (ver kPropMain).
void crystal(Builder& b, const Vec3& base, const Vec3& dir, float len, float rad, const Col& c, uint32_t seed) {
    Vec3 up = std::fabs(dir.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 u = normalize(cross(dir, up)), v = cross(dir, u);
    Vec3 saved = b.center;
    const int N = 6;
    Vec3 r0[N], r1[N];
    for (int i = 0; i < N; ++i) {
        float a = 6.2831853f * i / N + 0.4f * ruinRand(seed, 3000 + i);
        Vec3 d = u * std::cos(a) + v * std::sin(a);
        r0[i] = base + d * rad;
        r1[i] = base + dir * (len * 0.62f) + d * (rad * 0.85f);
    }
    Vec3 tip = base + dir * len;
    b.center = b.T(base + dir * (len * 0.45f));
    for (int i = 0; i < N; ++i) {
        int j = (i + 1) % N;
        float sh = 0.78f + 0.3f * ((i * 5 + 1) % 4) / 3.0f;
        Col f{c.r * sh, c.g * sh, c.b * sh};
        b.quad(r0[i], r0[j], r1[j], r1[i], f);
        b.tri(r1[i], r1[j], tip, {std::min(1.0f, f.r * 1.25f), std::min(1.0f, f.g * 1.2f), std::min(1.0f, f.b * 1.1f)});
    }
    b.center = saved;
}

void buildOre(Builder& b, const PropModel& rock, uint32_t seed, int ore) {
    // candidatos: vértices da metade de cima (normal para cima/para o lado)
    std::vector<int> cand;
    for (size_t i = 0; i < rock.count; ++i) if (rock.vertices[i].ny > 20 && rock.vertices[i].y > 0.25f) cand.push_back((int)i);
    if (cand.empty()) return;
    const uint8_t savedA = b.alpha;
    b.alpha = ore == 2 ? 240 : (ore == 3 ? 255 : 252);   // ferro: sem brilho
    const int n = ore == 2 ? 5 : 6;
    for (int k = 0; k < n; ++k) {
        const PropVertex& pv = rock.vertices[(size_t)cand[ruinHash(seed, 4000u + (uint32_t)k) % cand.size()]];
        Vec3 nrm = normalize(Vec3{pv.nx / 127.0f, pv.ny / 127.0f, pv.nz / 127.0f});
        Vec3 p{pv.x, pv.y, pv.z};
        Vec3 dir = normalize(nrm + Vec3{0, 0.55f, 0});
        if (ore == 2) {          // diamante: cristais longos e finos, azul-claro reluzente
            float len = 0.36f + 0.34f * ruinRand(seed, 4100 + k), rad = 0.055f + 0.05f * ruinRand(seed, 4200 + k);
            Col c = (k & 1) ? Col{0.30f, 0.72f, 1.00f} : Col{0.48f, 0.88f, 1.00f};
            crystal(b, p - nrm * 0.07f, dir, len, rad, c, seed + (uint32_t)k);
            if (k < 2) {         // cristais menores ao lado, inclinados
                Vec3 d2 = normalize(dir + Vec3{0.45f * (k ? 1 : -1), 0, 0.3f});
                crystal(b, p - nrm * 0.05f, d2, len * 0.55f, rad * 0.8f, {0.55f, 0.9f, 1.0f}, seed + 77u + (uint32_t)k);
            }
        } else if (ore == 3) {   // ferro: veios/nódulos de ferrugem alaranhada e cinza-aço escuro, foscos
            float r = 0.09f + 0.10f * ruinRand(seed, 4400 + k);
            Vec3 c0 = p + nrm * (r * 0.30f);
            b.blob(c0.x, c0.y, c0.z, r * 1.3f, r * 0.8f, r, seed + 53u * (uint32_t)k, 0.45f, (k & 1) ? Col{0.66f, 0.36f, 0.20f} : Col{0.30f, 0.31f, 0.35f}, 0.6f);
            if (k < 3) b.blob(c0.x + 0.12f * nrm.z, c0.y, c0.z - 0.12f * nrm.x, r * 0.8f, r * 0.55f, r * 0.7f, seed + 97u * (uint32_t)k, 0.4f, Col{0.52f, 0.27f, 0.16f}, 0.6f);
        } else {                 // ouro: pepitas amarelas irregulares coladas à pedra
            float r = 0.11f + 0.11f * ruinRand(seed, 4300 + k);
            Vec3 c0 = p + nrm * (r * 0.35f);
            b.blob(c0.x, c0.y, c0.z, r * 1.25f, r * 0.9f, r, seed + 31u * (uint32_t)k, 0.38f, (k & 1) ? Col{1.0f, 0.78f, 0.14f} : Col{0.92f, 0.66f, 0.08f}, 0.8f);
        }
    }
    b.alpha = savedA;
}

// TECIDO (lixo trazido pelo mar): trapo amassado e molhado deitado na areia. Dois panos de contorno irregular (leque de triângulos com o miolo
// mais alto e a borda ondulada); o de cima é menor e mais claro (dobra). Cor por variante: sujo/branco, azul desbotado, vermelho desbotado, cinza.
void buildCloth(Builder& b, uint32_t seed, int variant) {
    static const Col kCols[4] = {{0.92f, 0.88f, 0.76f}, {0.50f, 0.68f, 0.82f}, {0.80f, 0.34f, 0.28f}, {0.50f, 0.66f, 0.38f}};   // branco sujo, azul, vermelho e verde desbotados
    const Col base = kCols[variant & 3];
    b.center = b.T({0.0f, -1.0f, 0.0f});          // normais para CIMA (o centro padrão fica acima do objeto)
    auto h01 = [&](uint32_t k) { return (float)(hash2(seed ^ (k * 0x9E3779B1u), k + 7u) & 0xFFFF) / 65535.0f; };
    for (int layer = 0; layer < 2; ++layer) {
        const int n = 9; Vec3 pts[n]; float rad0 = layer == 0 ? 0.26f : 0.15f, rad1 = layer == 0 ? 0.16f : 0.09f;
        float ox = layer == 0 ? 0.0f : (h01(50) - 0.5f) * 0.12f, oz = layer == 0 ? 0.0f : (h01(51) - 0.5f) * 0.12f, y0 = layer == 0 ? 0.004f : 0.020f;
        for (int i = 0; i < n; ++i) {
            float ang = 6.2831853f * (i + 0.35f * (h01(10 + i + layer * 20) - 0.5f)) / n, r = rad0 + rad1 * h01(30 + i + layer * 20);
            pts[i] = {ox + r * std::cos(ang), y0 + 0.022f * h01(70 + i + layer * 20) * (layer ? 1.4f : 1.0f), oz + r * std::sin(ang)};
        }
        Vec3 c{ox, y0 + (layer == 0 ? 0.045f : 0.035f) + 0.015f * h01(90 + layer), oz};
        Col col{base.r * (layer ? 1.10f : 1.0f), base.g * (layer ? 1.10f : 1.0f), base.b * (layer ? 1.10f : 1.0f)};
        for (int i = 0; i < n; ++i) b.tri(pts[i], pts[(i + 1) % n], c, col, 0.92f + 0.16f * h01(110 + i + layer * 20));
    }
}

void buildObject(std::vector<Vtx>& out, const WorldObject& o, const World& w, int ore = 0) {
    float yaw = o.rot * (6.2831853f / 256.0f), s = o.scale100 * 0.01f;
    float cx = o.xcm * 0.01f, cz = o.zcm * 0.01f;
    float oy = o.ycm * 0.01f - 0.05f;
    Builder b{out, s, std::cos(yaw), std::sin(yaw), cx, oy, cz};

    if (o.type == ObjLog) {  // tronco caído: alinhado à inclinação do terreno e sempre acima da superfície
        float dx, dz; World::logDir(o, dx, dz);
        float halfL = 2.30f * s;
        Vec3 pA{cx - dx * halfL, 0.0f, cz - dz * halfL};
        Vec3 pB{cx + dx * halfL, 0.0f, cz + dz * halfL};
        float hA = w.heightAt(pA.x, pA.z);
        float hB = w.heightAt(pB.x, pB.z);

        Vec3 axis{pB.x - pA.x, hB - hA, pB.z - pA.z};
        b.ax = normalize(axis);

        Vec3 N = terrainNormal(w, cx, cz, 0.5f);
        Vec3 up = N - b.ax * dot(N, b.ax);
        b.ay = normalize(dot(up, up) > 1e-4f ? up : Vec3{0, 1, 0});
        b.az = normalize(cross(b.ax, b.ay));

        float max_req_y = -1e9f;
        const int kSamples = 11;
        for (int i = 0; i < kSamples; ++i) {
            float u = (float)i / (kSamples - 1);
            float t = -halfL + 2.0f * halfL * u;

            Vec3 p_mid = b.ax * t + b.ay * (0.02f * s);
            float gh_mid = w.heightAt(cx + p_mid.x, cz + p_mid.z);
            max_req_y = std::max(max_req_y, gh_mid - p_mid.y);

            Vec3 p_left = b.ax * t - b.az * (0.35f * s) + b.ay * (0.08f * s);
            float gh_left = w.heightAt(cx + p_left.x, cz + p_left.z);
            max_req_y = std::max(max_req_y, gh_left - p_left.y);

            Vec3 p_right = b.ax * t + b.az * (0.35f * s) + b.ay * (0.08f * s);
            float gh_right = w.heightAt(cx + p_right.x, cz + p_right.z);
            max_req_y = std::max(max_req_y, gh_right - p_right.y);
        }
        b.oy = max_req_y + 0.015f;  // repousa sobre o ponto mais alto com folga de 1.5 cm (nunca afundado)
    } else if (o.type == ObjStick) {  // graveto coletável: inclinado com o relevo e sempre acima do terreno
        float hx = std::cos(yaw), hz = -std::sin(yaw);
        float halfL = 0.30f;
        Vec3 pA{cx - hx * halfL * s, 0.0f, cz - hz * halfL * s};
        Vec3 pB{cx + hx * halfL * s, 0.0f, cz + hz * halfL * s};
        float hA = w.heightAt(pA.x, pA.z);
        float hB = w.heightAt(pB.x, pB.z);

        Vec3 axis{pB.x - pA.x, hB - hA, pB.z - pA.z};
        b.ax = normalize(axis);

        Vec3 N = terrainNormal(w, cx, cz, 0.25f);
        Vec3 up = N - b.ax * dot(N, b.ax);
        b.ay = normalize(dot(up, up) > 1e-4f ? up : Vec3{0, 1, 0});
        b.az = normalize(cross(b.ax, b.ay));

        float max_req_y = -1e9f;
        const int kSamples = 9;
        for (int i = 0; i < kSamples; ++i) {
            float u = (float)i / (kSamples - 1);
            float lx = (-halfL + 2.0f * halfL * u);

            Vec3 p_mid = (b.ax * lx) * s;
            float gh_mid = w.heightAt(cx + p_mid.x, cz + p_mid.z);
            max_req_y = std::max(max_req_y, gh_mid - p_mid.y);

            Vec3 p_l = (b.ax * lx - b.az * 0.025f) * s;
            float gh_l = w.heightAt(cx + p_l.x, cz + p_l.z);
            max_req_y = std::max(max_req_y, gh_l - p_l.y);

            Vec3 p_r = (b.ax * lx + b.az * 0.025f) * s;
            float gh_r = w.heightAt(cx + p_r.x, cz + p_r.z);
            max_req_y = std::max(max_req_y, gh_r - p_r.y);
        }
        b.oy = max_req_y + 0.025f;  // repousa visivelmente sobre a inclinação com +2.5 cm de folga (nunca afundado)
    } else if (o.type == ObjPebble || o.type == ObjCloth) {  // pedrinhas e tecidos coletáveis: orientados pela normal do relevo e visíveis
        float hx = std::cos(yaw), hz = -std::sin(yaw);
        Vec3 dir{hx, 0.0f, hz};

        Vec3 N = terrainNormal(w, cx, cz, 0.25f);
        b.ay = N;
        Vec3 tan_dir = dir - N * dot(dir, N);
        if (dot(tan_dir, tan_dir) < 1e-4f) {
            Vec3 fallback = std::fabs(N.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            tan_dir = cross(fallback, N);
        }
        b.ax = normalize(tan_dir);
        b.az = normalize(cross(b.ax, b.ay));

        float rad = (o.type == ObjCloth ? 0.32f : 0.15f) * s;
        float max_req_y = -1e9f;
        Vec3 base_center = b.ay * (0.02f * s);
        float gh_center = w.heightAt(cx + base_center.x, cz + base_center.z);
        max_req_y = std::max(max_req_y, gh_center - base_center.y);

        const float angles[4] = {0.0f, 1.5707963f, 3.1415926f, 4.712389f};
        for (float ang : angles) {
            Vec3 p = (b.ax * std::cos(ang) + b.az * std::sin(ang)) * rad + base_center;
            float gh = w.heightAt(cx + p.x, cz + p.z);
            max_req_y = std::max(max_req_y, gh - p.y);
        }
        b.oy = max_req_y + 0.008f;  // repousa sobre a inclinação com 8 mm de folga
    }

    b.center = b.T({0, 1.0f, 0});
    uint32_t seed = hash2((uint32_t)o.xcm, (uint32_t)o.zcm);
    float tint = 0.9f + 0.2f * ((seed & 0xFF) / 255.0f);
    switch (o.type) {
        case ObjPalm: {
            b.conform = &w;
            b.prop(kPropPalm, tint);
            break;
        }
        case ObjCoconut: {
            b.conform = &w;
            b.prop(kPropCoconut, tint);
            break;
        }
        case ObjRock: {
            b.prop(kPropRocks[(seed >> 8) % 3], tint);
            if (ore) buildOre(b, kPropRocks[(seed >> 8) % 3], seed, ore);
            break;
        }
        case ObjCabin: buildRuin(b, o, seed, 2.3f, 1.8f, 2.4f, {0.40f, 0.30f, 0.20f}, 0); break;
        case ObjShed: buildRuin(b, o, seed, 3.4f, 2.4f, 3.0f, {0.46f, 0.46f, 0.44f}, 1); break;
        case ObjStick: {
            b.prop(kPropStick, (o.variant & 4) ? tint * 1.55f : tint);   // bit 2 = madeira clara de praia
            break;
        }
        case ObjPebble: {
            int variant = (int)(o.variant % 3);
            b.prop(kPropPebbles[variant], tint);
            break;
        }
        case ObjCloth: buildCloth(b, seed, (int)(o.variant & 3)); break;
        case ObjLog: {
            b.prop(kPropLog, tint);
            break;
        }
        default: buildRuin(b, o, seed, 1.5f, 1.5f, 6.0f, {0.38f, 0.27f, 0.17f}, 2); break;   // torre de vigia em ruínas
    }
}

}  // namespace

// Malha de um tufo: lâminas estilizadas otimizadas (Grass.fbx), y em [0,1] = altura relativa. Dois lados visíveis (sem culling).
// A malha extraída do FBX (kTuftMeshVerts) ficou com triângulos soltos/deitados após a decimação (lâminas "voando" de lado),
// então o tufo é montado aqui: 7 lâminas (1 triângulo cada = 21 vértices), base no chão (y=0), ponta em cima (y<=1), em leque,
// cada uma levemente inclinada para fora. Mantém kTuftVertCount = 21 (mesmo draw call).
static std::vector<float> tuftMesh() {
    static_assert(kTuftVertCount == 21, "tufo = 7 lâminas x 3 vértices");
    std::vector<float> m;
    const float kTwoPi = 6.2831853f;
    for (int i = 0; i < 7; ++i) {
        float a = kTwoPi * (float)i / 7.0f + 0.35f * (float)((i * 5) % 3);   // leque com pequena irregularidade
        float dx = std::cos(a), dz = std::sin(a), tx = -dz, tz = dx;           // direção radial e tangente
        float r0 = 0.03f + 0.04f * (float)(i % 3);                              // base afastada do centro
        float h = i == 0 ? 1.0f : 0.62f + 0.12f * (float)((i * 3) % 4);        // alturas variadas (máx. 1)
        float lean = 0.05f + 0.07f * h, w = 0.032f;                             // inclinação da ponta e meia largura da base
        float cx = dx * r0, cz = dz * r0;
        const float v[9] = {cx - tx * w, 0.0f, cz - tz * w,  cx + tx * w, 0.0f, cz + tz * w,  cx + dx * lean, h, cz + dz * lean};
        m.insert(m.end(), v, v + 9);
    }
    return m;
}

void TerrainRenderer::destroy() {
    if (tuftVbo_) { glDeleteBuffers(1, &tuftVbo_); tuftVbo_ = 0; }
    for (Chunk& c : chunks_) {
        if (!c.valid) continue;
        glDeleteBuffers(1, &c.vbo); glDeleteBuffers(3, c.ibo); glDeleteVertexArrays(3, c.vao);
        if (c.pvbo) { glDeleteBuffers(1, &c.pvbo); glDeleteVertexArrays(1, &c.pvao); }
        if (c.gvbo) { glDeleteBuffers(1, &c.gvbo); glDeleteVertexArrays(1, &c.gvao); }
        if (c.svbo) { glDeleteBuffers(1, &c.svbo); glDeleteVertexArrays(1, &c.svao); }
    }
    chunks_.clear();
}

void TerrainRenderer::build(const World& w) {
    destroy();
    const float OX = w.oxcm * 0.01f, OZ = w.ozcm * 0.01f;   // origem da ilha no mundo (0 na inicial)
    chunks_.resize((size_t)kChunksPerSide * kChunksPerSide);
    auto H = [&](int ix, int iz) { return w.vertexHeight(std::max(0, std::min(World::kCells, ix)), std::max(0, std::min(World::kCells, iz))); };

    // objetos por chunk
    std::vector<std::vector<Vtx>> props(chunks_.size()), smalls(chunks_.size());
    smallRef_.assign(w.objects.size(), SmallRef{});
    propRef_.assign(w.objects.size(), SmallRef{});
    src_ = &w;
    for (size_t oi = 0; oi < w.objects.size(); ++oi) {
        const WorldObject& o = w.objects[oi];
        int cx = std::max(0, std::min(kChunksPerSide - 1, (o.xcm - w.oxcm + World::kHalfCm) / (kChunkCells * 200))), cz = std::max(0, std::min(kChunksPerSide - 1, (o.zcm - w.ozcm + World::kHalfCm) / (kChunkCells * 200)));
        const int ci = cz * kChunksPerSide + cx;
        if (World::isCollectible(o.type)) {
            std::vector<Vtx>& sv = smalls[(size_t)ci];
            int first = (int)sv.size();
            buildObject(sv, o, w);
            smallRef_[oi] = {ci, first, (int)sv.size() - first};
        } else {
            std::vector<Vtx>& pvv = props[(size_t)ci];
            int pfirst = (int)pvv.size();
            buildObject(pvv, o, w, o.type == ObjRock ? w.oreOf((uint32_t)oi) : 0);
            propRef_[oi] = {ci, pfirst, (int)pvv.size() - pfirst};
        }
    }

    {   // malha do tufo (uma vez)
        std::vector<float> tm = tuftMesh();
        glGenBuffers(1, &tuftVbo_);
        glBindBuffer(GL_ARRAY_BUFFER, tuftVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tm.size() * 4), tm.data(), GL_STATIC_DRAW);
    }
    struct Tuft { float x, y, z; uint8_t scale, rot, tint, pad; };
    long grassTotal = 0;

    int built = 0; size_t vtxBytes = 0, propBytes = 0;
    for (int cz = 0; cz < kChunksPerSide; ++cz)
        for (int cx = 0; cx < kChunksPerSide; ++cx) {
            Chunk& c = chunks_[(size_t)cz * kChunksPerSide + cx];
            float mn = 1e9f, mx = -1e9f;
            for (int j = 0; j < kChunkVerts; ++j) for (int i = 0; i < kChunkVerts; ++i) { float h = H(cx * kChunkCells + i, cz * kChunkCells + j); mn = std::min(mn, h); mx = std::max(mx, h); }
            if (mx < -2.4f) continue;  // todo abaixo da camada opaca do mar (-2.2 m): não desenha (economiza memória e draw calls)
            c.valid = true; c.minH = mn; c.maxH = mx;
            c.cx = (cx * kChunkCells + kChunkCells * 0.5f) * World::kCell - World::kHalf + OX;
            c.cz = (cz * kChunkCells + kChunkCells * 0.5f) * World::kCell - World::kHalf + OZ;
            c.cy = (mn + mx) * 0.5f;
            float half = kChunkCells * World::kCell * 0.5f;
            c.radius = std::sqrt(2.0f * half * half + (mx - mn) * (mx - mn) * 0.25f) + 10.0f;  // +10 m: copas das árvores

            std::vector<Vtx> v((size_t)kChunkVerts * kChunkVerts + kSkirtVerts);
            auto getBiomeWeights = [&](int x, int z, float& ws, float& wd, float& wg, float& wr) {
                int cx_clamped = std::max(0, std::min(World::kCells, x));
                int cz_clamped = std::max(0, std::min(World::kCells, z));
                uint8_t bm = w.biome[(size_t)cz_clamped * World::kVerts + cx_clamped];
                ws = wd = wg = wr = 0.0f;
                switch (bm) {
                    case BiomeSand:   ws = 1.0f; break;
                    case BiomeDirt:   wd = 1.0f; break;
                    case BiomeGrass:  wg = 1.0f; break;
                    case BiomeForest: wd = 0.65f; wg = 0.35f; break;
                    case BiomeRock:   wr = 1.0f; break;
                    case BiomeSeaFloor: ws = 1.0f; break;
                    case BiomePad:    wd = 0.75f; wr = 0.25f; break;
                    default:          wg = 1.0f; break;
                }
            };

            for (int j = 0; j < kChunkVerts; ++j)
                for (int i = 0; i < kChunkVerts; ++i) {
                    int ix = cx * kChunkCells + i, iz = cz * kChunkCells + j;
                    float h = H(ix, iz);
                    float nx = (H(ix - 1, iz) - H(ix + 1, iz)) / (2 * World::kCell), nz = (H(ix, iz - 1) - H(ix, iz + 1)) / (2 * World::kCell);
                    float nl = std::sqrt(nx * nx + 1.0f + nz * nz);
                    uint8_t b = w.biome[(size_t)iz * World::kVerts + ix];

                    float ws, wd, wg, wr;
                    getBiomeWeights(ix, iz, ws, wd, wg, wr);
                    float sWs = ws * 0.6f, sWd = wd * 0.6f, sWg = wg * 0.6f, sWr = wr * 0.6f;
                    const int ddx[4] = {-1, 1, 0, 0}, ddz[4] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; ++d) {
                        float nws, nwd, nwg, nwr;
                        getBiomeWeights(ix + ddx[d], iz + ddz[d], nws, nwd, nwg, nwr);
                        sWs += nws * 0.1f; sWd += nwd * 0.1f; sWg += nwg * 0.1f; sWr += nwr * 0.1f;
                    }
                    ws = sWs; wd = sWd; wg = sWg; wr = sWr;

                    // Barrancos e encostas íngremes expõem a rocha (Layer 3)
                    float slope = std::sqrt(nx * nx + nz * nz);
                    if (slope > 0.50f && b != BiomeSand) {
                        float rk = std::min(1.0f, (slope - 0.50f) / 0.35f);
                        float inv = 1.0f - rk;
                        ws *= inv; wd *= inv; wg *= inv;
                        wr = wr * inv + rk;
                    }
                    float wTot = ws + wd + wg + wr;
                    if (wTot > 1e-4f) {
                        float invTot = 1.0f / wTot;
                        ws *= invTot; wd *= invTot; wg *= invTot; wr *= invTot;
                    } else {
                        wg = 1.0f;
                    }

                    Vtx& o = v[(size_t)j * kChunkVerts + i];
                    o = {ix * World::kCell - World::kHalf + OX, h, iz * World::kCell - World::kHalf + OZ, packN(nx / nl), packN(1.0f / nl), packN(nz / nl), 0,
                         packC(ws), packC(wd), packC(wg), packC(wr)};
                }
            for (int k = 0; k < kChunkVerts; ++k) {  // saias: cópias das bordas, mais baixas
                Vtx a = v[(size_t)0 * kChunkVerts + k], bt = v[(size_t)(kChunkVerts - 1) * kChunkVerts + k];
                Vtx l = v[(size_t)k * kChunkVerts + 0], r = v[(size_t)k * kChunkVerts + kChunkVerts - 1];
                a.y -= kSkirtDepth; bt.y -= kSkirtDepth; l.y -= kSkirtDepth; r.y -= kSkirtDepth;
                size_t base = (size_t)kChunkVerts * kChunkVerts;
                v[base + k] = a; v[base + kChunkVerts + k] = bt; v[base + 2 * kChunkVerts + k] = l; v[base + 3 * kChunkVerts + k] = r;
            }
            glGenBuffers(1, &c.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(Vtx)), v.data(), GL_STATIC_DRAW);
            vtxBytes += v.size() * sizeof(Vtx);

            const int steps[3] = {1, 2, 4};
            glGenBuffers(3, c.ibo); glGenVertexArrays(3, c.vao);
            for (int L = 0; L < 3; ++L) {
                int s = steps[L];
                std::vector<uint16_t> ind;
                auto V = [&](int i, int j) { return (uint16_t)(j * kChunkVerts + i); };
                for (int j = 0; j < kChunkCells; j += s)
                    for (int i = 0; i < kChunkCells; i += s) {  // triangulação igual à de World::heightAt (diagonal 00-11, CCW virado para +Y)
                        ind.push_back(V(i, j)); ind.push_back(V(i + s, j + s)); ind.push_back(V(i + s, j));
                        ind.push_back(V(i, j)); ind.push_back(V(i, j + s)); ind.push_back(V(i + s, j + s));
                    }
                const int sb = kChunkVerts * kChunkVerts;
                for (int e = 0; e < 4; ++e)  // saias nas 4 bordas
                    for (int k = 0; k < kChunkCells; k += s) {
                        uint16_t a, b2;
                        if (e == 0) { a = V(k, 0); b2 = V(k + s, 0); }
                        else if (e == 1) { a = V(k, kChunkCells); b2 = V(k + s, kChunkCells); }
                        else if (e == 2) { a = V(0, k); b2 = V(0, k + s); }
                        else { a = V(kChunkCells, k); b2 = V(kChunkCells, k + s); }
                        uint16_t sa = (uint16_t)(sb + e * kChunkVerts + k), sb2 = (uint16_t)(sb + e * kChunkVerts + k + s);
                        ind.push_back(a); ind.push_back(b2); ind.push_back(sb2); ind.push_back(a); ind.push_back(sb2); ind.push_back(sa);
                    }
                c.idx[L] = (int)ind.size();
                glBindVertexArray(c.vao[L]);
                glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
                glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
                glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(Vtx), (void*)12);
                glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)16);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c.ibo[L]);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(ind.size() * 2), ind.data(), GL_STATIC_DRAW);
                glBindVertexArray(0);
            }
            std::vector<Vtx>& pv = props[(size_t)cz * kChunksPerSide + cx];
            if (!pv.empty()) {
                c.pcount = (int)pv.size();
                glGenBuffers(1, &c.pvbo); glGenVertexArrays(1, &c.pvao);
                glBindVertexArray(c.pvao);
                glBindBuffer(GL_ARRAY_BUFFER, c.pvbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pv.size() * sizeof(Vtx)), pv.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
                glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(Vtx), (void*)12);
                glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)16);
                glBindVertexArray(0);
                propBytes += pv.size() * sizeof(Vtx);
            }
            std::vector<Vtx>& sv = smalls[(size_t)cz * kChunksPerSide + cx];
            if (!sv.empty()) {   // coletáveis num buffer à parte (alcance de desenho menor)
                c.scount = (int)sv.size();
                glGenBuffers(1, &c.svbo); glGenVertexArrays(1, &c.svao);
                glBindVertexArray(c.svao);
                glBindBuffer(GL_ARRAY_BUFFER, c.svbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sv.size() * sizeof(Vtx)), sv.data(), GL_DYNAMIC_DRAW);
                c.scpu.assign((const uint8_t*)sv.data(), (const uint8_t*)(sv.data() + sv.size()));
                glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
                glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(Vtx), (void*)12);
                glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)16);
                glBindVertexArray(0);
                propBytes += sv.size() * sizeof(Vtx);
            }
            // ---- grama: grade com jitter de 1.4 m sobre grama/floresta/terra/areia (hash => igual em todos os aparelhos) ----
            {
                std::vector<std::pair<uint32_t, Tuft>> tufts;
                const int cell = 140, x0 = (cx * kChunkCells * 200), z0 = (cz * kChunkCells * 200), n = kChunkCells * 200 / cell;
                for (int gz = 0; gz < n; ++gz)
                    for (int gx = 0; gx < n; ++gx) {
                        uint32_t hv = hash2((uint32_t)(x0 / cell + gx), (uint32_t)(z0 / cell + gz) ^ 0x6A55u);
                        int32_t xcm = x0 + gx * cell + (int32_t)(hv & 0x3FF) * cell / 1024 - World::kHalfCm + w.oxcm;
                        int32_t zcm = z0 + gz * cell + (int32_t)((hv >> 10) & 0x3FF) * cell / 1024 - World::kHalfCm + w.ozcm;
                        float x = xcm * 0.01f, z = zcm * 0.01f;
                        uint8_t bm = w.biomeAt(x, z);
                        int pct = bm == BiomeGrass ? 88 : (bm == BiomeForest ? 48 : (bm == BiomeDirt ? 8 : (bm == BiomeSand ? 5 : 0)));
                        if ((int)((hv >> 20) % 100u) >= pct) continue;
                        float h = w.heightAt(x, z);
                        if (h < 0.3f) continue;
                        if (std::fabs(w.heightAt(x + 0.6f, z) - h) > 0.5f || std::fabs(w.heightAt(x, z + 0.6f) - h) > 0.5f) continue;   // não em barrancos
                        float px = x, pz = z; w.pushOut(px, pz, 0.12f);
                        if (std::fabs(px - x) + std::fabs(pz - z) > 0.02f) continue;                                                     // não dentro de troncos/pedras
                        uint32_t h2 = hash2(hv, 0x51u);
                        Tuft t{x, h - 0.02f, z, (uint8_t)(h2 & 0xFF), (uint8_t)((h2 >> 8) & 0xFF), (uint8_t)((h2 >> 16) & 0xFF), 0};
                        tufts.push_back({hash2(hv, 0x77u), t});
                    }
                if (!tufts.empty()) {
                    std::sort(tufts.begin(), tufts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });  // ordem embaralhada: um prefixo = amostra uniforme
                    std::vector<Tuft> inst; inst.reserve(tufts.size());
                    for (auto& t : tufts) inst.push_back(t.second);
                    c.gcount = (int)inst.size();
                    grassTotal += c.gcount;
                    glGenBuffers(1, &c.gvbo); glGenVertexArrays(1, &c.gvao);
                    glBindVertexArray(c.gvao);
                    glBindBuffer(GL_ARRAY_BUFFER, tuftVbo_);
                    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
                    glBindBuffer(GL_ARRAY_BUFFER, c.gvbo);
                    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(inst.size() * sizeof(Tuft)), inst.data(), GL_STATIC_DRAW);
                    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Tuft), (void*)0);  glVertexAttribDivisor(1, 1);
                    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Tuft), (void*)12); glVertexAttribDivisor(2, 1);
                    glBindVertexArray(0);
                }
            }
            ++built;
        }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    LOG("terreno: %d chunks (de %zu), %.1f MB de vértices + %.1f MB de objetos (%zu objetos), %ld tufos de grama", built, chunks_.size(), vtxBytes / 1048576.0, propBytes / 1048576.0, w.objects.size(), grassTotal);
}

void TerrainRenderer::drawTerrainImpl(const Params& P, Counts& C, bool forceLow) {
    Frustum fr(P.vp);
    for (size_t ci = 0; ci < chunks_.size(); ++ci) {
        const Chunk& c = chunks_[ci];
        if (!c.valid) continue;
        float dx = c.cx - P.cam.x, dz = c.cz - P.cam.z, d = std::sqrt(dx * dx + dz * dz);
        if (d - 46.0f > P.viewDist) { ++C.culled; continue; }
        if (!fr.sphere(c.cx, c.cy, c.cz, c.radius)) { ++C.culled; continue; }
        // LOD0 (malha de 2 m = a mesma superfície de World::heightAt) vale até a BORDA PRÓXIMA do chunk (d - 32): os coletáveis (gravetos, pedrinhas, tecidos) são
        // assentados sobre essa superfície exata; numa malha mais grossa (LOD1 = 4 m) até 10 cm de relevo some e eles ficavam ENTERRADOS. drawSmall só alcança
        // chunks com d <= smallDist + 46 (<= 131 m), sempre dentro deste LOD0 (>= lod0 + 32 = 87 m na pior qualidade).
        int lod = forceLow ? 2 : ((d - 32.0f < P.lod0) ? 0 : (d < P.lod1 ? 1 : 2));
        glBindVertexArray(c.vao[lod]);
        glDrawElements(GL_TRIANGLES, c.idx[lod], GL_UNSIGNED_SHORT, nullptr);
        ++C.chunks; C.tris += c.idx[lod] / 3;
    }
    glBindVertexArray(0);
}
void TerrainRenderer::drawTerrain(const Params& p, Counts& c) { drawTerrainImpl(p, c, false); }
void TerrainRenderer::drawTerrainLow(const Params& p, Counts& c) { drawTerrainImpl(p, c, true); }

void TerrainRenderer::drawProps(const Params& P, Counts& C) {
    Frustum fr(P.vp);
    for (const Chunk& c : chunks_) {
        if (!c.valid || !c.pcount) continue;
        float dx = c.cx - P.cam.x, dz = c.cz - P.cam.z, d = std::sqrt(dx * dx + dz * dz);
        if (d - 46.0f > P.propDist) continue;
        if (!fr.sphere(c.cx, c.cy, c.cz, c.radius)) continue;
        glBindVertexArray(c.pvao);
        glDrawArrays(GL_TRIANGLES, 0, c.pcount);
        ++C.propChunks; C.propTris += c.pcount / 3;
    }
    glBindVertexArray(0);
}

void TerrainRenderer::setSmallHidden(uint32_t objIdx, bool hidden) {
    if (objIdx >= smallRef_.size()) return;
    const SmallRef& r = smallRef_[objIdx];
    if (r.chunk < 0 || r.count <= 0 || r.chunk >= (int)chunks_.size()) return;
    Chunk& c = chunks_[(size_t)r.chunk];
    if (!c.svbo || (size_t)(r.first + r.count) * sizeof(Vtx) > c.scpu.size()) return;
    const uint8_t* src = c.scpu.data() + (size_t)r.first * sizeof(Vtx);
    std::vector<Vtx> tmp((size_t)r.count);
    std::memcpy(tmp.data(), src, tmp.size() * sizeof(Vtx));
    if (hidden) for (Vtx& v : tmp) v = tmp[0];   // triângulos degenerados (área zero): não geram fragmentos
    glBindBuffer(GL_ARRAY_BUFFER, c.svbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)r.first * sizeof(Vtx)), (GLsizeiptr)(tmp.size() * sizeof(Vtx)), tmp.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TerrainRenderer::hideProp(uint32_t objIdx) {
    if (objIdx >= propRef_.size()) return;
    const SmallRef& r = propRef_[objIdx];
    if (r.chunk < 0 || r.count <= 0 || r.chunk >= (int)chunks_.size()) return;
    Chunk& c = chunks_[(size_t)r.chunk];
    if (!c.pvbo || r.first + r.count > c.pcount) return;
    std::vector<Vtx> z((size_t)r.count);
    std::memset(z.data(), 0, z.size() * sizeof(Vtx));   // todos no mesmo ponto: triângulos degenerados
    glBindBuffer(GL_ARRAY_BUFFER, c.pvbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)r.first * sizeof(Vtx)), (GLsizeiptr)(z.size() * sizeof(Vtx)), z.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TerrainRenderer::showProp(uint32_t objIdx) {
    if (!src_ || objIdx >= propRef_.size() || objIdx >= src_->objects.size()) return;
    const SmallRef& r = propRef_[objIdx];
    if (r.chunk < 0 || r.count <= 0 || r.chunk >= (int)chunks_.size()) return;
    Chunk& c = chunks_[(size_t)r.chunk];
    if (!c.pvbo || r.first + r.count > c.pcount) return;
    const WorldObject& o = src_->objects[objIdx];
    std::vector<Vtx> v;
    buildObject(v, o, *src_, o.type == ObjRock ? src_->oreOf(objIdx) : 0);
    if ((int)v.size() != r.count) return;
    glBindBuffer(GL_ARRAY_BUFFER, c.pvbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)r.first * sizeof(Vtx)), (GLsizeiptr)(v.size() * sizeof(Vtx)), v.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool TerrainRenderer::propGhost(uint32_t objIdx, std::vector<uint8_t>& ghost, Vec3& base) const {
    if (!src_ || objIdx >= src_->objects.size()) return false;
    const WorldObject& o = src_->objects[objIdx];
    std::vector<Vtx> v;
    buildObject(v, o, *src_, o.type == ObjRock ? src_->oreOf(objIdx) : 0);
    ghost.assign((const uint8_t*)v.data(), (const uint8_t*)(v.data() + v.size()));
    base = {o.xcm * 0.01f, o.ycm * 0.01f - 0.05f, o.zcm * 0.01f};
    return !v.empty();
}

void TerrainRenderer::drawSmall(const Params& P, Counts& C) {
    Frustum fr(P.vp);
    for (const Chunk& c : chunks_) {
        if (!c.valid || !c.scount) continue;
        float dx = c.cx - P.cam.x, dz = c.cz - P.cam.z, d = std::sqrt(dx * dx + dz * dz);
        if (d - 46.0f > P.smallDist) continue;
        if (!fr.sphere(c.cx, c.cy, c.cz, c.radius)) continue;
        glBindVertexArray(c.svao);
        glDrawArrays(GL_TRIANGLES, 0, c.scount);
        ++C.smallChunks; C.smallTris += c.scount / 3;
    }
    glBindVertexArray(0);
}

void TerrainRenderer::drawGrass(const Params& P, Counts& C) {
    Frustum fr(P.vp);
    for (const Chunk& c : chunks_) {
        if (!c.valid || !c.gcount) continue;
        float dx = c.cx - P.cam.x, dz = c.cz - P.cam.z, d = std::sqrt(dx * dx + dz * dz);
        if (d - 46.0f > P.grassDist) continue;
        if (!fr.sphere(c.cx, c.cy, c.cz, c.radius)) continue;
        int n = std::max(1, (int)(c.gcount * P.grassDensity));
        glBindVertexArray(c.gvao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, (GLsizei)kTuftVertCount, n);
        ++C.grassChunks; C.grassTufts += n;
    }
    glBindVertexArray(0);
}

void TerrainRenderer::drawPropsAround(const Vec3& f, float radius, Counts& C) {
    for (const Chunk& c : chunks_) {
        if (!c.valid || !c.pcount) continue;
        float dx = c.cx - f.x, dz = c.cz - f.z;
        float lim = radius + 46.0f;
        if (dx * dx + dz * dz > lim * lim) continue;
        glBindVertexArray(c.pvao);
        glDrawArrays(GL_TRIANGLES, 0, c.pcount);
        ++C.propChunks; C.propTris += c.pcount / 3;
    }
    glBindVertexArray(0);
}

}  // namespace e0
