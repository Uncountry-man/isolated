// Testes do mundo/terreno (PC). Uso: build/worldtest            -> testes
//                                    build/worldtest --preview SEED out.ppm   -> imagem do mapa (biomas + relevo + objetos)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../shared/protocol.h"
using namespace e0net;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void preview(uint32_t seed, const char* path) {
    World w; if (!World::generate(seed, w)) { std::printf("falha ao gerar\n"); return; }
    const int S = 2, N = World::kVerts * S;
    std::vector<uint8_t> img((size_t)N * N * 3);
    static const uint8_t col[BiomeCount][3] = {{20, 50, 110}, {222, 200, 140}, {115, 84, 56}, {84, 140, 56}, {40, 100, 42}, {128, 128, 132}, {150, 140, 125}};
    for (int iz = 0; iz < World::kVerts; ++iz)
        for (int ix = 0; ix < World::kVerts; ++ix) {
            int hx = w.hcm[(size_t)iz * World::kVerts + std::min(ix + 1, World::kCells)] - w.hcm[(size_t)iz * World::kVerts + std::max(ix - 1, 0)];
            int hz = w.hcm[(size_t)std::min(iz + 1, World::kCells) * World::kVerts + ix] - w.hcm[(size_t)std::max(iz - 1, 0) * World::kVerts + ix];
            float shade = 0.75f + 0.5f * std::fmax(-0.5f, std::fmin(0.5f, (-hx * 0.5f - hz * 0.5f) / 120.0f));
            const uint8_t* c = col[w.biome[(size_t)iz * World::kVerts + ix]];
            int h = w.hcm[(size_t)iz * World::kVerts + ix];
            for (int dy = 0; dy < S; ++dy) for (int dx = 0; dx < S; ++dx) {
                uint8_t* p = &img[((size_t)(iz * S + dy) * N + ix * S + dx) * 3];
                float sh = h < 0 ? 1.0f : shade;
                for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::fmin(255.0f, c[k] * sh);
            }
        }
    for (const WorldObject& o : w.objects) {
        int px = (int)((o.xcm + World::kHalfCm) / 100), pz = (int)((o.zcm + World::kHalfCm) / 100);
        uint8_t c[3] = {0, 0, 0};
        if (o.type == ObjPalm) { c[0] = 10; c[1] = 60; c[2] = 20; } else if (o.type == ObjCoconut) { c[0] = 30; c[1] = 90; c[2] = 30; }
        else if (o.type == ObjRock) { c[0] = 70; c[1] = 70; c[2] = 75; } else if (o.type == ObjLog) { c[0] = 120; c[1] = 70; c[2] = 20; } else if (o.type == ObjStick) { c[0] = 255; c[1] = 160; c[2] = 40; } else if (o.type == ObjPebble) { c[0] = 235; c[1] = 235; c[2] = 235; } else { c[0] = 230; c[1] = 40; c[2] = 40; }
        int r = World::isBuilding(o.type) ? 4 : (o.type == ObjRock || o.type == ObjLog ? 2 : 1);
        for (int dz = -r; dz <= r; ++dz) for (int dx = -r; dx <= r; ++dx) {
            int x = px + dx, z = pz + dz; if (x < 0 || z < 0 || x >= N || z >= N) continue;
            std::memcpy(&img[((size_t)z * N + x) * 3], c, 3);
        }
    }
    int sx = (w.spawnXcm + World::kHalfCm) / 100, sz = (w.spawnZcm + World::kHalfCm) / 100;  // spawn: cruz branca
    for (int d = -8; d <= 8; ++d) { std::memset(&img[((size_t)sz * N + sx + d) * 3], 255, 3); std::memset(&img[((size_t)(sz + d) * N + sx) * 3], 255, 3); }
    FILE* f = std::fopen(path, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", N, N); std::fwrite(img.data(), 1, img.size(), f); std::fclose(f);
    World::Stats s = w.stats();
    std::printf("seed %u: hash %08x  extensao %.0f m  altura max %.1f m  spawn (%.1f, %.1f)\n", seed, w.hash, s.extentM, s.maxH, w.spawnX(), w.spawnZ());
    std::printf("biomas %%: mar %.0f areia %.1f terra %.1f grama %.1f floresta %.1f pedra %.1f piso %.2f | objetos: pinheiro %d carvalho %d pedra %d cabana %d galpao %d torre %d tronco-caido %d | coletaveis: gravetos %d pedrinhas %d (total %zu)\n",
                s.pct[0], s.pct[1], s.pct[2], s.pct[3], s.pct[4], s.pct[5], s.pct[6], s.count[1], s.count[2], s.count[3], s.count[4], s.count[5], s.count[6], s.count[7], s.count[8], s.count[9], w.objects.size());
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--scan") == 0) {  // procura sementes com todos os recursos
        for (uint32_t sd = 1; sd < 400; ++sd) {
            World w; if (!World::generate(sd, w)) continue;
            World::Stats st = w.stats();
            if ((w.features & 31u) != 31u) continue;
            std::printf("semente %u: ilha %.0f m grama %.0f%% floresta %.0f%% areia %.1f%% terra %.1f%% pedra %.1f%% | arvores %d pedras %d construcoes %d troncos %d | pico %.0f m\n", sd, st.extentM, st.pct[3], st.pct[4], st.pct[1], st.pct[2], st.pct[5], st.count[1] + st.count[2], st.count[3], st.count[4] + st.count[5] + st.count[6], st.count[7], st.maxH);
        }
        return 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--preview") == 0) { preview((uint32_t)std::strtoul(argv[2], nullptr, 10), argv[3]); return 0; }

    // 1) determinismo e tempo
    World a, b;
    double t0 = nowMs();
    bool okA = World::generate(1337, a);
    double ms = nowMs() - t0;
    World::generate(1337, b);
    CHECK(okA && a.hash == b.hash && a.hcm == b.hcm && a.biome == b.biome && a.objects.size() == b.objects.size(), "mesma semente => mundo idêntico (hash %08x)", a.hash);
    World c; World::generate(1338, c);
    CHECK(c.hash != a.hash, "semente diferente => mundo diferente (%08x x %08x)", c.hash, a.hash);
    std::printf("   geracao: %.0f ms (PC)\n", ms);
    std::printf("   hash da semente 1337 = %08x  (o app no celular loga o mesmo valor: prova de determinismo entre CPUs)\n", a.hash);

    // 2) salvar / carregar / corromper
    std::string pathStr = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : ".") + "/e0_worldtest.e0w";
    const char* path = pathStr.c_str();
    CHECK(a.save(path), "salvar arquivo");
    World d;
    CHECK(World::load(path, d) && d.hash == a.hash && d.hcm == a.hcm && d.objects.size() == a.objects.size() && d.spawnXcm == a.spawnXcm, "carregar => idêntico ao gerado");
    std::vector<uint8_t> bytes = a.toBytes();
    std::printf("   tamanho do arquivo: %.0f KB\n", bytes.size() / 1024.0);
    bytes[5000] ^= 0x01;
    World e;
    CHECK(!World::fromBytes(bytes.data(), bytes.size(), e), "arquivo corrompido (1 bit) é rejeitado pelo CRC");
    CHECK(!World::fromBytes(bytes.data(), 100, e), "arquivo truncado é rejeitado");

    // 3) limites: ilha <= 1 km, tem terra, spawn válido, vários mundos
    int withAll = 0, withLogs = 0; uint32_t seen = 0, absent = 0; double maxExt = 0; bool allValid = true; double maxMs = 0;
    const int kSeeds = 40;
    for (uint32_t s = 1; s <= (uint32_t)kSeeds; ++s) {
        World w; double q = nowMs(); bool ok = World::generate(s * 7919u + 13u, w); maxMs = std::fmax(maxMs, nowMs() - q);
        if (!ok) { allValid = false; std::printf("   semente %u falhou\n", s); continue; }
        World::Stats st = w.stats();
        maxExt = std::fmax(maxExt, st.extentM);
        float sh = w.heightAt(w.spawnX(), w.spawnZ());
        if (st.landVerts < 8000 || sh < 1.0f || sh > 30.0f || st.pct[BiomeGrass] < 0.5) allValid = false;
        seen |= w.features; absent |= ~w.features;
        if ((w.features & 31u) == 31u) ++withAll;
        if (st.count[7] > 0) ++withLogs;
    }
    CHECK(allValid, "%d sementes: todas geram ilha com terra, grama e spawn seco", kSeeds);
    CHECK(maxExt <= 500.0, "ilha cabe em meio km (maior extensão %.0f m)", maxExt);
    uint32_t varied = seen & absent & 31u;  // recurso que existe em alguma ilha e falta em outra
    CHECK(varied == 31u, "cada recurso (areia, terra, floresta, pedras, construções) aparece em algumas ilhas e falta em outras (máscara %02x)", varied);
    CHECK(withAll < kSeeds, "nem toda ilha tem todos os recursos (%d de %d têm tudo)", withAll, kSeeds);
    CHECK(withLogs > 5 && withLogs < kSeeds, "árvores caídas (troncos): presentes em %d de %d ilhas", withLogs, kSeeds);
    std::printf("   pior tempo de geracao: %.0f ms\n", maxMs);

    // 4) consultas de altura
    bool exact = true;
    for (int i = 0; i < 200; ++i) {
        int ix = (i * 37) % World::kVerts, iz = (i * 91) % World::kVerts;
        float x = ix * World::kCell - World::kHalf, z = iz * World::kCell - World::kHalf;
        if (std::fabs(a.heightAt(x, z) - a.vertexHeight(ix, iz)) > 1e-4f) exact = false;
    }
    CHECK(exact, "heightAt coincide com a grade nos vértices");
    float maxJump = 0;
    for (float x = -100; x < 100; x += 0.1f) maxJump = std::fmax(maxJump, std::fabs(a.heightAt(x + 0.1f, 33.3f) - a.heightAt(x, 33.3f)));
    CHECK(maxJump < 0.6f, "altura contínua (maior salto em 10 cm: %.2f m)", maxJump);
    CHECK(a.heightAt(a.spawnX(), a.spawnZ()) > 1.0f && a.heightAt(-(World::kHalf - 10), -(World::kHalf - 10)) < -5.0f, "spawn em terra firme (%.1f m) e borda do mundo no fundo do mar (%.1f m)", a.heightAt(a.spawnX(), a.spawnZ()), a.heightAt(-(World::kHalf - 10), -(World::kHalf - 10)));

    // 5) colisão com objetos
    int trees = 0, boxes = 0, logs = 0;
    bool pushedOk = true;
    for (const WorldObject& o : a.objects) {
        World::Collider cl = World::colliderOf(o); bool box = cl.kind == 1; float p = cl.a, q = cl.b;
        if (cl.kind == 3) continue;
        bool crowded = false;  // vizinho a < 4 m: o jogador pode ficar entre dois (legítimo); testa só objetos isolados
        for (const WorldObject& n : a.objects) { if (&n == &o) continue; float ddx = (n.xcm - o.xcm) * 0.01f, ddz = (n.zcm - o.zcm) * 0.01f; if (ddx * ddx + ddz * ddz < 16.0f) { crowded = true; break; } }
        if (crowded) continue;
        if (box) { if (++boxes > 6) continue; } else if (cl.kind == 2) { if (++logs > 20) continue; } else { if (++trees > 40) continue; }
        float x = o.xcm * 0.01f + 0.05f, z = o.zcm * 0.01f - 0.03f, r = 0.35f;  // quase no centro => deve sair
        a.pushOut(x, z, r);
        float dx = x - o.xcm * 0.01f, dz = z - o.zcm * 0.01f;
        bool outside;
        if (box) outside = std::fabs(dx) >= p + r - 0.01f || std::fabs(dz) >= q + r - 0.01f;
        else if (cl.kind == 2) { float t = std::fmax(-q, std::fmin(q, dx * cl.dx + dz * cl.dz)); float ex = dx - cl.dx * t, ez = dz - cl.dz * t; outside = std::sqrt(ex * ex + ez * ez) >= p + r - 0.01f; }
        else outside = std::sqrt(dx * dx + dz * dz) >= p + r - 0.01f;
        if (!outside) pushedOk = false;
    }
    CHECK(pushedOk, "pushOut tira o jogador de dentro de árvores, pedras e construções");
    {
        float x0 = a.spawnX(), z0 = a.spawnZ(), x = x0, z = z0; a.pushOut(x, z, 0.35f);
        CHECK(std::fabs(x - x0) < 1e-4f && std::fabs(z - z0) < 1e-4f, "o spawn está livre de obstáculos");
    }

    // 5b) COLETÁVEIS (gravetos e pedrinhas): gerados, em lugar seco, sem colisão, sem estar dentro de obstáculos; mundos v2 são atualizados
    {
        int sticks = 0, pebbles = 0, cloths = 0, clothOffBeach = 0, inWater = 0, blocking = 0, overlap = 0, nearSpawn = 0;
        for (const WorldObject& o : a.objects) {
            if (!World::isCollectible(o.type)) continue;
            (o.type == ObjStick ? sticks : (o.type == ObjCloth ? cloths : pebbles))++;
            if (o.type == ObjCloth && (a.biomeAt(o.xcm * 0.01f, o.zcm * 0.01f) != BiomeSand || o.ycm > 140)) ++clothOffBeach;   // tecido só na praia
            if (o.ycm < 20) ++inWater;
            float x = o.xcm * 0.01f, z = o.zcm * 0.01f, px = x, pz = z;
            a.pushOut(px, pz, kPlayerRadius);
            if (std::fabs(px - x) + std::fabs(pz - z) > 1e-4f) ++blocking;        // o coletável não pode empurrar o jogador
            for (const WorldObject& q : a.objects) {                                // nem estar dentro de árvore/pedra/tronco/construção
                if (World::isCollectible(q.type)) continue;
                World::Collider c = World::colliderOf(q);
                float dx = x - q.xcm * 0.01f, dz = z - q.zcm * 0.01f;
                if (std::fabs(dx) > 8 || std::fabs(dz) > 8) continue;
                bool inside;
                if (c.kind == 1) inside = std::fabs(dx) < c.a && std::fabs(dz) < c.b;
                else if (c.kind == 2) { float t = std::fmax(-c.b, std::fmin(c.b, dx * c.dx + dz * c.dz)); float ex = dx - c.dx * t, ez = dz - c.dz * t; inside = std::sqrt(ex * ex + ez * ez) < c.a; }
                else inside = std::sqrt(dx * dx + dz * dz) < c.a;
                if (inside) { ++overlap; break; }
            }
            float sx = x - a.spawnX(), sz = z - a.spawnZ(); if (sx * sx + sz * sz < 25.0f * 25.0f) ++nearSpawn;
        }
        CHECK(sticks > 40 && pebbles > 40, "coletáveis gerados: %d gravetos e %d pedrinhas (%d perto do spawn, a menos de 25 m)", sticks, pebbles, nearSpawn);
        CHECK(clothOffBeach == 0 && (cloths > 0 || !(a.features & FeatSand)), "tecidos (%d) só na praia: fora da areia = %d (ilha %s areia)", cloths, clothOffBeach, (a.features & FeatSand) ? "com" : "sem");
        CHECK(inWater == 0 && blocking == 0 && overlap == 0, "coletáveis em terra seca, sem colisão e fora de obstáculos (água %d, bloqueando %d, sobrepostos %d)", inWater, blocking, overlap);
        // atualização de mundo antigo: remove coletáveis, grava como v2 e recarrega => igual ao mundo novo, bit a bit
        World old = a;
        old.objects.erase(std::remove_if(old.objects.begin(), old.objects.end(), [](const WorldObject& o) { return World::isCollectible(o.type); }), old.objects.end());
        std::vector<uint8_t> v2 = old.toBytes();
        uint32_t two = 2; std::memcpy(v2.data() + 4, &two, 4);                      // cabeçalho: versão 2
        uint32_t crc = crc32(v2.data(), v2.size() - 4); std::memcpy(v2.data() + v2.size() - 4, &crc, 4);
        World up;
        bool okUp = World::fromBytes(v2.data(), v2.size(), up);
        CHECK(okUp && up.upgraded && up.hash == a.hash && up.objects.size() == a.objects.size(), "mundo salvo v2 é ATUALIZADO ao carregar e fica idêntico ao gerado (hash %08x == %08x, %zu objetos)", up.hash, a.hash, up.objects.size());
        {   // mundo salvo v3 (sem tecidos): ATUALIZADO ao carregar, idêntico ao gerado
            World o3 = a;
            o3.objects.erase(std::remove_if(o3.objects.begin(), o3.objects.end(), [](const WorldObject& o) { return o.type == ObjCloth; }), o3.objects.end());
            std::vector<uint8_t> v3 = o3.toBytes();
            uint32_t three = 3; std::memcpy(v3.data() + 4, &three, 4);
            uint32_t crc3 = crc32(v3.data(), v3.size() - 4); std::memcpy(v3.data() + v3.size() - 4, &crc3, 4);
            World up3; bool ok3 = World::fromBytes(v3.data(), v3.size(), up3);
            CHECK(ok3 && up3.upgraded && up3.hash == a.hash && up3.objects.size() == a.objects.size(), "mundo salvo v3 é ATUALIZADO (tecidos) e fica idêntico ao gerado (hash %08x == %08x)", up3.hash, a.hash);
        }
        CHECK(a.hash == [&] { World g; World::generate(a.seed, g); return g.hash; }(), "geração com coletáveis é determinística");
    }

    // 6) FÍSICA sobre o terreno (a mesma stepPlayer do servidor e do cliente)
    {
        setTerrain(&a);
        auto runWalk = [&](float yaw, bool run, float seconds, PlayerState& p, double& sinkViolations, double& groundViolations, double& objViolations, int& swimSteps) {
            for (int i = 0; i < (int)(seconds * 30); ++i) {
                InputCmd c; c.moveX = 0; c.moveY = 1; c.camYaw = yaw; c.dt = 1.0f / 30; c.flags = run ? InputRun : 0;
                stepPlayer(p, c);
                float g = a.heightAt(p.x, p.z);
                float sup = supportHeightAt(p.x, p.z);
                if (p.y < World::kSwimFloorH - 0.001f) ++sinkViolations;  // nunca afunda abaixo do nível de nado (-1.20 m)
                if (p.y < sup - 0.001f || (g >= World::kSwimFloorH && p.y < g - 0.001f)) ++groundViolations;
                if (isSwimmingPos(p.x, p.y, p.z)) ++swimSteps;
                int near = 0;
                for (const WorldObject& o : a.objects) {
                    float dx = p.x - o.xcm * 0.01f, dz = p.z - o.zcm * 0.01f;
                    if (std::fabs(dx) > 10 || std::fabs(dz) > 10) continue;
                    World::Collider cl = World::colliderOf(o);
                    if (cl.kind == 3) continue;
                    bool inside;
                    if (cl.kind == 1) inside = std::fabs(dx) < cl.a - 0.05f && std::fabs(dz) < cl.b - 0.05f;
                    else if (cl.kind == 2) { float t = std::fmax(-cl.b, std::fmin(cl.b, dx * cl.dx + dz * cl.dz)); float ex = dx - cl.dx * t, ez = dz - cl.dz * t; inside = std::sqrt(ex * ex + ez * ez) < cl.a + kPlayerRadius - 0.08f; }
                    else inside = std::sqrt(dx * dx + dz * dz) < cl.a + kPlayerRadius - 0.08f;
                    if (inside) ++near;
                }
                if (near) ++objViolations;
            }
        };
        double sink = 0, ground = 0, objs = 0;
        int swimSteps = 0;
        float minH = 1e9f, maxH = -1e9f;
        for (int k = 0; k < 16; ++k) {   // 16 direções, andando/nadando por 150 s cada, a partir do spawn
            PlayerState p; p.x = a.spawnX(); p.z = a.spawnZ(); p.y = a.heightAt(p.x, p.z);
            runWalk(k * 0.3927f, (k & 1) != 0, 150.0f, p, sink, ground, objs, swimSteps);
            minH = std::fmin(minH, a.heightAt(p.x, p.z)); maxH = std::fmax(maxH, a.heightAt(p.x, p.z));
        }
        CHECK(swimSteps > 500 && minH < -5.0f && sink == 0, "mar explorável (sem barreira invisível): nada na superfície y=%.2f m sem afundar (%d passos nadando, fundo mín %.1f m)", World::kSwimFloorH, swimSteps, minH);
        CHECK(ground == 0, "o jogador nunca fica abaixo do chão ou da flutuação (%.0f violações)", ground);
        CHECK(objs == 0, "o jogador nunca atravessa árvores, pedras ou construções (%.0f violações)", objs);

        // determinismo da física: mesma entrada => mesmo estado, bit a bit
        PlayerState p1, p2; p1.x = p2.x = a.spawnX(); p1.z = p2.z = a.spawnZ(); p1.y = p2.y = a.heightAt(p1.x, p1.z);
        uint32_t rng = 12345;
        for (int i = 0; i < 6000; ++i) {
            rng = rng * 1664525u + 1013904223u;
            InputCmd c; c.moveX = ((rng >> 8) & 0xFF) / 127.5f - 1.0f; c.moveY = ((rng >> 16) & 0xFF) / 127.5f - 1.0f;
            c.camYaw = ((rng >> 24) & 0xFF) * 0.0245f; c.dt = 1.0f / 30; c.flags = (uint8_t)((rng & 1 ? InputRun : 0) | ((rng >> 5) % 40 == 0 ? InputJump : 0));
            stepPlayer(p1, c); stepPlayer(p2, c);
        }
        CHECK(std::memcmp(&p1, &p2, sizeof p1) == 0, "física determinística: 6000 passos aleatórios, estado idêntico (%.2f %.2f %.2f)", p1.x, p1.y, p1.z);

        // pulo no relevo
        PlayerState j; j.x = a.spawnX(); j.z = a.spawnZ(); j.y = a.heightAt(j.x, j.z);
        float base = j.y, peak = 0;
        InputCmd jc; jc.dt = 1.0f / 30; jc.flags = InputJump; stepPlayer(j, jc); jc.flags = 0;
        for (int i = 0; i < 40; ++i) { stepPlayer(j, jc); peak = std::fmax(peak, j.y - base); }
        CHECK(peak > 0.9f && peak < 1.3f && std::fabs(j.y - base) < 0.001f, "pulo sobre o relevo: pico %.2f m e volta ao chão", peak);

        // subir barranco íngreme é bloqueado (penhasco/pedra); descer é livre
        bool blocked = false;
        for (int iz = 20; iz < World::kCells - 20 && !blocked; iz += 3)
            for (int ix = 20; ix < World::kCells - 20 && !blocked; ix += 3) {
                float x = ix * 2.0f - World::kHalf, z = iz * 2.0f - World::kHalf, hh = a.heightAt(x, z), hr = a.heightAt(x + 2.0f, z);
                if (hh > 1.0f && hr - hh > 2.6f) {   // degrau de >2.6 m em 2 m (~52°): não pode subir
                    PlayerState q; q.x = x; q.z = z; q.y = hh;
                    for (int i = 0; i < 20; ++i) { InputCmd c; c.moveY = 1; c.camYaw = 1.5708f; c.dt = 1.0f / 30; stepPlayer(q, c); }   // yaw 90° = +X
                    if (q.x < x + 1.9f) blocked = true;
                }
            }
        CHECK(blocked, "barranco íngreme (> 49°) bloqueia a subida");
        setTerrain(nullptr);
    }

    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DO MUNDO PASSARAM\n", fails);
    return fails ? 1 : 0;
}
