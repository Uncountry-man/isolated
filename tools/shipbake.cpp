// Pós-processo do navio (roda no PC, chamado por tools/export_ship.py). Formato de entrada e saída: E0S1 (ver export_ship.py).
//   build/shipbake entrada.e0s saida.e0s [aresta_max_m]
//   build/shipbake --preview navio.e0s saida.ppm  ex ey ez  tx ty tz     (imagem com a MESMA luz do shader: cor * (ambiente + sol * ndl * sombra))
// O GLB vem com normais invertidas em boa parte da malha (material double-sided esconde isso no Sketchfab). Com a normal do convés
// apontando para baixo, ndl = 0: o sol e a sombra projetada não têm efeito e sobra só o ambiente, tudo chapado. Aqui, com raios:
//  1. subdivide faces grandes (o convés tem triângulos de 3 m²; sem isso a oclusão por vértice não teria resolução);
//  2. descobre o lado EXPOSTO de cada triângulo original (o que tem mais espaço livre) e vira a normal e o sentido para ele;
//  3. grava a oclusão de ambiente (cantos, sob o toldo, junto das amuradas) na cor do vértice: zero custo em runtime.
// NÃO rode duas vezes sobre o mesmo arquivo (a oclusão multiplicaria de novo): a entrada é sempre a saída crua do export_ship.py.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(V3 a) { return std::sqrt(dot(a, a)); }
static V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 1, 0}; }

struct Vert { V3 p, n; float c[3]; uint8_t a = 255; bool sub = false; };   // a: alfa do vértice (252 = brilho de ouro no shader; 254 = vela: sem subdividir nem oclusão, sai como 255)
struct Tri { uint32_t v[3]; uint32_t parent; };

// ---- arquivo ----
#pragma pack(push, 1)
struct FileVert { float p[3]; int8_t n[4]; uint8_t c[4]; };
#pragma pack(pop)
static_assert(sizeof(FileVert) == 20, "E0S1 usa 20 B por vértice");

static bool gFlagged = false;   // o arquivo marca o que subdividir (galeão); sem marca vale needsDetail por cor e altura
static bool load(const char* path, std::vector<Vert>& vs, std::vector<Tri>& ts) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char mg[4]; uint32_t nv = 0, ni = 0;
    if (std::fread(mg, 1, 4, f) != 4 || std::memcmp(mg, "E0S1", 4) || std::fread(&nv, 4, 1, f) != 1 || std::fread(&ni, 4, 1, f) != 1) { std::fclose(f); return false; }
    std::vector<FileVert> fv(nv); std::vector<uint32_t> id(ni);
    bool ok = std::fread(fv.data(), sizeof(FileVert), nv, f) == nv && std::fread(id.data(), 4, ni, f) == ni;
    std::fclose(f);
    if (!ok) return false;
    vs.resize(nv);
    for (uint32_t i = 0; i < nv; ++i) {
        vs[i].p = {fv[i].p[0], fv[i].p[1], fv[i].p[2]};
        vs[i].n = norm({fv[i].n[0] / 127.0f, fv[i].n[1] / 127.0f, fv[i].n[2] / 127.0f});
        for (int k = 0; k < 3; ++k) vs[i].c[k] = (float)fv[i].c[k];
        vs[i].a = fv[i].c[3];
        vs[i].sub = fv[i].n[3] != 0; gFlagged = gFlagged || vs[i].sub;   // byte livre da normal: 1 = esta peça pode ser subdividida
    }
    ts.resize(ni / 3);
    for (uint32_t i = 0; i < ni / 3; ++i) ts[i] = {{id[i * 3], id[i * 3 + 1], id[i * 3 + 2]}, i};
    return true;
}

