#include "world.h"
#include "ruins.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace e0net {

// Direções dos troncos: 64 ângulos em Q14 (constantes: idênticas em qualquer plataforma, sem sin/cos em runtime)
static const int16_t kLogCos[64] = {16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665, 11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606, 0, -1606, -3196, -4756, -6270, -7723, -9102, -10394, -11585, -12665, -13623, -14449, -15137, -15679, -16069, -16305, -16384, -16305, -16069, -15679, -15137, -14449, -13623, -12665, -11585, -10394, -9102, -7723, -6270, -4756, -3196, -1606, 0, 1606, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305};
static const int16_t kLogSin[64] = {0, 1606, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305, 16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665, 11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606, 0, -1606, -3196, -4756, -6270, -7723, -9102, -10394, -11585, -12665, -13623, -14449, -15137, -15679, -16069, -16305, -16384, -16305, -16069, -15679, -15137, -14449, -13623, -12665, -11585, -10394, -9102, -7723, -6270, -4756, -3196, -1606};

int World::logCosQ14(int i) { return kLogCos[i & 63]; }
int World::logSinQ14(int i) { return kLogSin[i & 63]; }

// =============================================================================================
// CRC32
// =============================================================================================
uint32_t crc32(const uint8_t* d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

namespace {

// =============================================================================================
// Ruído de valor em PONTO FIXO (Q16) — só inteiros: idêntico em qualquer plataforma
// =============================================================================================
inline uint32_t hash32(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
inline int64_t lattice(uint32_t seed, int32_t ix, int32_t iy) { return (int64_t)(hash32(seed, (uint32_t)ix, (uint32_t)iy) >> 16); }  // 0..65535
inline int64_t smooth(int64_t t) { int64_t t2 = (t * t) >> 16; return (t2 * (3 * 65536 - 2 * t)) >> 16; }                          // 3t²-2t³ (Q16)

int64_t vnoise(uint32_t seed, int64_t X, int64_t Y) {  // X,Y em Q16 (unidades de lattice); devolve 0..65535
    int32_t ix = (int32_t)(X >> 16), iy = (int32_t)(Y >> 16);  // >> aritmético = floor (também p/ negativos)
    int64_t fx = X & 0xFFFF, fy = Y & 0xFFFF;
    int64_t sx = smooth(fx), sy = smooth(fy);
    int64_t a = lattice(seed, ix, iy), b = lattice(seed, ix + 1, iy), c = lattice(seed, ix, iy + 1), d = lattice(seed, ix + 1, iy + 1);
    int64_t ab = a + (((b - a) * sx) >> 16), cd = c + (((d - c) * sx) >> 16);
    return ab + (((cd - ab) * sy) >> 16);
}
int64_t fbm(uint32_t seed, int64_t X, int64_t Y, int oct) {  // 0..65535
    int64_t acc = 0, tot = 0, amp = 65536;
    for (int o = 0; o < oct; ++o) {
        acc += amp * vnoise(seed + (uint32_t)o * 101u, X, Y);
        tot += amp; X *= 2; Y *= 2; amp >>= 1;
    }
    return acc / tot;
}
int64_t isqrt64(int64_t n) {  // raiz quadrada inteira (piso)
    if (n <= 0) return 0;
    uint64_t v = (uint64_t)n, r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (int64_t)r;
}
inline int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(hash32(seed, 0xA5A5u, 1u) | 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
    bool chance(int pct) { return (int)(next() % 100u) < pct; }
};

// "Receita" da ilha: sorteada pela semente. Recursos opcionais => nem toda ilha tem tudo.
struct Recipe {
    int R0m = 180;            // raio base da ilha (m)
    int coastAmp = 24000;     // irregularidade da costa (Q16)
    int H = 9;                // altura do planalto (m)
    int hillAmp = 2;          // amplitude dos morros (m)
    bool cliff = false, sand = true, dirt = true, forest = true, rocks = true, buildings = true;
    int px = 0, pz = 0, pr = 6000, peakH = 18;  // montanha de pedra (cm / m)
    int forestPct = 40;       // % aproximada da ilha com floresta
    int nBuild = 3;
    int sandTrees = 0;        // % de palmeiras/coqueiros em areia alta (ilhas pequenas; 0 na ilha inicial)
    bool small = false;       // ilha pequena do oceano aberto: pula o ruído longe da ilha e não precisa de ponto de nascimento
};

Recipe makeRecipe(uint32_t seed) {
    Rng g(seed);
    Recipe R;
    R.R0m = g.range(150, 205);              // diâmetro final (com a costa irregular) fica abaixo de 500 m
    R.coastAmp = g.range(18000, 30000);
    R.H = g.range(7, 11);
    R.hillAmp = g.range(1, 3);
    R.cliff = g.chance(25);
    R.sand = !R.cliff && g.chance(90);
    R.dirt = g.chance(75);
    R.forest = g.chance(80);
    R.rocks = g.chance(75);
    R.buildings = g.chance(55);
    R.forestPct = g.range(30, 60);
    R.nBuild = g.range(1, 4);
    R.pr = g.range(4000, 8000);
    R.peakH = g.range(10, 24);
    int ang = g.range(0, 359);
    int rad = g.range(R.R0m / 10, R.R0m * 45 / 100) * 100;  // cm
    // cos/sin inteiros (tabela por 8 direções, suficiente p/ variar a posição sem float)
    static const int cs[8][2] = {{100, 0}, {71, 71}, {0, 100}, {-71, 71}, {-100, 0}, {-71, -71}, {0, -100}, {71, -71}};
    R.px = (int)((int64_t)cs[(ang / 45) & 7][0] * rad / 100);
    R.pz = (int)((int64_t)cs[(ang / 45) & 7][1] * rad / 100);
    return R;
}

// Receita das ilhas PEQUENAS do oceano aberto: raio de 26 a 70 m, planalto baixo e poucos biomas, cada tipo com seus recursos.
Recipe makeIslandRecipe(uint32_t seed, uint8_t kind) {
    if (kind == IslandBig) return makeRecipe(seed);
    Rng g(seed ^ 0x15EAu);
    Recipe R;
    R.small = true;
    R.coastAmp = g.range(16000, 26000);
    R.hillAmp = 1; R.cliff = false; R.sand = true; R.dirt = false; R.forest = false; R.rocks = false; R.buildings = false;
    R.forestPct = 0; R.nBuild = 0; R.sandTrees = 0;
    switch (kind) {
        case IslandAtoll:   R.R0m = g.range(26, 40); R.H = 3; R.sandTrees = 24; break;                               // só areia e palmeiras
        case IslandPalm:    R.R0m = g.range(35, 55); R.H = g.range(3, 4); R.forest = true; R.forestPct = 22; R.sandTrees = 12; break;
        case IslandForest:  R.R0m = g.range(45, 70); R.H = g.range(4, 6); R.forest = true; R.forestPct = 70; R.hillAmp = 2; R.sand = g.chance(60); break;
        case IslandRock:    R.R0m = g.range(28, 50); R.H = g.range(3, 4); R.rocks = true; R.dirt = true; R.sand = g.chance(50); R.cliff = g.chance(40);
                            R.pr = R.R0m * 55; R.peakH = g.range(7, 12); R.px = 0; R.pz = 0; break;
        case IslandMeadow:  R.R0m = g.range(40, 60); R.H = g.range(3, 5); R.dirt = true; R.sandTrees = 3; R.hillAmp = 2; break;
        default:            R.R0m = g.range(45, 65); R.H = g.range(4, 6); R.forest = true; R.forestPct = 30; R.rocks = g.chance(60); R.dirt = true; R.buildings = true; R.nBuild = g.range(1, 2);
                            R.pr = R.R0m * 45; R.peakH = g.range(6, 9); R.px = R.R0m * 25; R.pz = -R.R0m * 20; break;
    }
    return R;
}

constexpr int kHalfV = World::kCells / 2;  // índice do centro
constexpr int kHalfCm = World::kHalfCm;

int32_t heightCm(const Recipe& R, uint32_t seed, int ix, int iz) {
    int64_t xcm = (int64_t)(ix - kHalfV) * 200, zcm = (int64_t)(iz - kHalfV) * 200;
    // distorção de domínio (costa orgânica)
    if (R.small) {   // fora de 2.5 raios (+ margem da distorção) nada do ruído importa
        int64_t dr = isqrt64(xcm * xcm + zcm * zcm);
        if (dr > (int64_t)R.R0m * 100 * 5 / 2 + 1000) return -4000;
    }
    int64_t n1 = fbm(seed ^ 0x1111u, xcm * 65536 / 20000, zcm * 65536 / 20000, 3);
    int64_t n2 = fbm(seed ^ 0x2222u, (xcm + 91000) * 65536 / 20000, (zcm - 53000) * 65536 / 20000, 3);
    int64_t xw = xcm + (n1 - 32768) * 4000 / 32768, zw = zcm + (n2 - 32768) * 4000 / 32768;
    int64_t d = isqrt64(xw * xw + zw * zw);           // cm
    int64_t R0 = (int64_t)R.R0m * 100;
    if (R.small && d > R0 * 5 / 2) return -4000;      // ilha pequena: longe dela é fundo do mar (mesmo valor que o cálculo completo daria, só mais rápido)
    int64_t m = (R0 - d) * 65536 / R0;                // Q16: 1 no centro, 0 na costa, <0 no mar
    int64_t nc = fbm(seed ^ 0x3333u, xw * 65536 / 14000, zw * 65536 / 14000, 4);
    m += (nc - 32768) * R.coastAmp / 32768;

    int64_t hq;  // Q16 metros
    if (m <= 0) {
        hq = clamp64(m * 70, -40LL * 65536, 0);       // fundo do mar desce até -40 m
    } else {
        int64_t L = m > 65536 ? 65536 : m, prof;
        if (R.cliff) prof = clamp64(L * 33, 0, 65536);                                    // costa de penhasco
        else prof = ((L * 91750) >> 16) - ((((L * L) >> 16) * 26214) >> 16);              // 1.4L - 0.4L²
        hq = ((int64_t)R.H * 65536 * prof) >> 16;
        int64_t hill = fbm(seed ^ 0x4444u, xw * 65536 / 10000, zw * 65536 / 10000, 4) - 32768;   // ±0.5
        hq += (((hill * (int64_t)R.hillAmp * 2 * 65536) >> 16) * clamp64(L * 4, 0, 65536)) >> 16;
        int64_t bump = fbm(seed ^ 0x6666u, xw * 65536 / 3500, zw * 65536 / 3500, 2) - 32768;
        hq += (((bump * 22938) >> 15) * clamp64(L * 6, 0, 65536)) >> 16;                          // ±0.35 m
        if (R.rocks) {
            int64_t dx = xw - R.px, dz = zw - R.pz, pr2 = (int64_t)R.pr * R.pr;
            int64_t t = 65536 - (dx * dx + dz * dz) * 65536 / pr2;
            if (t > 0) {
                int64_t t2 = (t * t) >> 16;
                int64_t rn = fbm(seed ^ 0x5555u, xw * 65536 / 6000, zw * 65536 / 6000, 3);
                int64_t f = 45875 + ((rn * 39322) >> 16);                                         // 0.7..1.3
                int64_t mt = (((int64_t)R.peakH * 65536 * t2) >> 16) * f >> 16;
                hq += (mt * clamp64(L * 8, 0, 65536)) >> 16;
            }
        }
    }
    if (d > kHalfCm - 4500) {  // borda do mundo: sempre mar fundo
        int64_t f = clamp64((d - (kHalfCm - 4500)) * 65536 / 3000, 0, 65536);
        hq = ((hq * (65536 - f)) >> 16) + (((-30LL * 65536) * f) >> 16);
    }
    return (int32_t)clamp64((hq * 100 + 32768) >> 16, -4000, 6000);
}

// altura inteira (cm) numa posição (cm) — mesma triangulação de World::heightAt (usada p/ ycm dos objetos)
int32_t heightCmAt(const std::vector<int16_t>& h, int32_t xcm, int32_t zcm) {
    int32_t gx = xcm + kHalfCm, gz = zcm + kHalfCm;
    gx = gx < 0 ? 0 : (gx > 2 * kHalfCm - 1 ? 2 * kHalfCm - 1 : gx); gz = gz < 0 ? 0 : (gz > 2 * kHalfCm - 1 ? 2 * kHalfCm - 1 : gz);
    int ix = gx / 200, iz = gz / 200, fx = gx % 200, fz = gz % 200;
    const int16_t* p = &h[(size_t)iz * World::kVerts + ix];
    int32_t h00 = p[0], h10 = p[1], h01 = p[World::kVerts], h11 = p[World::kVerts + 1];
    return fx >= fz ? h00 + ((h10 - h00) * fx + (h11 - h10) * fz) / 200 : h00 + ((h11 - h01) * fx + (h01 - h00) * fz) / 200;
}

struct BSite { int32_t x, z; uint8_t type, rot; int32_t hx, hz; };  // centro (cm), meia-largura (cm, já rotacionada)

void footprint(uint8_t type, uint8_t rot, int32_t& hx, int32_t& hz) {
    int a = 230, b = 180;                       // cabana
    if (type == ObjShed) { a = 340; b = 240; }
    if (type == ObjTower) { a = 150; b = 150; }
    if (rot & 64) { hx = b; hz = a; } else { hx = a; hz = b; }  // 90°: troca
}

}  // namespace

void World::logDir(const WorldObject& o, float& dx, float& dz) {
    int i = (o.rot >> 2) & 63;
    dx = kLogCos[i] * (1.0f / 16384.0f); dz = kLogSin[i] * (1.0f / 16384.0f);
}

World::Collider World::colliderOf(const WorldObject& o) {
    float s = o.scale100 * 0.01f;
    Collider c;
    switch (o.type) {
        case ObjPalm: case ObjCoconut: c.kind = 0; c.a = 0.30f * s; break;
        case ObjRock: c.kind = 0; c.a = 0.85f * s; break;
        case ObjStick: case ObjPebble: case ObjCloth: c.kind = 3; break;   // coletáveis: o jogador passa por cima
        case ObjLog: c.kind = 2; c.a = 0.38f * s; c.b = 2.3f * s; logDir(o, c.dx, c.dz); break;
        // construções = RUÍNAS (kind 4): colidem só as partes em pé (trechos de parede > 45 cm, pilares, chaminé, postes da torre); brechas e entulho baixo se atravessam
        case ObjCabin: case ObjShed: case ObjTower: {
            c.kind = 4;
            int32_t hx, hz; footprint(o.type, o.rot, hx, hz);
            c.a = hx * 0.01f * s; c.b = hz * 0.01f * s;   // meia-largura da ÁREA (p/ a grade espacial)
            break;
        }
        default: {
            c.kind = 1;
            int32_t hx, hz; footprint(o.type, o.rot, hx, hz);
            c.a = hx * 0.01f * s; c.b = hz * 0.01f * s;
        }
    }
    return c;
}

// =============================================================================================
// Coletáveis (gravetos e pedrinhas): só GERADOS (sem sistema de coleta), sem colisão. Pós-passe determinístico (só inteiros):
// também é aplicado a mundos salvos de versão antiga (v2) ao carregar, sem mexer em nada do que já existia.
// =============================================================================================
namespace {

// folga (cm) entre o ponto e o colisor do objeto (negativo = dentro). Tudo inteiro => mesmo resultado em qualquer CPU.
int64_t gapCm(const WorldObject& o, int64_t x, int64_t z) {
    int64_t dx = x - o.xcm, dz = z - o.zcm, sc = o.scale100;
    switch (o.type) {
        case ObjPalm: case ObjCoconut: return isqrt64(dx * dx + dz * dz) - 30 * sc / 100;
        case ObjRock: return isqrt64(dx * dx + dz * dz) - 85 * sc / 100;
        case ObjLog: {
            int i = (o.rot >> 2) & 63;
            int64_t c = (int64_t)World::logCosQ14(i), sn = (int64_t)World::logSinQ14(i), half = 230 * sc / 100;
            int64_t t = (dx * c + dz * sn) >> 14;
            t = t < -half ? -half : (t > half ? half : t);
            int64_t ex = dx - ((t * c) >> 14), ez = dz - ((t * sn) >> 14);
            return isqrt64(ex * ex + ez * ez) - 38 * sc / 100;
        }
        case ObjStick: case ObjPebble: case ObjCloth: return isqrt64(dx * dx + dz * dz) - 12;    // só p/ não empilhar coletáveis
        default: {  // construções: caixa
            int32_t hx, hz; footprint(o.type, o.rot, hx, hz);
            int64_t ax = std::llabs(dx) - hx * sc / 100, az = std::llabs(dz) - hz * sc / 100;
            if (ax <= 0 && az <= 0) return -1;
            ax = ax < 0 ? 0 : ax; az = az < 0 ? 0 : az;
            return isqrt64(ax * ax + az * az);
        }
    }
}

}  // namespace

static void addCollectibles(World& w) {
    const uint32_t seed = w.seed;
    auto biomeAtCm = [&](int32_t x, int32_t z) {
        int ix = std::max(0, std::min(World::kCells, (x + kHalfCm + 100) / 200)), iz = std::max(0, std::min(World::kCells, (z + kHalfCm + 100) / 200));
        return w.biome[(size_t)iz * World::kVerts + ix];
    };
    auto free = [&](int32_t x, int32_t z, int64_t margin) {
        for (const WorldObject& o : w.objects) {
            int64_t dx = o.xcm - x, dz = o.zcm - z;
            if (dx > 1200 || dx < -1200 || dz > 1200 || dz < -1200) continue;        // longe demais p/ importar
            if (gapCm(o, x, z) < margin) return false;
        }
        return true;
    };
    auto flat = [&](int32_t x, int32_t z) {
        return std::abs(heightCmAt(w.hcm, x + 120, z) - heightCmAt(w.hcm, x - 120, z)) + std::abs(heightCmAt(w.hcm, x, z + 120) - heightCmAt(w.hcm, x, z - 120)) < 110;
    };
    auto put = [&](uint8_t type, uint32_t hv, int32_t x, int32_t z, uint8_t b) {
        WorldObject o; o.type = type;
        bool pale = type == ObjStick && b == BiomeSand;                               // graveto claro (madeira de praia)
        o.variant = (uint8_t)(((hv >> 4) & 3) | (pale ? 4u : 0u));
        o.rot = (uint8_t)(hv >> 24);
        o.scale100 = (uint16_t)(type == ObjStick ? 70 + ((hv >> 8) & 0x3F) * 70 / 63 : 60 + ((hv >> 8) & 0x3F) * 70 / 63);
        o.xcm = x; o.zcm = z; o.ycm = (int16_t)heightCmAt(w.hcm, x, z);
        w.objects.push_back(o);
    };
    const size_t before = w.objects.size();   // só obstáculos "de verdade" contam p/ evitar sobreposição (os coletáveis novos entram no fim)

    // ---- gravetos: floresta (mais), grama, terra e praia; e em volta dos troncos caídos ----
    {
        const int cell = 550, n = 2 * kHalfCm / cell + 1;
        for (int cz = 0; cz < n; ++cz)
            for (int cx = 0; cx < n; ++cx) {
                uint32_t hv = hash32(seed ^ 0x57C1u, (uint32_t)cx, (uint32_t)cz);
                int32_t x = cx * cell - kHalfCm + (int32_t)(hv & 0x3FF) * cell / 1024, z = cz * cell - kHalfCm + (int32_t)((hv >> 10) & 0x3FF) * cell / 1024;
                if (x < -(kHalfCm - 2000) || x > kHalfCm - 2000 || z < -(kHalfCm - 2000) || z > kHalfCm - 2000) continue;
                uint8_t b = biomeAtCm(x, z);
                int pct = b == BiomeForest ? 34 : (b == BiomeGrass ? 7 : (b == BiomeDirt ? 9 : (b == BiomeSand ? 4 : 0)));
                if ((int)((hv >> 20) % 100u) >= pct) continue;
                if (heightCmAt(w.hcm, x, z) < 25 || !flat(x, z) || !free(x, z, 20)) continue;
                put(ObjStick, hv, x, z, b);
            }
    }
    for (size_t i = 0; i < before; ++i) {   // 2 a 5 gravetos perto de cada tronco caído
        if (w.objects[i].type != ObjLog) continue;
        const WorldObject lg = w.objects[i];
        int cnt = 2 + (int)(hash32(seed ^ 0x57C2u, (uint32_t)lg.xcm, (uint32_t)lg.zcm) % 4u);
        for (int k = 0; k < cnt; ++k) {
            uint32_t hv = hash32(seed ^ 0x57C3u, (uint32_t)(lg.xcm * 31 + k), (uint32_t)lg.zcm);
            int32_t r = 180 + (int32_t)(hv & 0xFF) * 220 / 256, a = (int32_t)((hv >> 8) & 63);   // 1.8..4 m, em 64 direções
            int32_t x = lg.xcm + (int32_t)((int64_t)World::logCosQ14(a) * r >> 14), z = lg.zcm + (int32_t)((int64_t)World::logSinQ14(a) * r >> 14);
            if (heightCmAt(w.hcm, x, z) < 25 || biomeAtCm(x, z) == BiomePad || !free(x, z, 20)) continue;
            put(ObjStick, hv, x, z, biomeAtCm(x, z));
        }
    }

    // ---- pedrinhas: áreas de pedra, praia, terra; e 0 a 3 em volta de cada pedra grande ----
    {
        const int cell = 650, n = 2 * kHalfCm / cell + 1;
        for (int cz = 0; cz < n; ++cz)
            for (int cx = 0; cx < n; ++cx) {
                uint32_t hv = hash32(seed ^ 0x9EB1u, (uint32_t)cx, (uint32_t)cz);
                int32_t x = cx * cell - kHalfCm + (int32_t)(hv & 0x3FF) * cell / 1024, z = cz * cell - kHalfCm + (int32_t)((hv >> 10) & 0x3FF) * cell / 1024;
                if (x < -(kHalfCm - 2000) || x > kHalfCm - 2000 || z < -(kHalfCm - 2000) || z > kHalfCm - 2000) continue;
                uint8_t b = biomeAtCm(x, z);
                int pct = b == BiomeRock ? 30 : (b == BiomeSand ? 10 : (b == BiomeDirt ? 12 : (b == BiomeGrass ? 3 : (b == BiomeForest ? 2 : 0))));
                if ((int)((hv >> 20) % 100u) >= pct) continue;
                if (heightCmAt(w.hcm, x, z) < 25 || !free(x, z, 25)) continue;
                put(ObjPebble, hv, x, z, b);
            }
    }
    for (size_t i = 0; i < before; ++i) {
        if (w.objects[i].type != ObjRock) continue;
        const WorldObject rk = w.objects[i];
        int cnt = (int)(hash32(seed ^ 0x9EB2u, (uint32_t)rk.xcm, (uint32_t)rk.zcm) % 4u);
        for (int k = 0; k < cnt; ++k) {
            uint32_t hv = hash32(seed ^ 0x9EB3u, (uint32_t)(rk.xcm * 17 + k), (uint32_t)rk.zcm);
            int32_t r = (85 * rk.scale100 / 100) + 40 + (int32_t)(hv & 0xFF) * 120 / 256, a = (int32_t)((hv >> 8) & 63);
            int32_t x = rk.xcm + (int32_t)((int64_t)World::logCosQ14(a) * r >> 14), z = rk.zcm + (int32_t)((int64_t)World::logSinQ14(a) * r >> 14);
            if (heightCmAt(w.hcm, x, z) < 25 || biomeAtCm(x, z) == BiomePad || !free(x, z, 25)) continue;
            put(ObjPebble, hv, x, z, biomeAtCm(x, z));
        }
    }
}

// TECIDOS: lixo trazido pelo mar. SÓ na areia (praia) e perto da água (altura 25..140 cm), em pequenos grupos (linha de maré). Pós-passe só com inteiros,
// roda DEPOIS de addCollectibles (os índices dos objetos já existentes nunca mudam: mundos v3 são atualizados só acrescentando estes no fim).
static void addCloth(World& w) {
    const uint32_t seed = w.seed;
    auto biomeAtCm = [&](int32_t x, int32_t z) {
        int ix = std::max(0, std::min(World::kCells, (x + kHalfCm + 100) / 200)), iz = std::max(0, std::min(World::kCells, (z + kHalfCm + 100) / 200));
        return w.biome[(size_t)iz * World::kVerts + ix];
    };
    auto free = [&](int32_t x, int32_t z, int64_t margin) {
        for (const WorldObject& o : w.objects) {
            int64_t dx = o.xcm - x, dz = o.zcm - z;
            if (dx > 1200 || dx < -1200 || dz > 1200 || dz < -1200) continue;
            if (gapCm(o, x, z) < margin) return false;
        }
        return true;
    };
    auto beach = [&](int32_t x, int32_t z) {
        if (biomeAtCm(x, z) != BiomeSand) return false;
        int h = heightCmAt(w.hcm, x, z);
        return h >= 25 && h <= 140;
    };
    auto put = [&](uint32_t hv, int32_t x, int32_t z) {
        WorldObject o; o.type = ObjCloth;
        o.variant = (uint8_t)((hv >> 4) & 3);
        o.rot = (uint8_t)(hv >> 24);
        o.scale100 = (uint16_t)(70 + ((hv >> 8) & 0x3F) * 60 / 63);
        o.xcm = x; o.zcm = z; o.ycm = (int16_t)heightCmAt(w.hcm, x, z);
        w.objects.push_back(o);
    };
    const int cell = 600, n = 2 * kHalfCm / cell + 1;
    for (int cz = 0; cz < n; ++cz)
        for (int cx = 0; cx < n; ++cx) {
            uint32_t hv = hash32(seed ^ 0xC10Fu, (uint32_t)cx, (uint32_t)cz);
            int32_t x = cx * cell - kHalfCm + (int32_t)(hv & 0x3FF) * cell / 1024, z = cz * cell - kHalfCm + (int32_t)((hv >> 10) & 0x3FF) * cell / 1024;
            if (x < -(kHalfCm - 2000) || x > kHalfCm - 2000 || z < -(kHalfCm - 2000) || z > kHalfCm - 2000) continue;
            if ((int)((hv >> 20) % 100u) >= 9) continue;
            if (!beach(x, z) || !free(x, z, 25)) continue;
            put(hv, x, z);
            for (int k = 0; k < 2; ++k) {   // 0 a 2 pedaços a mais por perto (arrastados juntos pela maré)
                uint32_t h2 = hash32(seed ^ 0xC110u, (uint32_t)(x * 31 + k), (uint32_t)z);
                if ((int)(h2 % 100u) >= 45) continue;
                int32_t r = 90 + (int32_t)(h2 & 0xFF) * 170 / 256, a = (int32_t)((h2 >> 8) & 63);
                int32_t x2 = x + (int32_t)((int64_t)World::logCosQ14(a) * r >> 14), z2 = z + (int32_t)((int64_t)World::logSinQ14(a) * r >> 14);
                if (!beach(x2, z2) || !free(x2, z2, 25)) continue;
                put(h2, x2, z2);
            }
        }
}

// =============================================================================================
// Geração
// =============================================================================================
static constexpr int kVerts = World::kVerts, kCells = World::kCells;
static bool genImpl(const Recipe& R, uint32_t seed, World& w, bool wantSpawn);
bool World::generate(uint32_t seed, World& w) { return genImpl(makeRecipe(seed), seed, w, true); }
bool World::generateIsland(uint32_t seed, uint8_t kind, int32_t oxcm, int32_t ozcm, World& w) {
    if (kind >= IslandKindCount) return false;
    if (!genImpl(makeIslandRecipe(seed, kind), seed, w, false)) return false;
    w.kind = kind;
    w.shiftTo(oxcm, ozcm);
    return true;
}
static bool genImpl(const Recipe& R, uint32_t seed, World& w, bool wantSpawn) {
    w = World();
    w.seed = seed;
    w.hcm.assign((size_t)kVerts * kVerts, 0);
    w.biome.assign((size_t)kVerts * kVerts, BiomeGrass);
    auto H = [&](int ix, int iz) -> int16_t& { return w.hcm[(size_t)iz * kVerts + ix]; };
    for (int iz = 0; iz < kVerts; ++iz)
        for (int ix = 0; ix < kVerts; ++ix) H(ix, iz) = (int16_t)heightCm(R, seed, ix, iz);

    Rng g(seed ^ 0xB11D1u);

    // ---- construções: acha terreno plano, aplaina e marca o piso ----
    std::vector<BSite> sites;
    if (R.buildings) {
        for (int attempt = 0; attempt < 900 && (int)sites.size() < R.nBuild; ++attempt) {
            int32_t cx = g.range(-R.R0m * 70 / 100, R.R0m * 70 / 100) * 100, cz = g.range(-R.R0m * 70 / 100, R.R0m * 70 / 100) * 100;
            uint8_t type = (uint8_t)(g.chance(55) ? ObjCabin : (g.chance(60) ? ObjShed : ObjTower));
            uint8_t rot = (uint8_t)(g.range(0, 3) * 64);
            int32_t hx, hz; footprint(type, rot, hx, hz);
            int32_t mg = 400;  // margem p/ checagem e aplainamento
            int x0 = (cx - hx - mg + kHalfCm) / 200, x1 = (cx + hx + mg + kHalfCm) / 200 + 1;
            int z0 = (cz - hz - mg + kHalfCm) / 200, z1 = (cz + hz + mg + kHalfCm) / 200 + 1;
            if (x0 < 2 || z0 < 2 || x1 > kCells - 2 || z1 > kCells - 2) continue;
            int hmin = 99999, hmax = -99999; int64_t sum = 0; int cnt = 0;
            for (int iz = z0; iz <= z1; ++iz) for (int ix = x0; ix <= x1; ++ix) { int hh = H(ix, iz); hmin = std::min(hmin, hh); hmax = std::max(hmax, hh); sum += hh; ++cnt; }
            if (hmin < 180 || hmax > 1500 || hmax - hmin > 130) continue;
            bool near = false;
            for (const BSite& s : sites) { int64_t dx = s.x - cx, dz = s.z - cz; if (dx * dx + dz * dz < 3000LL * 3000LL) near = true; }
            if (near) continue;
            int32_t target = (int32_t)(sum / cnt);
            for (int iz = z0; iz <= z1; ++iz)
                for (int ix = x0; ix <= x1; ++ix) {  // aplainamento com borda suave
                    int32_t vx = ix * 200 - kHalfCm, vz = iz * 200 - kHalfCm;
                    int32_t ex = std::max(0, std::abs(vx - cx) - hx), ez = std::max(0, std::abs(vz - cz) - hz);   // distância ao retângulo (cm)
                    int32_t dd = std::max(ex, ez);
                    int32_t wgt = dd <= 100 ? 256 : std::max(0, (int32_t)(256 * (mg + 100 - dd) / mg));
                    H(ix, iz) = (int16_t)(H(ix, iz) + ((target - H(ix, iz)) * wgt) / 256);
                }
            sites.push_back({cx, cz, type, rot, hx, hz});
        }
    }

    // ---- biomas (por vértice) ----
    int rockLine = (R.H + R.hillAmp + R.peakH * 22 / 100) * 100;  // pedra só acima do planalto (parte alta da montanha)
    int forestThr = 65536 - (R.forestPct * 65536 / 100) * 9 / 10 - 9000;
    for (int iz = 0; iz < kVerts; ++iz)
        for (int ix = 0; ix < kVerts; ++ix) {
            int hh = H(ix, iz);
            int ixa = std::max(ix - 1, 0), ixb = std::min(ix + 1, kCells), iza = std::max(iz - 1, 0), iza2 = std::min(iz + 1, kCells);
            int dx = H(ixb, iz) - H(ixa, iz), dz = H(ix, iza2) - H(ix, iza);
            int slope = (int)isqrt64((int64_t)dx * dx + (int64_t)dz * dz) * 1000 / ((ixb - ixa + iza2 - iza) * 100);  // m/m * 1000 (espaço 2 m ou 4 m)
            int64_t xcm = (int64_t)(ix - kHalfV) * 200, zcm = (int64_t)(iz - kHalfV) * 200;
            int64_t nSand = fbm(seed ^ 0x7777u, xcm * 65536 / 6000, zcm * 65536 / 6000, 2);
            int64_t nDirt = fbm(seed ^ 0x8888u, xcm * 65536 / 5500, zcm * 65536 / 5500, 3);
            int64_t nFor = fbm(seed ^ 0x9999u, xcm * 65536 / 8000, zcm * 65536 / 8000, 3);
            uint8_t b = BiomeGrass;
            if (hh < -30) b = BiomeSeaFloor;
            else if ((R.rocks && (slope >= 800 || hh > rockLine)) || slope >= 1100) b = BiomeRock;
            else if (R.sand && hh < 150 + (int)((nSand - 32768) / 400)) b = BiomeSand;
            else if (R.dirt && nDirt > 41500) b = BiomeDirt;
            else if (R.forest && nFor > forestThr && slope < 650 && hh > 120) b = BiomeForest;
            w.biome[(size_t)iz * kVerts + ix] = b;
        }
    for (const BSite& s : sites)  // piso das construções
        for (int iz = 0; iz < kVerts; ++iz)
            for (int ix = 0; ix < kVerts; ++ix) {
                int32_t vx = ix * 200 - kHalfCm, vz = iz * 200 - kHalfCm;
                if (std::abs(vx - s.x) <= s.hx + 150 && std::abs(vz - s.z) <= s.hz + 150) w.biome[(size_t)iz * kVerts + ix] = BiomePad;
            }

    // ---- objetos ----
    auto biomeNear = [&](int32_t xcm, int32_t zcm) {
        int ix = std::max(0, std::min(kCells, (xcm + kHalfCm + 100) / 200)), iz = std::max(0, std::min(kCells, (zcm + kHalfCm + 100) / 200));
        return w.biome[(size_t)iz * kVerts + ix];
    };
    auto nearBuilding = [&](int32_t x, int32_t z, int32_t extra) {
        for (const BSite& s : sites) if (std::abs(x - s.x) < s.hx + extra && std::abs(z - s.z) < s.hz + extra) return true;
        return false;
    };
    for (const BSite& s : sites) {
        WorldObject o; o.type = s.type; o.rot = s.rot; o.variant = (uint8_t)(hash32(seed, (uint32_t)s.x, (uint32_t)s.z) & 3);
        o.scale100 = 100; o.xcm = s.x; o.zcm = s.z; o.ycm = (int16_t)heightCmAt(w.hcm, s.x, s.z);
        w.objects.push_back(o);
    }
    if (R.forest || R.sandTrees > 0) {  // árvores: grade com jitter (7 m)
        for (int cz = 0; cz < 2 * kHalfCm / 700 + 1; ++cz)
            for (int cx = 0; cx < 2 * kHalfCm / 700 + 1; ++cx) {
                uint32_t hv = hash32(seed ^ 0x7EE5u, (uint32_t)cx, (uint32_t)cz);
                int32_t x = cx * 700 - kHalfCm + (int32_t)(hv & 0x3FF) * 700 / 1024, z = cz * 700 - kHalfCm + (int32_t)((hv >> 10) & 0x3FF) * 700 / 1024;
                if (x < -(kHalfCm - 2000) || x > kHalfCm - 2000 || z < -(kHalfCm - 2000) || z > kHalfCm - 2000) continue;
                uint8_t b = biomeNear(x, z);
                int pct = b == BiomeForest ? (R.forest ? 68 : 0) : (b == BiomeGrass ? (R.forest || R.sandTrees ? 5 : 0) : (b == BiomeSand ? R.sandTrees : 0));
                if ((int)((hv >> 20) % 100u) >= pct) continue;
                int32_t hh = heightCmAt(w.hcm, x, z);
                if (hh < 100 || nearBuilding(x, z, 450)) continue;
                WorldObject o; o.type = (uint8_t)((hh > 600 || ((hv >> 12) & 1)) ? ObjPalm : ObjCoconut);
                o.variant = (uint8_t)((hv >> 5) & 3); o.rot = (uint8_t)(hv >> 24);
                o.scale100 = (uint16_t)(85 + ((hv >> 8) & 0x3F) * 45 / 63);
                o.xcm = x; o.zcm = z; o.ycm = (int16_t)hh;
                w.objects.push_back(o);
            }
    }
    if (R.rocks) {  // pedras: mais nas áreas de pedra
        for (int cz = 0; cz < 2 * kHalfCm / 900 + 1; ++cz)
            for (int cx = 0; cx < 2 * kHalfCm / 900 + 1; ++cx) {
                uint32_t hv = hash32(seed ^ 0x40C4u, (uint32_t)cx, (uint32_t)cz);
                int32_t x = cx * 900 - kHalfCm + (int32_t)(hv & 0x3FF) * 900 / 1024, z = cz * 900 - kHalfCm + (int32_t)((hv >> 10) & 0x3FF) * 900 / 1024;
                if (x < -(kHalfCm - 2000) || x > kHalfCm - 2000 || z < -(kHalfCm - 2000) || z > kHalfCm - 2000) continue;
                uint8_t b = biomeNear(x, z);
                int pct = b == BiomeRock ? 38 : ((b == BiomeGrass || b == BiomeDirt || b == BiomeForest) ? 5 : (b == BiomeSand ? 3 : 0));
                if ((int)((hv >> 20) % 100u) >= pct) continue;
                int32_t hh = heightCmAt(w.hcm, x, z);
                if (hh < 80 || nearBuilding(x, z, 350)) continue;
                WorldObject o; o.type = ObjRock; o.variant = (uint8_t)((hv >> 5) & 3); o.rot = (uint8_t)(hv >> 24);
                o.scale100 = (uint16_t)(60 + ((hv >> 8) & 0x3F) * 170 / 63);
                o.xcm = x; o.zcm = z; o.ycm = (int16_t)hh;
                w.objects.push_back(o);
            }
    }

    if (R.forest) {  // árvores caídas: troncos deitados em terreno quase plano, mais na floresta
        const int cellCm = 1400;
        for (int cz = 0; cz < 2 * kHalfCm / cellCm + 1; ++cz)
            for (int cx = 0; cx < 2 * kHalfCm / cellCm + 1; ++cx) {
                uint32_t hv = hash32(seed ^ 0x106Au, (uint32_t)cx, (uint32_t)cz);
                int32_t x = cx * cellCm - kHalfCm + (int32_t)(hv & 0x3FF) * cellCm / 1024, z = cz * cellCm - kHalfCm + (int32_t)((hv >> 10) & 0x3FF) * cellCm / 1024;
                if (x < -(kHalfCm - 2000) || x > kHalfCm - 2000 || z < -(kHalfCm - 2000) || z > kHalfCm - 2000) continue;
                uint8_t b = biomeNear(x, z);
                int pct = b == BiomeForest ? 22 : (b == BiomeGrass ? 4 : 0);
                if ((int)((hv >> 20) % 100u) >= pct) continue;
                int32_t hh = heightCmAt(w.hcm, x, z);
                if (hh < 100 || nearBuilding(x, z, 600)) continue;
                int32_t sl = std::abs(heightCmAt(w.hcm, x + 250, z) - heightCmAt(w.hcm, x - 250, z)) + std::abs(heightCmAt(w.hcm, x, z + 250) - heightCmAt(w.hcm, x, z - 250));
                if (sl > 110) continue;                                      // só em terreno plano
                bool clash = false;
                for (const WorldObject& o : w.objects) {                      // longe de árvores, pedras e outros troncos
                    int64_t dx = o.xcm - x, dz = o.zcm - z, lim = World::isBuilding(o.type) ? 900 : 380;
                    if (o.type == ObjLog) lim = 600;
                    if (dx * dx + dz * dz < lim * lim) { clash = true; break; }
                }
                if (clash) continue;
                WorldObject o; o.type = ObjLog; o.variant = (uint8_t)((hv >> 5) & 3); o.rot = (uint8_t)(hv >> 24);
                o.scale100 = (uint16_t)(70 + ((hv >> 8) & 0x3F) * 60 / 63);
                o.xcm = x; o.zcm = z; o.ycm = (int16_t)hh;
                w.objects.push_back(o);
            }
    }

    // ---- ponto de nascimento: terreno plano e seco perto do centro; limpa objetos ao redor ----
    bool found = !wantSpawn;   // ilhas do oceano aberto não têm nascimento (ninguém nasce nelas): o centro serve de referência
    if (!wantSpawn) { w.spawnXcm = 0; w.spawnZcm = 0; }
    for (int pass = 0; pass < 2 && !found; ++pass) {  // 1ª: terra baixa e plana; 2ª: aceita planaltos (ilhas de penhasco)
        const int32_t maxH = pass == 0 ? 900 : 2800, maxStep = pass == 0 ? 90 : 120;
        for (int ring = 0; ring < 150 && !found; ring += 2) {  // até 300 m do centro
            for (int k = 0; k < 16 && !found; ++k) {
                static const int cs[16][2] = {{100, 0}, {92, 38}, {71, 71}, {38, 92}, {0, 100}, {-38, 92}, {-71, 71}, {-92, 38},
                                              {-100, 0}, {-92, -38}, {-71, -71}, {-38, -92}, {0, -100}, {38, -92}, {71, -71}, {92, -38}};
                int32_t x = (int32_t)((int64_t)cs[k][0] * ring * 200 / 100), z = (int32_t)((int64_t)cs[k][1] * ring * 200 / 100);
                bool ok = !nearBuilding(x, z, 2200);  // longe de construções (22 m)
                for (int dz = -800; dz <= 800 && ok; dz += 400)
                    for (int dx = -800; dx <= 800 && ok; dx += 400) {
                        int32_t hh = heightCmAt(w.hcm, x + dx, z + dz), hn = heightCmAt(w.hcm, x + dx + 200, z + dz + 200);
                        uint8_t b = biomeNear(x + dx, z + dz);
                        if (hh < 150 || hh > maxH || std::abs(hn - hh) > maxStep || b == BiomeRock || b == BiomeSeaFloor || b == BiomePad) ok = false;
                    }
                if (ok) { w.spawnXcm = x; w.spawnZcm = z; found = true; }
            }
        }
    }
    if (!found) {  // último recurso: o vértice seco mais próximo do centro (fora de rocha, piso e construções)
        int64_t best = -1;
        for (int iz = 4; iz < kCells - 4; ++iz)
            for (int ix = 4; ix < kCells - 4; ++ix) {
                int32_t hh = H(ix, iz); uint8_t b = w.biome[(size_t)iz * kVerts + ix];
                int32_t x = ix * 200 - kHalfCm, z = iz * 200 - kHalfCm;
                if (hh < 150 || hh > 3500 || b == BiomeRock || b == BiomeSeaFloor || b == BiomePad || nearBuilding(x, z, 1500)) continue;
                int64_t d2 = (int64_t)x * x + (int64_t)z * z;
                if (best < 0 || d2 < best) { best = d2; w.spawnXcm = x; w.spawnZcm = z; found = true; }
            }
    }
    if (!found) { if (std::getenv("E0_WORLD_DEBUG")) std::fprintf(stderr, "[mundo %u] sem ponto de nascimento valido\n", seed); return false; }
    std::vector<WorldObject> kept;
    if (wantSpawn) for (const WorldObject& o : w.objects) {
        int64_t dx = o.xcm - w.spawnXcm, dz = o.zcm - w.spawnZcm;
        bool building = World::isBuilding(o.type);
        if (!building && dx * dx + dz * dz < 900LL * 900LL) continue;   // limpa 9 m em volta do spawn
        if (building && std::abs(dx) < 1500 && std::abs(dz) < 1500) { if (std::getenv("E0_WORLD_DEBUG")) std::fprintf(stderr, "[mundo %u] construcao no spawn\n", seed); return false; }  // (não deve ocorrer)
        kept.push_back(o);
    }
    if (wantSpawn) w.objects.swap(kept);

    w.features = (R.sand ? (uint32_t)FeatSand : 0u) | (R.dirt ? (uint32_t)FeatDirt : 0u) | (R.forest ? (uint32_t)FeatForest : 0u) | (R.rocks ? (uint32_t)FeatRocks : 0u) |
                 (R.buildings && !sites.empty() ? (uint32_t)FeatBuildings : 0u) | (R.cliff ? (uint32_t)FeatCliffCoast : 0u);
    addCollectibles(w);   // gravetos e pedrinhas coletáveis (só gerados)
    addCloth(w);          // tecidos trazidos pelo mar (só na praia)
    w.buildIndex();
    std::vector<uint8_t> bytes = w.toBytes();
    std::memcpy(&w.hash, bytes.data() + bytes.size() - 4, 4);
    return true;
}

// =============================================================================================
// Arquivo .e0w
// =============================================================================================
std::vector<uint8_t> World::toBytes() const {
    std::vector<uint8_t> b;
    b.reserve(64 + hcm.size() * 3 + objects.size() * 16);
    auto put = [&](const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; b.insert(b.end(), q, q + n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    put("E0W1", 4);
    u32(kGenVersion); u32(seed); u32((uint32_t)kCells); u32(features);
    u32((uint32_t)spawnXcm); u32((uint32_t)spawnZcm); u32((uint32_t)objects.size());
    put(hcm.data(), hcm.size() * 2);
    put(biome.data(), biome.size());
    for (const WorldObject& o : objects) {
        put(&o.type, 1); put(&o.variant, 1); put(&o.rot, 1); put(&o.scale100, 2);
        put(&o.xcm, 4); put(&o.zcm, 4); put(&o.ycm, 2);
    }
    uint32_t c = crc32(b.data(), b.size());
    u32(c);
    return b;
}

bool World::fromBytes(const uint8_t* d, size_t n, World& w) {
    if (n < 40 || std::memcmp(d, "E0W1", 4) != 0) return false;
    uint32_t stored; std::memcpy(&stored, d + n - 4, 4);
    if (crc32(d, n - 4) != stored) return false;  // arquivo corrompido / adulterado
    size_t o = 4;
    auto u32 = [&](uint32_t& v) { std::memcpy(&v, d + o, 4); o += 4; };
    uint32_t gen, cells, nObj, sx, sz;
    u32(gen); w = World(); u32(w.seed); u32(cells); u32(w.features); u32(sx); u32(sz); u32(nObj);
    if ((gen != kGenVersion && gen != 3 && gen != 2) || cells != (uint32_t)kCells) return false;   // v2 = sem coletáveis, v3 = sem tecidos: atualiza abaixo
    w.spawnXcm = (int32_t)sx; w.spawnZcm = (int32_t)sz;
    size_t nv = (size_t)kVerts * kVerts;
    if (o + nv * 3 + (size_t)nObj * 15 + 4 > n || nObj > 200000) return false;
    w.hcm.resize(nv); std::memcpy(w.hcm.data(), d + o, nv * 2); o += nv * 2;
    w.biome.resize(nv); std::memcpy(w.biome.data(), d + o, nv); o += nv;
    w.objects.resize(nObj);
    for (WorldObject& ob : w.objects) {
        std::memcpy(&ob.type, d + o, 1); std::memcpy(&ob.variant, d + o + 1, 1); std::memcpy(&ob.rot, d + o + 2, 1);
        std::memcpy(&ob.scale100, d + o + 3, 2); std::memcpy(&ob.xcm, d + o + 5, 4); std::memcpy(&ob.zcm, d + o + 9, 4); std::memcpy(&ob.ycm, d + o + 13, 2);
        o += 15;
    }
    w.hash = stored;
    if (gen == 2 || gen == 3) {   // mundo salvo antigo: acrescenta o que faltava (v2: coletáveis; v2/v3: tecidos) sem tocar em mais nada e recalcula o hash
        if (gen == 2) addCollectibles(w);
        addCloth(w);
        w.upgraded = true;
        std::vector<uint8_t> nb = w.toBytes();
        std::memcpy(&w.hash, nb.data() + nb.size() - 4, 4);
    }
    w.buildIndex();
    return true;
}

bool World::save(const std::string& path) const {
    std::vector<uint8_t> b = toBytes();
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;  // grava e troca: nunca deixa um arquivo pela metade
}

bool World::load(const std::string& path, World& w) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return fromBytes(b.data(), b.size(), w);
}

// =============================================================================================
// Colisão com objetos (grade de 16 m)
// =============================================================================================
static constexpr int kBucketCm = 1600, kBuckets = World::kCells / 8;  // baldes de 16 m

void World::buildIndex() {
    {   // minérios: ordena as pedras grandes por hash da posição (desempate pelo índice) e marca as primeiras
        ore_.assign(objects.size(), 0);
        std::vector<std::pair<uint32_t, uint32_t>> rocks;
        for (uint32_t i = 0; i < objects.size(); ++i)
            if (objects[i].type == ObjRock) rocks.push_back({ruinHash(ruinHash((uint32_t)objects[i].xcm, (uint32_t)objects[i].zcm), 0xD1A40u), i});
        std::sort(rocks.begin(), rocks.end());
        const size_t n = rocks.size();
        size_t nd = n >= 6 ? std::max<size_t>(1, n / 25) : 0, ng = n >= 3 ? std::max<size_t>(2, n * 8 / 100) : 0;
        ng = std::min(ng, (n - nd) / 2);
        size_t ni = n >= 4 ? std::max<size_t>(2, n * 10 / 100) : 0;   // FERRO (3): depois de diamante e ouro, não altera os anteriores
        ni = std::min(ni, n - nd - ng);
        for (size_t k = 0; k < nd + ng + ni && k < n; ++k) ore_[rocks[k].second] = k < nd ? 2 : (k < nd + ng ? 1 : 3);
    }
    std::vector<uint32_t> count((size_t)kBuckets * kBuckets + 1, 0);
    auto range = [&](const WorldObject& o, int& bx0, int& bx1, int& bz0, int& bz1) {
        Collider c = colliderOf(o);
        float rx = c.kind == 2 ? c.a + c.b : c.a, rz = (c.kind == 1 || c.kind == 4) ? c.b : rx;
        int32_t px = (int32_t)(rx * 100) + 100, pz = (int32_t)(rz * 100) + 100;
        bx0 = std::max(0, (o.xcm - oxcm - px + kHalfCm) / kBucketCm); bx1 = std::min(kBuckets - 1, (o.xcm - oxcm + px + kHalfCm) / kBucketCm);
        bz0 = std::max(0, (o.zcm - ozcm - pz + kHalfCm) / kBucketCm); bz1 = std::min(kBuckets - 1, (o.zcm - ozcm + pz + kHalfCm) / kBucketCm);
    };
    for (const WorldObject& o : objects) {
        if (colliderOf(o).kind == 3) continue;
        int x0, x1, z0, z1; range(o, x0, x1, z0, z1);
        for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) ++count[(size_t)z * kBuckets + x + 1];
    }
    for (size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
    cellStart_ = count;
    cellItems_.assign(count.back(), 0);
    std::vector<uint32_t> fill(count.begin(), count.end() - 1);
    for (uint32_t i = 0; i < objects.size(); ++i) {
        if (colliderOf(objects[i]).kind == 3) continue;
        int x0, x1, z0, z1; range(objects[i], x0, x1, z0, z1);
        for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) cellItems_[fill[(size_t)z * kBuckets + x]++] = i;
    }
}

void World::shiftTo(int32_t newOxcm, int32_t newOzcm) {
    const int32_t dx = newOxcm - oxcm, dz = newOzcm - ozcm;
    for (WorldObject& o : objects) { o.xcm += dx; o.zcm += dz; }
    spawnXcm += dx; spawnZcm += dz;
    oxcm = newOxcm; ozcm = newOzcm;
    buildIndex();   // a grade espacial e o minério (hash da posição) passam a valer para as coordenadas globais
}

void World::pushOut(float& x, float& z, float r, float feetY) const {
    if (cellStart_.empty()) return;
    for (int iter = 0; iter < 2; ++iter) {
        const float lx = x - oxcm * 0.01f + kHalf, lz = z - ozcm * 0.01f + kHalf;   // coordenadas locais da grade (a ilha pode estar deslocada)
        if (lx < 0 || lz < 0) return;
        int bx = (int)(lx * 100.0f) / kBucketCm, bz = (int)(lz * 100.0f) / kBucketCm;
        if (bx >= kBuckets || bz >= kBuckets) return;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                int cx = bx + dx, cz = bz + dz;
                if (cx < 0 || cz < 0 || cx >= kBuckets || cz >= kBuckets) continue;
                size_t cell = (size_t)cz * kBuckets + cx;
                for (uint32_t k = cellStart_[cell]; k < cellStart_[cell + 1]; ++k) {
                    if (!removed_.empty() && removed_[cellItems_[k]]) continue;
                    const WorldObject& o = objects[cellItems_[k]];
                    Collider cl = colliderOf(o);
                    if (cl.kind == 2 && feetY > heightAt(o.xcm * 0.01f, o.zcm * 0.01f) + cl.a * 1.45f) continue;   // pulou por cima do tronco caído
                    float a = cl.a, b = cl.b;
                    float ox = o.xcm * 0.01f, oz = o.zcm * 0.01f;
                    if (cl.kind == 0 || cl.kind == 2) {
                        float cx0 = ox, cz0 = oz;  // círculo: o próprio centro; cápsula: ponto do eixo mais próximo do jogador
                        if (cl.kind == 2) { float t = (x - ox) * cl.dx + (z - oz) * cl.dz; t = std::fmax(-b, std::fmin(b, t)); cx0 = ox + cl.dx * t; cz0 = oz + cl.dz * t; }
                        float ddx = x - cx0, ddz = z - cz0, d2 = ddx * ddx + ddz * ddz, rr = a + r;
                        if (d2 < rr * rr) {
                            float d = std::sqrt(d2);
                            if (d < 1e-5f) { x = cx0 - cl.dz * rr * (cl.kind == 2 ? 1.0f : 0.0f) + (cl.kind == 2 ? 0.0f : rr); if (cl.kind == 2) z = cz0 + cl.dx * rr; }
                            else { x = cx0 + ddx / d * rr; z = cz0 + ddz / d * rr; }
                        }
                    } else {  // caixa(s) alinhada(s) aos eixos: construção antiga (kind 1) ou as partes em pé de uma ruína (kind 4)
                        auto pushBox = [&](float bx, float bz, float ba, float bb) {
                            float qx = std::fmax(-ba, std::fmin(ba, x - bx)), qz = std::fmax(-bb, std::fmin(bb, z - bz));
                            float ddx = x - bx - qx, ddz = z - bz - qz, d2 = ddx * ddx + ddz * ddz;
                            if (d2 < r * r) {
                                if (d2 > 1e-10f) { float d = std::sqrt(d2); x = bx + qx + ddx / d * r; z = bz + qz + ddz / d * r; }
                                else {  // centro dentro da caixa: sai pelo lado mais próximo
                                    float px = ba - std::fabs(x - bx), pz = bb - std::fabs(z - bz);
                                    if (px < pz) x = bx + ((x - bx) >= 0 ? ba + r : -(ba + r)); else z = bz + ((z - bz) >= 0 ? bb + r : -(bb + r));
                                }
                            }
                        };
                        if (cl.kind == 1) pushBox(ox, oz, a, b);
                        else {
                            RuinBox rb[kMaxRuinBoxes];
                            int kind = o.type == ObjCabin ? 0 : (o.type == ObjShed ? 1 : 2);
                            int nb = ruinSolidBoxes(kind, ruinSeed(o.xcm, o.zcm), rb, kMaxRuinBoxes);
                            float sc = o.scale100 * 0.01f; int q = (o.rot >> 6) & 3;   // giro em múltiplos de 90° (como no desenho)
                            for (int i = 0; i < nb; ++i) {
                                float x0 = rb[i].x0 * sc, z0 = rb[i].z0 * sc, x1 = rb[i].x1 * sc, z1 = rb[i].z1 * sc;
                                // local -> mundo (yaw = q*90°): x' = cos x + sin z ; z' = -sin x + cos z
                                float wx0, wz0, wx1, wz1;
                                if (q == 0) { wx0 = x0; wx1 = x1; wz0 = z0; wz1 = z1; }
                                else if (q == 1) { wx0 = z0; wx1 = z1; wz0 = -x1; wz1 = -x0; }
                                else if (q == 2) { wx0 = -x1; wx1 = -x0; wz0 = -z1; wz1 = -z0; }
                                else { wx0 = -z1; wx1 = -z0; wz0 = x0; wz1 = x1; }
                                pushBox(ox + (wx0 + wx1) * 0.5f, oz + (wz0 + wz1) * 0.5f, (wx1 - wx0) * 0.5f, (wz1 - wz0) * 0.5f);
                            }
                        }
                    }
                }
            }
    }
}

// Um ponto (tiro) está dentro de algum objeto sólido? Mesma geometria da colisão, com ALTURA por tipo (a bala passa por cima de pedras baixas, não por troncos).
bool World::blocksShot(float x, float y, float z) const {
    if (cellStart_.empty()) return false;
    const float lx = x - oxcm * 0.01f + kHalf, lz = z - ozcm * 0.01f + kHalf;
    if (lx < 0 || lz < 0) return false;
    const int bx = (int)(lx * 100.0f) / kBucketCm, bz = (int)(lz * 100.0f) / kBucketCm;
    if (bx >= kBuckets || bz >= kBuckets) return false;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = bx + dx, cz = bz + dz;
            if (cx < 0 || cz < 0 || cx >= kBuckets || cz >= kBuckets) continue;
            const size_t cell = (size_t)cz * kBuckets + cx;
            for (uint32_t k = cellStart_[cell]; k < cellStart_[cell + 1]; ++k) {
                if (!removed_.empty() && removed_[cellItems_[k]]) continue;
                const WorldObject& o = objects[cellItems_[k]];
                const Collider cl = colliderOf(o);
                const float ox = o.xcm * 0.01f, oz = o.zcm * 0.01f, sc = o.scale100 * 0.01f;
                float top;   // altura sólida acima do chão do objeto
                switch (o.type) {
                    case ObjPalm: case ObjCoconut: top = 6.5f * sc; break;
                    case ObjRock: top = 1.25f * sc; break;
                    case ObjLog: top = cl.a * 2.0f; break;
                    default: top = 3.0f; break;   // ruínas / construções
                }
                if (y < heightAt(ox, oz) - 0.3f || y > heightAt(ox, oz) + top) continue;
                if (cl.kind == 0 || cl.kind == 2) {
                    float cx0 = ox, cz0 = oz;
                    if (cl.kind == 2) { float t = (x - ox) * cl.dx + (z - oz) * cl.dz; t = std::fmax(-cl.b, std::fmin(cl.b, t)); cx0 = ox + cl.dx * t; cz0 = oz + cl.dz * t; }
                    const float ddx = x - cx0, ddz = z - cz0, rr = cl.a * (cl.kind == 0 && o.type == ObjRock ? 0.95f : 1.0f);
                    if (ddx * ddx + ddz * ddz < rr * rr) return true;
                } else if (cl.kind == 1) {
                    if (std::fabs(x - ox) < cl.a && std::fabs(z - oz) < cl.b) return true;
                } else if (cl.kind == 4) {
                    RuinBox rb[kMaxRuinBoxes];
                    const int kind = o.type == ObjCabin ? 0 : (o.type == ObjShed ? 1 : 2);
                    const int nb = ruinSolidBoxes(kind, ruinSeed(o.xcm, o.zcm), rb, kMaxRuinBoxes);
                    const int q = (o.rot >> 6) & 3;
                    for (int i = 0; i < nb; ++i) {
                        float x0 = rb[i].x0 * sc, z0 = rb[i].z0 * sc, x1 = rb[i].x1 * sc, z1 = rb[i].z1 * sc, wx0, wz0, wx1, wz1;
                        if (q == 0) { wx0 = x0; wx1 = x1; wz0 = z0; wz1 = z1; }
                        else if (q == 1) { wx0 = z0; wx1 = z1; wz0 = -x1; wz1 = -x0; }
                        else if (q == 2) { wx0 = -x1; wx1 = -x0; wz0 = -z1; wz1 = -z0; }
                        else { wx0 = -z1; wx1 = -z0; wz0 = x0; wz1 = x1; }
                        if (x >= ox + wx0 && x <= ox + wx1 && z >= oz + wz0 && z <= oz + wz1) return true;
                    }
                }
            }
        }
    return false;
}

World::Stats World::stats() const {
    Stats s;
    size_t nv = hcm.size();
    int64_t cnt[BiomeCount] = {};
    int minx = kVerts, maxx = -1, minz = kVerts, maxz = -1;
    for (int iz = 0; iz < kVerts; ++iz)
        for (int ix = 0; ix < kVerts; ++ix) {
            size_t i = (size_t)iz * kVerts + ix;
            ++cnt[biome[i]];
            if (hcm[i] > 0) {
                ++s.landVerts; s.maxH = std::max(s.maxH, hcm[i] * 0.01f);
                minx = std::min(minx, ix); maxx = std::max(maxx, ix); minz = std::min(minz, iz); maxz = std::max(maxz, iz);
            }
        }
    for (int b = 0; b < BiomeCount; ++b) s.pct[b] = 100.0 * cnt[b] / (double)nv;
    for (const WorldObject& o : objects) if (o.type < 12) ++s.count[o.type];
    if (maxx >= 0) s.extentM = std::max(maxx - minx, maxz - minz) * kCell;
    return s;
}

}  // namespace e0net
