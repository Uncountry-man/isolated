// Pré-visualização offline do CARREGAR no ombro: desenha quem carrega (azul) e o corpo (vermelho) como pontos com z-buffer, em 4 vistas, sem precisar do celular.
// Uso: build/carrypreview saida.ppm [zoom=1] [so_quem_carrega]   (depois: python3 -c "from PIL import Image; Image.open('saida.ppm').save('saida.png')")
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../engine/src/anim.h"
using namespace e0;
struct SV { Vec3 p, n; uint8_t b[4], w[4]; };
static bool load(const char* path, std::vector<SV>& out) {
    FILE* f = std::fopen(path, "rb"); if (!f) return false;
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t)sz); if (std::fread(d.data(), 1, d.size(), f) != d.size()) return false; std::fclose(f);
    uint32_t ns; std::memcpy(&ns, d.data() + 4, 4); size_t o = 8;
    for (uint32_t s = 0; s < ns; ++s) {
        uint32_t nv, ni; std::memcpy(&nv, d.data() + o + 12, 4); std::memcpy(&ni, d.data() + o + 16, 4); o += 20;
        for (uint32_t i = 0; i < nv; ++i) { SV v; std::memcpy(&v.p, d.data() + o, 12); std::memcpy(&v.n, d.data() + o + 12, 12); std::memcpy(v.b, d.data() + o + 24, 4); std::memcpy(v.w, d.data() + o + 28, 4); out.push_back(v); o += 32; }
        o += (size_t)ni * 4;
    }
    return true;
}
static void skin(const SV& v, const Vec4* R, Vec3& p, Vec3& n) {
    Vec4 r[3] = {};
    for (int k = 0; k < 4; ++k) { float w = v.w[k] / 255.0f; for (int j = 0; j < 3; ++j) { const Vec4& q = R[v.b[k] * 3 + j]; r[j].x += q.x * w; r[j].y += q.y * w; r[j].z += q.z * w; r[j].w += q.w * w; } }
    p = {r[0].x * v.p.x + r[0].y * v.p.y + r[0].z * v.p.z + r[0].w, r[1].x * v.p.x + r[1].y * v.p.y + r[1].z * v.p.z + r[1].w, r[2].x * v.p.x + r[2].y * v.p.y + r[2].z * v.p.z + r[2].w};
    n = normalize(Vec3{r[0].x * v.n.x + r[0].y * v.n.y + r[0].z * v.n.z, r[1].x * v.n.x + r[1].y * v.n.y + r[1].z * v.n.z, r[2].x * v.n.x + r[2].y * v.n.y + r[2].z * v.n.z});
}
int main(int argc, char** argv) {
    AnimBank bank; std::vector<SV> m;
    if (!bank.loadFromFile("assets/ybot.e0a") || !load("assets/ybot.e0m", m)) return 1;
    AnimPlayer a, c; a.reset(); c.reset();
    AnimInput ia{}; ia.grounded = true; ia.carrying = true;
    AnimInput ic{}; ic.grounded = true; ic.dead = true; ic.carried = true;
    for (int i = 0; i < 60; ++i) { a.update(1 / 60.f, ia, bank); c.update(1 / 60.f, ic, bank); }
    const Vec4* S = a.boneRows() + 3 * 3;   // Spine2 de quem carrega: o corpo acompanha o tronco
    const float Z = argc > 2 ? (float)std::atof(argv[2]) : 1.0f; const int W = 480, H = 620, NV = 4; const float y0 = Z > 1.0f ? 0.55f : 0.0f; std::vector<uint8_t> img((size_t)W * NV * H * 3, 30); std::vector<float> zb((size_t)W * NV * H, -1e9f);
    const float yaws[NV] = {3.14159f, 0.0f, 1.5708f, -0.7f};   // frente, costas, lado direito, 3/4
    for (int view = 0; view < NV; ++view) {
        float cs = std::cos(yaws[view]), sn = std::sin(yaws[view]);
        for (int who = 0; who < (argc > 3 ? 1 : 2); ++who) for (const SV& v : m) {
            Vec3 p, n; skin(v, who ? c.boneRows() : a.boneRows(), p, n);
            if (who) {
                Vec3 q{S[0].x * p.x + S[0].y * p.y + S[0].z * p.z + S[0].w, S[1].x * p.x + S[1].y * p.y + S[1].z * p.z + S[1].w, S[2].x * p.x + S[2].y * p.y + S[2].z * p.z + S[2].w};
                Vec3 nn{S[0].x * n.x + S[0].y * n.y + S[0].z * n.z, S[1].x * n.x + S[1].y * n.y + S[1].z * n.z, S[2].x * n.x + S[2].y * n.y + S[2].z * n.z}; p = q; n = nn;
            }
            float x = p.x * cs + p.z * sn, z = -p.x * sn + p.z * cs, nz = -n.x * sn + n.z * cs;   // câmera em +Z da vista
            int sx = view * W + W / 2 + (int)(x * 240 * Z), sy = H - 20 - (int)((p.y - y0) * 240 * Z);
            float l = 0.25f + 0.75f * std::max(0.0f, 0.3f * n.y + 0.9f * nz);
            const int rad = Z > 1.0f ? 3 : 1;
            for (int dy = -rad; dy <= rad; ++dy) for (int dx = -rad; dx <= rad; ++dx) {
                int X = sx + dx, Y = sy + dy; if (X < view * W || X >= (view + 1) * W || Y < 0 || Y >= H) continue;
                size_t i = (size_t)Y * W * NV + X; if (z <= zb[i]) continue; zb[i] = z;
                img[i * 3] = (uint8_t)(who ? 230 * l : 40 * l); img[i * 3 + 1] = (uint8_t)(who ? 70 * l : 120 * l); img[i * 3 + 2] = (uint8_t)(who ? 60 * l : 220 * l);
            }
        }
    }
    FILE* f = std::fopen(argc > 1 ? argv[1] : "carry.ppm", "wb"); std::fprintf(f, "P6\n%d %d\n255\n", W * NV, H); std::fwrite(img.data(), 1, img.size(), f); std::fclose(f);
    return 0;
}