static bool save(const char* path, const std::vector<Vert>& vs, const std::vector<Tri>& ts) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t nv = (uint32_t)vs.size(), ni = (uint32_t)ts.size() * 3;
    std::fwrite("E0S1", 1, 4, f); std::fwrite(&nv, 4, 1, f); std::fwrite(&ni, 4, 1, f);
    for (const Vert& v : vs) {
        FileVert o{};
        o.p[0] = v.p.x; o.p[1] = v.p.y; o.p[2] = v.p.z;
        o.n[0] = (int8_t)std::lround(v.n.x * 127); o.n[1] = (int8_t)std::lround(v.n.y * 127); o.n[2] = (int8_t)std::lround(v.n.z * 127);
        for (int k = 0; k < 3; ++k) o.c[k] = (uint8_t)std::clamp((int)std::lround(v.c[k]), 0, 255);
        o.c[3] = v.a == 254 ? 255 : v.a;
        std::fwrite(&o, sizeof o, 1, f);
    }
    for (const Tri& t : ts) std::fwrite(t.v, 4, 3, f);
    std::fclose(f);
    return true;
}

// ---- BVH de triângulos ----
struct Node { float lo[3], hi[3]; uint32_t left, first, count; };   // count > 0 = folha
struct Bvh {
    const std::vector<Vert>* vs; std::vector<Tri> tris; std::vector<Node> nodes;
    V3 P(const Tri& t, int k) const { return (*vs)[t.v[k]].p; }
    void bounds(Node& n, uint32_t a, uint32_t b) {
        for (int k = 0; k < 3; ++k) { n.lo[k] = 1e30f; n.hi[k] = -1e30f; }
        for (uint32_t i = a; i < b; ++i) for (int j = 0; j < 3; ++j) {
            V3 p = P(tris[i], j); float q[3] = {p.x, p.y, p.z};
            for (int k = 0; k < 3; ++k) { n.lo[k] = std::min(n.lo[k], q[k]); n.hi[k] = std::max(n.hi[k], q[k]); }
        }
    }
    uint32_t build(uint32_t a, uint32_t b) {
        uint32_t me = (uint32_t)nodes.size(); nodes.push_back({});
        Node n{}; bounds(n, a, b);
        if (b - a <= 4) { n.first = a; n.count = b - a; nodes[me] = n; return me; }
        int ax = 0; float ext = -1;
        for (int k = 0; k < 3; ++k) if (n.hi[k] - n.lo[k] > ext) { ext = n.hi[k] - n.lo[k]; ax = k; }
        auto key = [&](const Tri& t) { V3 c = P(t, 0) + P(t, 1) + P(t, 2); return ax == 0 ? c.x : ax == 1 ? c.y : c.z; };
        uint32_t m = (a + b) / 2;
        std::nth_element(tris.begin() + a, tris.begin() + m, tris.begin() + b, [&](const Tri& p, const Tri& q) { return key(p) < key(q); });
        n.left = build(a, m);
        uint32_t right = build(m, b); (void)right;
        n.first = right; n.count = 0;   // nó interno: first = filho da direita
        nodes[me] = n;
        return me;
    }
    void init(const std::vector<Vert>& v, const std::vector<Tri>& t) { vs = &v; tris = t; nodes.clear(); nodes.reserve(t.size() * 2); build(0, (uint32_t)tris.size()); }
    // Devolve a distância do primeiro acerto (ou tmax); idx = triângulo acertado em `tris`, bu/bv = baricêntricas
    float trace(V3 o, V3 d, float tmax, int* idx = nullptr, float* bu = nullptr, float* bv = nullptr) const {
        float inv[3] = {1.0f / (d.x != 0 ? d.x : 1e-20f), 1.0f / (d.y != 0 ? d.y : 1e-20f), 1.0f / (d.z != 0 ? d.z : 1e-20f)};
        float oo[3] = {o.x, o.y, o.z};
        uint32_t st[64]; int sp = 0; st[sp++] = 0;
        float best = tmax;
        while (sp) {
            const Node& n = nodes[st[--sp]];
            float t0 = 0, t1 = best;
            for (int k = 0; k < 3; ++k) {
                float a = (n.lo[k] - oo[k]) * inv[k], b = (n.hi[k] - oo[k]) * inv[k];
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a); t1 = std::min(t1, b);
            }
            if (t0 > t1) continue;
            if (n.count == 0) { st[sp++] = n.left; st[sp++] = n.first; continue; }
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                const Tri& t = tris[i];
                V3 a = P(t, 0), e1 = P(t, 1) - a, e2 = P(t, 2) - a;
                V3 pv = cross(d, e2); float det = dot(e1, pv);
                if (std::fabs(det) < 1e-12f) continue;
                float id = 1.0f / det; V3 tv = o - a;
                float u = dot(tv, pv) * id; if (u < 0 || u > 1) continue;
                V3 qv = cross(tv, e1);
                float v = dot(d, qv) * id; if (v < 0 || u + v > 1) continue;
                float tt = dot(e2, qv) * id;
                if (tt > 1e-4f && tt < best) { best = tt; if (idx) { *idx = (int)i; *bu = u; *bv = v; } }
            }
        }
        return best;
    }
};

