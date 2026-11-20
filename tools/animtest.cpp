// Teste automatizado do sistema de animação esquelética (AnimBank / AnimPlayer) e malhas E0M2.
// Uso: build/animtest
#include <cmath>
#include <cstdio>
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

int main() {
    AnimBank bank;
    CHECK(bank.loadFromFile("assets/ybot.e0a") && bank.valid(), "ybot.e0a carregado (%d ossos, %d clipes)", bank.numBones(), bank.numClips());
    if (!bank.valid()) return 1;

    // 1. Pose de repouso -> matrizes identidade
    BoneTransform rest[kMaxBones];
    Vec4 rows[kBoneRows];
    bank.restPose(rest);
    bank.computeSkinning(rest, rows);
    float maxRestErr = 0.0f;
    for (int b = 0; b < kMaxBones; ++b) {
        Vec4 e0{rows[b * 3 + 0].x - 1, rows[b * 3 + 0].y, rows[b * 3 + 0].z, rows[b * 3 + 0].w};
        Vec4 e1{rows[b * 3 + 1].x, rows[b * 3 + 1].y - 1, rows[b * 3 + 1].z, rows[b * 3 + 1].w};
        Vec4 e2{rows[b * 3 + 2].x, rows[b * 3 + 2].y, rows[b * 3 + 2].z - 1, rows[b * 3 + 2].w};
        for (const Vec4& e : {e0, e1, e2}) {
            maxRestErr = std::max(maxRestErr, std::max(std::max(std::fabs(e.x), std::fabs(e.y)), std::max(std::fabs(e.z), std::fabs(e.w))));
        }
    }
    CHECK(maxRestErr < 1e-3f, "pose de repouso gera matrizes identidade (erro máx %.6f)", maxRestErr);

    // 2. Malhas E0M2 (LOD0, LOD1, LOD2) e pesos válidos
    const char* lodFiles[3] = {"import assets/ybot_ref/ybot.e0m", "import assets/ybot_ref/ybot_lod1.e0m", "import assets/ybot_ref/ybot_lod2.e0m"};
    std::vector<SkinnedVert> lod0;
    for (int l = 0; l < 3; ++l) {
        std::vector<SkinnedVert> verts;
        bool ok = loadE0M2(lodFiles[l], verts);
        bool validW = ok;
        for (const SkinnedVert& v : verts) {
            int sumW = (int)v.w[0] + v.w[1] + v.w[2] + v.w[3];
            if (sumW != 255 || v.b[0] >= kMaxBones || v.b[1] >= kMaxBones || v.b[2] >= kMaxBones || v.b[3] >= kMaxBones) { validW = false; break; }
        }
        CHECK(ok && validW, "%s: %zu vértices E0M2 com pesos válidos (soma=255, ossos<65)", lodFiles[l], verts.size());
        if (l == 0) lod0 = std::move(verts);
    }

    // 3. Avaliação de todos os 10 clipes na malha completa (altura, pés no chão e deformação real)
    for (int c = 0; c < ClipCount; ++c) {
        float dur = bank.clipDuration(c);
        BoneTransform pose[kMaxBones];
        bank.sampleTime(c, dur * 0.5f, pose);
        bank.computeSkinning(pose, rows);
        float minY = 1e9f, maxY = -1e9f, maxDisp = 0.0f;
        for (size_t i = 0; i < lod0.size(); i += 7) {
            Vec3 p = evalVertex(lod0[i], rows);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
            Vec3 d = p - lod0[i].p;
            maxDisp = std::max(maxDisp, std::sqrt(dot(d, d)));
        }
        float h = maxY - minY;
        bool isSwim = (c == ClipSwimIdle || c == ClipSwim);
        bool groundOk = isSwim ? (minY > -0.35f && minY < 1.00f && maxY > 1.30f && maxY < 1.52f)
                      : ((c == ClipJump || c == ClipFall) ? (minY > -0.05f && minY < 0.45f) : (c == ClipDeathFwd || std::fabs(minY) < 0.06f));   // OMBRO fica no ar (no ombro de quem carrega)
        float minH = isSwim ? 0.35f : (c == ClipRescue ? 0.45f : 0.90f);   // RESGATE (RCP) é ajoelhado: ~0,6 m
        CHECK(h > minH && h < (c == ClipArmUp ? 2.40f : 2.15f) && groundOk && maxDisp > 0.15f,
              "clipe %d (%s, %.2fs): altura=%.2fm minY=%.3fm maxY=%.3fm desloc_max=%.2fm",
              c, AnimBank::clipLabel(c), dur, h, minY, maxY, maxDisp);
    }

    // 4. Máquina de estados do AnimPlayer (locomoção, pulo, queda, pouso e ações)
    AnimPlayer player;
    player.reset();
    AnimInput in{};
    for (int i = 0; i < 10; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "PARADO") == 0, "estado inicial parado (%s)", player.activeName());

    in.speed = 3.0f;
    for (int i = 0; i < 20; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "ANDAR") == 0, "velocidade 3 m/s -> ANDAR (%s)", player.activeName());

    in.speed = 6.0f;
    for (int i = 0; i < 20; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "CORRER") == 0, "velocidade 6 m/s -> CORRER (%s)", player.activeName());

    in.grounded = false; in.vy = 6.5f;
    player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "PULAR") == 0, "início do pulo -> PULAR (%s)", player.activeName());

    for (int i = 0; i < 25; ++i) { in.vy -= 18.0f / 60.0f; player.update(1.0f / 60.0f, in, bank); }
    CHECK(std::strcmp(player.activeName(), "CAIR") == 0, "no ar após subida -> CAIR (%s)", player.activeName());

    in.grounded = true; in.vy = 0.0f; in.speed = 0.0f;
    player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "POUSAR") == 0, "toque no chão -> POUSAR (%s)", player.activeName());

    for (int i = 0; i < 60; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "PARADO") == 0, "após pouso volta para PARADO (%s)", player.activeName());

    // Ações por ferramenta (códigos: 1/5/6 = bater, 2 = cortar, 3 = minerar, 4 = coletar). tool: 0 nada, 1 machadinha, 2 picareta.
    //  sem ferramenta, cortar/minerar viram socos; cada ferramenta tem golpes de uma mão próprios.
    struct ActCase { int tool, code; const char* label; };
    const ActCase cases[] = {
        {0, 1, "BATER 1"}, {0, 5, "BATER 2"}, {0, 6, "BATER 3"}, {0, 2, "BATER 2"}, {0, 3, "BATER 3"}, {0, 4, "COLETAR"},
        {1, 1, "MINERAR"}, {1, 5, "CORTAR"}, {1, 6, "MINERAR"}, {1, 2, "CORTAR"}, {1, 3, "MINERAR"}, {1, 4, "COLETAR"},
        {2, 1, "PICARETA"}, {2, 5, "MINERAR"}, {2, 6, "PICARETA"}, {2, 2, "MINERAR"}, {2, 3, "PICARETA"}, {2, 4, "COLETAR"},
    };
    for (const ActCase& c : cases) {
        player.reset();
        in.tool = c.tool;
        in.triggerAction = c.code;
        player.update(1.0f / 60.0f, in, bank);
        in.triggerAction = 0;
        player.update(1.0f / 60.0f, in, bank);
        CHECK(std::strcmp(player.activeName(), c.label) == 0 && player.inAction(),
              "ferramenta %d, ação %d dispara clipe %s (%s)", c.tool, c.code, c.label, player.activeName());
    }
    // Ferramenta: aparece com a mão fechada após a transição e some (guardada) ao nadar
    player.reset(); in.tool = 1; in.triggerAction = 0;
    for (int i = 0; i < 20; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(player.toolVisible(), "machadinha na mão fica visível após 0,33 s");
    in.swimming = true;
    for (int i = 0; i < 20; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(!player.toolVisible(), "ao nadar a ferramenta é guardada (some)");
    in.swimming = false;
    in.tool = 0;

    // 5. Fila de 1 golpe sem cancelar a animação atual
    player.reset();
    in.triggerAction = 1;  // inicia BATER 1
    player.update(0.10f, in, bank);
    in.triggerAction = 0;
    player.update(0.20f, in, bank);  // t = 0.30s de BATER 1 (~0.86s)
    CHECK(std::strcmp(player.activeName(), "BATER 1") == 0 && !player.hasQueuedAction(),
          "BATER 1 em andamento sem fila (%s)", player.activeName());

    in.triggerAction = 5;  // clica novamente (BATER 2) durante BATER 1
    player.update(0.05f, in, bank);
    in.triggerAction = 0;
    CHECK(std::strcmp(player.activeName(), "BATER 1") == 0 && player.hasQueuedAction() &&
          std::strcmp(player.queuedName(), "BATER 2") == 0,
          "novo clique NÃO cancela BATER 1 e enfileira BATER 2 (atual=%s, fila=%s)",
          player.activeName(), player.queuedName() ? player.queuedName() : "-");

    in.triggerAction = 6;  // 3º clique (BATER 3) enquanto já há 1 na fila -> ignorado (máx 1 na fila)
    player.update(0.05f, in, bank);
    in.triggerAction = 0;
    CHECK(std::strcmp(player.activeName(), "BATER 1") == 0 && player.hasQueuedAction() &&
          std::strcmp(player.queuedName(), "BATER 2") == 0,
          "3º clique não sobrescreve nem empilha além de 1 na fila (fila=%s)",
          player.queuedName() ? player.queuedName() : "-");

    player.update(0.55f, in, bank);  // passa do fim de BATER 1 (0.86s) -> entra em BATER 2 automaticamente
    CHECK(std::strcmp(player.activeName(), "BATER 2") == 0 && !player.hasQueuedAction(),
          "ao terminar BATER 1 inicia BATER 2 da fila e esvazia a fila (atual=%s)", player.activeName());

    // 6. Sistema de nado (BOIAR <-> NADAR)
    player.reset();
    in = AnimInput{};
    in.grounded = true;
    in.swimming = true;
    in.speed = 0.0f;
    for (int i = 0; i < 20; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "BOIAR") == 0, "na água parado -> BOIAR (%s)", player.activeName());

    in.speed = 2.6f;
    for (int i = 0; i < 20; ++i) player.update(1.0f / 60.0f, in, bank);
    CHECK(std::strcmp(player.activeName(), "NADAR") == 0, "na água em movimento -> NADAR (%s)", player.activeName());

    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE ANIMACAO PASSARAM\n", fails);
    return fails ? 1 : 0;
}
