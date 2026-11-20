// Ferramenta de ajuste: caixa (min/max) dos vértices do Y Bot no ÚLTIMO quadro de um clipe, em espaço do modelo (pés = origem, frente = -Z).
// Uso: build/posebox [clipe=22] [linha_do_tempo]   (ex.: 22 = MORRER2 de bruços, 18 = MORRER)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../engine/src/anim.h"

using namespace e0;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

struct SkinnedVert {
    Vec3 p, n;
    uint8_t b[4], w[4];
};

static bool loadE0M2(const char* path, std::vector<SkinnedVert>& out) {
    out.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 8) { std::fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)sz);
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fclose(f); return false; }
    std::fclose(f);
    if (std::memcmp(buf.data(), "E0M2", 4) != 0) return false;
    uint32_t nSub = 0;
    std::memcpy(&nSub, buf.data() + 4, 4);
    size_t o = 8;
    for (uint32_t s = 0; s < nSub; ++s) {
        if (o + 20 > buf.size()) return false;
        uint32_t nv = 0, ni = 0;
        std::memcpy(&nv, buf.data() + o + 12, 4);
        std::memcpy(&ni, buf.data() + o + 16, 4);
        o += 20;
        if (o + (size_t)nv * 32 + (size_t)ni * 4 > buf.size()) return false;
        for (uint32_t i = 0; i < nv; ++i) {
            SkinnedVert v;
            std::memcpy(&v.p, buf.data() + o, 12);
            std::memcpy(&v.n, buf.data() + o + 12, 12);
            std::memcpy(v.b, buf.data() + o + 24, 4);
            std::memcpy(v.w, buf.data() + o + 28, 4);
            out.push_back(v);
            o += 32;
        }
        o += (size_t)ni * 4;
    }
    return !out.empty();
}

static Vec3 evalVertex(const SkinnedVert& v, const Vec4* rows) {
    Vec4 r0{}, r1{}, r2{};
    for (int k = 0; k < 4; ++k) {
        float wf = v.w[k] / 255.0f;
        int idx = v.b[k] * 3;
        r0.x += rows[idx + 0].x * wf; r0.y += rows[idx + 0].y * wf; r0.z += rows[idx + 0].z * wf; r0.w += rows[idx + 0].w * wf;
        r1.x += rows[idx + 1].x * wf; r1.y += rows[idx + 1].y * wf; r1.z += rows[idx + 1].z * wf; r1.w += rows[idx + 1].w * wf;
        r2.x += rows[idx + 2].x * wf; r2.y += rows[idx + 2].y * wf; r2.z += rows[idx + 2].z * wf; r2.w += rows[idx + 2].w * wf;
    }
    return {
        r0.x * v.p.x + r0.y * v.p.y + r0.z * v.p.z + r0.w,
        r1.x * v.p.x + r1.y * v.p.y + r1.z * v.p.z + r1.w,
        r2.x * v.p.x + r2.y * v.p.y + r2.z * v.p.z + r2.w,
    };
}

static int timeline(AnimBank& bank, int clip, const std::vector<SkinnedVert>& v) {   // movimento por quadro: acha onde o clipe PARA de mexer
    int nf = (int)(bank.clipDuration(clip) * 30.0f + 0.5f); std::vector<Vec3> prev; BoneTransform pose[kMaxBones]; Vec4 rows[kBoneRows];
    for (int f = 0; f <= nf; ++f) {
        bank.sampleTime(clip, std::min(bank.clipDuration(clip), f / 30.0f), pose); bank.computeSkinning(pose, rows);
        std::vector<Vec3> cur; for (size_t i = 0; i < v.size(); i += 5) cur.push_back(evalVertex(v[i], rows));
        float d = 0, top = -1e9f; if (!prev.empty()) for (size_t i = 0; i < cur.size(); ++i) { Vec3 q = cur[i] - prev[i]; d = std::max(d, std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z)); }
        for (const Vec3& c : cur) top = std::max(top, c.y);
        std::printf("quadro %3d  t=%.2fs  movimento max %.4f m  altura %.2f\n", f, f / 30.0f, d, top);
        prev = cur;
    }
    return 0;
}

int main(int argc, char** argv) {
    int clip = argc > 1 ? std::atoi(argv[1]) : ClipDeathFwd;
    AnimBank bank;
    if (!bank.loadFromFile("assets/ybot.e0a") || !bank.valid()) { std::printf("sem assets/ybot.e0a\n"); return 1; }
    std::vector<SkinnedVert> v;
    if (!loadE0M2("assets/ybot_lod1.e0m", v)) { std::printf("sem malha\n"); return 1; }
    if (argc > 2) return timeline(bank, clip, v);
    BoneTransform pose[kMaxBones]; Vec4 rows[kBoneRows];
    bank.sampleTime(clip, bank.clipDuration(clip), pose);
    bank.computeSkinning(pose, rows);
    Vec3 mn{1e9f, 1e9f, 1e9f}, mx{-1e9f, -1e9f, -1e9f}, head{0, -1e9f, 0};
    for (const SkinnedVert& s : v) {
        Vec3 p = evalVertex(s, rows);
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)}; mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    std::printf("clipe %d (%s) quadro final: x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f]  centro (%.2f %.2f %.2f)\n", clip, AnimBank::clipLabel(clip), mn.x, mx.x, mn.y, mx.y, mn.z, mx.z, (mn.x + mx.x) / 2, (mn.y + mx.y) / 2, (mn.z + mx.z) / 2);
    // cabeça = vértices mais altos do corpo original (osso 5 = Head): posição média dos vértices ligados a ele
    Vec3 hs{0, 0, 0}; int hn = 0;
    for (const SkinnedVert& s : v) if (s.b[0] == 5 && s.w[0] > 200) { Vec3 p = evalVertex(s, rows); hs = hs + p; ++hn; }
    { Vec3 q{0, 0, 0}; int qn = 0; for (const SkinnedVert& s2 : v) if (s2.b[0] == 0 && s2.w[0] > 200) { q = q + evalVertex(s2, rows); ++qn; } if (qn) std::printf("quadril (osso 0) em (%.2f %.2f %.2f)\n", q.x / qn, q.y / qn, q.z / qn); }
    if (hn) std::printf("cabeça (osso 5) em (%.2f %.2f %.2f)\n", hs.x / hn, hs.y / hn, hs.z / hn);
    return 0;
}