// ---- amostragem determinística ----
static uint32_t hashU(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static float rnd(uint32_t s) { return (float)(hashU(s) >> 8) * (1.0f / 16777216.0f); }
static void basis(V3 n, V3& t, V3& b) { t = norm(cross(std::fabs(n.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}, n)); b = cross(n, t); }
static V3 cosDir(V3 n, V3 t, V3 b, float u, float v) {
    float r = std::sqrt(u), ph = 6.2831853f * v;
    return norm(t * (r * std::cos(ph)) + b * (r * std::sin(ph)) + n * std::sqrt(std::max(0.0f, 1.0f - u)));
}

template <class F> static void parallel(size_t n, F fn) {
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    for (unsigned i = 0; i < nt; ++i) th.emplace_back([&] { for (;;) { size_t a = next.fetch_add(256); if (a >= n) break; for (size_t k = a; k < std::min(n, a + 256); ++k) fn(k); } });
    for (auto& t : th) t.join();
}

// ---- 1. subdivisão por comprimento de aresta (o critério só olha a aresta: os 2 lados dela concordam, sem rachaduras) ----
// Só a madeira acima da água: velas e canhões têm cor própria e o mar cobre o fundo do casco; cada triângulo a mais custa 2x (passe principal e de sombra).
static bool isWood(const Vert& v) { return v.a != 254 && v.c[0] > v.c[2] + 20.0f; }
static bool needsDetail(const Vert& v) { return v.p.y > -0.4f && (gFlagged ? v.sub : isWood(v)); }
static void subdivide(std::vector<Vert>& vs, std::vector<Tri>& ts, float maxEdge) {
    for (int pass = 0; pass < 12; ++pass) {
        std::map<uint64_t, uint32_t> mids;
        auto mid = [&](uint32_t a, uint32_t b) -> int {
            if (!needsDetail(vs[a]) || !needsDetail(vs[b]) || len(vs[a].p - vs[b].p) <= maxEdge) return -1;
            uint64_t key = ((uint64_t)std::min(a, b) << 32) | std::max(a, b);
            auto it = mids.find(key);
            if (it != mids.end()) return (int)it->second;
            Vert m;
            m.p = (vs[a].p + vs[b].p) * 0.5f; m.n = norm(vs[a].n + vs[b].n); m.a = vs[a].a; m.sub = vs[a].sub;
            for (int k = 0; k < 3; ++k) m.c[k] = 0.5f * (vs[a].c[k] + vs[b].c[k]);
            vs.push_back(m);
            uint32_t id = (uint32_t)vs.size() - 1; mids[key] = id;
            return (int)id;
        };
        std::vector<Tri> out; out.reserve(ts.size() * 2);
        bool any = false;
        for (const Tri& t : ts) {
            uint32_t a = t.v[0], b = t.v[1], c = t.v[2];
            int ab = mid(a, b), bc = mid(b, c), ca = mid(c, a), n = (ab >= 0) + (bc >= 0) + (ca >= 0);
            auto put = [&](uint32_t x, uint32_t y, uint32_t z) { out.push_back({{x, y, z}, t.parent}); };
            if (n == 0) { out.push_back(t); continue; }
            any = true;
            if (n == 3) { put(a, ab, ca); put(ab, b, bc); put(ca, bc, c); put(ab, bc, ca); continue; }
            if (n == 1) {
                if (ab >= 0) { put(a, ab, c); put(ab, b, c); }
                else if (bc >= 0) { put(b, bc, a); put(bc, c, a); }
                else { put(c, ca, b); put(ca, a, b); }
                continue;
            }
            if (ab < 0) { put(c, ca, bc); put(ca, a, bc); put(a, b, bc); }        // 2 arestas: triângulo no vértice comum + quadrilátero
            else if (bc < 0) { put(a, ab, ca); put(ab, b, ca); put(b, c, ca); }
            else { put(b, bc, ab); put(bc, c, ab); put(c, a, ab); }
        }
        ts.swap(out);
        if (!any) break;
    }
}

// ---- 2. lado exposto de cada triângulo original ----
static void orient(std::vector<Vert>& vs, std::vector<Tri>& ts, const Bvh& bvh, uint32_t parents) {
    constexpr int kRays = 24; constexpr float kFar = 8.0f;
    struct Acc { double pos = 0, neg = 0; float fx = 0, fy = 0, fz = 0; };
    std::vector<float> sp(ts.size()), sn(ts.size()), wt(ts.size());
    std::vector<V3> fn(ts.size());
    parallel(ts.size(), [&](size_t i) {
        const Tri& t = ts[i];
        V3 a = vs[t.v[0]].p, b = vs[t.v[1]].p, c = vs[t.v[2]].p, g = cross(b - a, c - a);
        float ar = len(g) * 0.5f;
        wt[i] = ar; fn[i] = norm(g); sp[i] = sn[i] = 0;
        if (ar < 1e-9f) return;
        V3 n = fn[i], tt, bb; basis(n, tt, bb);
        V3 cen = (a + b + c) * (1.0f / 3.0f);
        for (int side = 0; side < 2; ++side) {
            V3 s = side ? n * -1.0f : n;
            float acc = 0;
            for (int r = 0; r < kRays; ++r) {
                uint32_t h = (uint32_t)i * 977u + (uint32_t)r * 2u + (uint32_t)side;
                acc += bvh.trace(cen + s * 0.006f, cosDir(s, tt, bb, rnd(h), rnd(h ^ 0x9e3779b9u)), kFar);
            }
            (side ? sn[i] : sp[i]) = acc / (kRays * kFar);
        }
    });
    // o voto é por triângulo ORIGINAL: pedaços de uma mesma face não discordam entre si. fn de todos os filhos é igual (mesmo plano), sentido inclusive.
    std::vector<Acc> acc(parents);
    for (size_t i = 0; i < ts.size(); ++i) { Acc& q = acc[ts[i].parent]; q.pos += (double)sp[i] * wt[i]; q.neg += (double)sn[i] * wt[i]; q.fx = fn[i].x; q.fy = fn[i].y; q.fz = fn[i].z; }
    std::map<uint64_t, uint32_t> flipped;
    size_t nFlip = 0, nTie = 0, nSwap = 0;
    double total = 0, wrong = 0;
    for (size_t i = 0; i < ts.size(); ++i) {
        Tri& t = ts[i];
        const Acc& q = acc[t.parent];
        double sum = q.pos + q.neg;
        V3 out;
        if (sum > 0 && std::fabs(q.pos - q.neg) < 0.04 * sum) {   // os 2 lados igualmente abertos (vela, bandeira): fica com a normal do arquivo
            ++nTie;
            V3 avg = vs[t.v[0]].n + vs[t.v[1]].n + vs[t.v[2]].n;
            out = dot(avg, fn[i]) >= 0 ? fn[i] : fn[i] * -1.0f;
        } else out = q.pos >= q.neg ? fn[i] : fn[i] * -1.0f;
        for (int k = 0; k < 3; ++k) {
            if (vs[t.v[k]].a == 254) continue;   // vela: a normal já vem pronta do exportador (aponta para cima: fica iluminada dos 2 lados)
            total += wt[i];
            if (dot(vs[t.v[k]].n, out) >= 0) continue;
            wrong += wt[i];
            uint64_t key = t.v[k];
            auto it = flipped.find(key);
            uint32_t id;
            if (it != flipped.end()) id = it->second;
            else { Vert m = vs[t.v[k]]; m.n = m.n * -1.0f; vs.push_back(m); id = (uint32_t)vs.size() - 1; flipped[key] = id; ++nFlip; }
            t.v[k] = id;
        }
        if (dot(fn[i], out) < 0) { std::swap(t.v[1], t.v[2]); ++nSwap; }   // sentido do triângulo = lado exposto
    }
    std::printf("normais: %.0f%% da area estava virada para dentro; %zu vertices duplicados, %zu triangulos reorientados, %zu sem lado definido (velas)\n",
                100.0 * wrong / total, nFlip, nSwap, nTie);
}

// ---- 3. oclusão de ambiente na cor do vértice ----
static void bakeAo(std::vector<Vert>& vs, const Bvh& bvh) {
    constexpr int kRays = 128; constexpr float kFar = 4.5f, kFloor = 0.22f;
    std::vector<float> ao(vs.size());
    parallel(vs.size(), [&](size_t i) {
        V3 n = vs[i].n, t, b; basis(n, t, b);
        V3 o = vs[i].p + n * 0.02f;
        float occ = 0;
        for (int r = 0; r < kRays; ++r) {
            uint32_t h = (uint32_t)i * 131u + (uint32_t)r;
            float d = bvh.trace(o, cosDir(n, t, b, (r + rnd(h)) / kRays, rnd(h ^ 0x85ebca6bu)), kFar);
            if (d < kFar) occ += 1.0f - d / kFar;
        }
        float vis = 1.0f - occ / kRays;
        ao[i] = kFloor + (1.0f - kFloor) * std::pow(std::clamp(vis, 0.0f, 1.0f), 1.6f);
    });
    double s = 0;
    for (size_t i = 0; i < vs.size(); ++i) {
        if (vs[i].a == 254 || (!isWood(vs[i]) && vs[i].p.y > 5.0f)) continue;   // vela = poucos triângulos enormes: a oclusão por vértice viraria manchas facetadas
        for (int k = 0; k < 3; ++k) vs[i].c[k] *= ao[i];
        s += ao[i];
    }
    std::printf("oclusao: media %.2f\n", s / vs.size());
}

// Tira os vértices que ficaram sem triângulo (todos os que usavam o original passaram para a cópia virada)
static void compact(std::vector<Vert>& vs, std::vector<Tri>& ts) {
    std::vector<int> remap(vs.size(), -1);
    std::vector<Vert> out; out.reserve(vs.size());
    for (Tri& t : ts) for (uint32_t& v : t.v) {
        if (remap[v] < 0) { remap[v] = (int)out.size(); out.push_back(vs[v]); }
        v = (uint32_t)remap[v];
    }
    vs.swap(out);
}

// ---- preview: mesma fórmula de kPropMain, sem culling (como drawShip) ----
static int preview(const char* in, const char* out, V3 eye, V3 tgt) {
    std::vector<Vert> vs; std::vector<Tri> ts;
    if (!load(in, vs, ts)) { std::fprintf(stderr, "nao li %s\n", in); return 1; }
    Bvh bvh; bvh.init(vs, ts);
    const int W = 960, H = 540;
    const V3 sun = norm({0.55f, 0.75f, 0.35f}), amb = {0.40f, 0.44f, 0.52f}, sunC = {1.05f, 0.98f, 0.88f};
    V3 f = norm(tgt - eye), r = norm(cross(f, {0, 1, 0})), u = cross(r, f);
    float th = std::tan(0.5f * 92.0f * 3.14159265f / 180.0f);
    std::vector<uint8_t> img((size_t)W * H * 3);
    parallel((size_t)W * H, [&](size_t i) {
        int x = (int)(i % W), y = (int)(i / W);
        V3 d = norm(f + r * (((x + 0.5f) / W * 2 - 1) * th) + u * ((1 - (y + 0.5f) / H * 2) * th * H / W));
        int hit = -1; float bu = 0, bv = 0;
        float t = bvh.trace(eye, d, 500.0f, &hit, &bu, &bv);
        V3 col;
        if (hit < 0) col = d.y < 0 ? V3{0.06f, 0.30f, 0.38f} : V3{0.55f, 0.72f, 0.90f};
        else {
            const Tri& q = bvh.tris[hit];
            const Vert &A = vs[q.v[0]], &B = vs[q.v[1]], &C = vs[q.v[2]];
            float w = 1 - bu - bv;
            V3 n = norm(A.n * w + B.n * bu + C.n * bv), p = eye + d * t;
            float ndl = std::max(dot(n, sun), 0.0f), sh = 1;
            V3 geo = norm(cross(B.p - A.p, C.p - A.p));
            if (dot(geo, d) > 0) geo = geo * -1.0f;
            if (ndl > 0 && bvh.trace(p + geo * 0.03f, sun, 80.0f) < 80.0f) sh = 0;
            float c[3]; for (int k = 0; k < 3; ++k) c[k] = (A.c[k] * w + B.c[k] * bu + C.c[k] * bv) / 255.0f;
            col = {c[0] * (amb.x + sunC.x * ndl * sh), c[1] * (amb.y + sunC.y * ndl * sh), c[2] * (amb.z + sunC.z * ndl * sh)};
        }
        uint8_t* o = &img[i * 3];
        o[0] = (uint8_t)(std::clamp(col.x, 0.0f, 1.0f) * 255); o[1] = (uint8_t)(std::clamp(col.y, 0.0f, 1.0f) * 255); o[2] = (uint8_t)(std::clamp(col.z, 0.0f, 1.0f) * 255);
    });
    FILE* fo = std::fopen(out, "wb");
    if (!fo) return 1;
    std::fprintf(fo, "P6\n%d %d\n255\n", W, H);
    std::fwrite(img.data(), 1, img.size(), fo);
    std::fclose(fo);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 10 && !std::strcmp(argv[1], "--preview")) {
        auto a = [&](int i) { return (float)std::atof(argv[i]); };
        return preview(argv[2], argv[3], {a(4), a(5), a(6)}, {a(7), a(8), a(9)});
    }
    if (argc != 3 && argc != 4) { std::fprintf(stderr, "uso: shipbake entrada.e0s saida.e0s [aresta_max] | --preview navio.e0s saida.ppm ex ey ez tx ty tz\n"); return 2; }
    std::vector<Vert> vs; std::vector<Tri> ts;
    if (!load(argv[1], vs, ts)) { std::fprintf(stderr, "nao li %s\n", argv[1]); return 1; }
    uint32_t parents = (uint32_t)ts.size();
    std::printf("entrada: %zu vertices, %zu triangulos\n", vs.size(), ts.size());
    subdivide(vs, ts, argc == 4 ? (float)std::atof(argv[3]) : 0.8f);
    std::printf("subdividido: %zu vertices, %zu triangulos\n", vs.size(), ts.size());
    Bvh bvh; bvh.init(vs, ts);
    orient(vs, ts, bvh, parents);
    bvh.init(vs, ts);
    bakeAo(vs, bvh);
    compact(vs, ts);
    if (!save(argv[2], vs, ts)) return 1;
    std::printf("saida: %zu vertices, %zu triangulos -> %s\n", vs.size(), ts.size(), argv[2]);
    return 0;
}
