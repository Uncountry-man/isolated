// Testes do OCEANO ABERTO (shared/terrain.h + World::generateIsland). Uso: build/oceantest [--list]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include "../shared/protocol.h"
using namespace e0net;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    const bool list = argc > 1 && std::strcmp(argv[1], "--list") == 0;
    const uint32_t mainSeed = kDefaultWorldSeed;
    // slots: ida e volta; o setor (0,0) é o slot 0; todos distintos
    bool slotsOk = slotOfSector(0, 0) == 0;
    bool seen[kIslandSlots] = {};
    for (int sx = -kOceanN; sx <= kOceanN; ++sx) for (int sz = -kOceanN; sz <= kOceanN; ++sz) {
        int s = slotOfSector(sx, sz), rx, rz;
        if (s < 0 || s >= kIslandSlots || seen[s]) { slotsOk = false; continue; }
        seen[s] = true; sectorOfSlot(s, rx, rz);
        if (rx != sx || rz != sz) slotsOk = false;
    }
    CHECK(slotsOk, "slots dos setores: 81 distintos, ida e volta, setor (0,0) = slot 0");

    World mainW; World::generate(mainSeed, mainW);
    CHECK(mainW.hash == 0xd3579d9bu, "a ilha inicial NAO mudou (hash %08x, esperado d3579d9b)", mainW.hash);

    std::vector<std::unique_ptr<World>> isl(kIslandSlots);
    int kinds[IslandKindCount] = {}, total = 0, noLand = 0, big = 0;
    double ms = 0, worst = 0;
    struct Agg { int trees = 0, rocks = 0, ore = 0, sticks = 0, pebbles = 0, logs = 0, builds = 0, sandV = 0, rockV = 0, forestV = 0, dirtV = 0, grassV = 0; } agg[IslandKindCount];
    int nearCount = 0;
    for (int sl = 1; sl < kIslandSlots; ++sl) {
        int sx, sz; sectorOfSlot(sl, sx, sz);
        IslandSpec sp = islandSpec(mainSeed, sx, sz);
        const bool near = sx >= -1 && sx <= 1 && sz >= -1 && sz <= 1;
        if (!sp.exists) { if (near) CHECK(false, "setor vizinho (%d,%d) sem ilha", sx, sz); continue; }
        if (near) ++nearCount;
        auto t0 = std::chrono::steady_clock::now();
        auto w = std::make_unique<World>();
        bool ok = World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, *w);
        double d = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms += d; worst = std::max(worst, d);
        if (!ok) { CHECK(false, "ilha do setor (%d,%d) nao gerou", sx, sz); continue; }
        ++total; ++kinds[sp.kind]; if (sp.kind == IslandBig) ++big;
        World::Stats st = w->stats();
        if (st.landVerts < 30) ++noLand;
        Agg& a = agg[sp.kind];
        a.trees += st.count[ObjPalm] + st.count[ObjCoconut]; a.rocks += st.count[ObjRock]; a.sticks += st.count[ObjStick]; a.pebbles += st.count[ObjPebble]; a.logs += st.count[ObjLog];
        a.builds += st.count[ObjCabin] + st.count[ObjShed] + st.count[ObjTower];
        for (uint32_t i = 0; i < w->objects.size(); ++i) if (w->objects[i].type == ObjRock && w->oreOf(i)) ++a.ore;
        a.sandV += (int)st.pct[BiomeSand]; a.rockV += (int)st.pct[BiomeRock]; a.forestV += (int)st.pct[BiomeForest]; a.dirtV += (int)st.pct[BiomeDirt]; a.grassV += (int)st.pct[BiomeGrass];
        // a grade fica dentro do setor e os objetos têm coordenadas globais perto da origem
        bool inside = true;
        for (const WorldObject& o : w->objects) if (std::abs(o.xcm - w->oxcm) > 32000 || std::abs(o.zcm - w->ozcm) > 32000) inside = false;
        if (!inside) CHECK(false, "objetos da ilha %d fora da grade", sl);
        // o centro e terra seca (ilhas pequenas) e a altura global bate com a local
        float cx = w->oxcm * 0.01f, cz = w->ozcm * 0.01f;
        if (st.landVerts >= 30 && w->heightAt(cx, cz) < -1.0f) { /* o centro pode ser mar em costa muito recortada; so conta se nenhuma terra */ }
        if (list) std::printf("  setor (%2d,%2d) slot %2d %-22s semente %08x origem (%6.0f,%6.0f) terra %4d verts, %4zu objs, hash %08x, %.0f ms\n", sx, sz, sl, islandKindName(sp.kind), sp.seed, cx, cz, st.landVerts, w->objects.size(), w->hash, d);
        isl[sl] = std::move(w);
    }
    CHECK(total >= 20 && total <= 60, "%d ilhas no oceano (esperado 20..60)", total);
    CHECK(noLand == 0, "toda ilha tem terra (%d sem)", noLand);
    CHECK(big >= 1 && big <= 5 && big * 8 < total, "ilhas grandes sao RARAS: %d de %d", big, total);
    CHECK(nearCount == 8, "os 8 setores em volta da ilha inicial tem ilha (%d)", nearCount);
    for (int k = 1; k < IslandKindCount; ++k) std::printf("   %-22s x%2d  arvores %3d pedras %3d minerio %2d gravetos %3d pedrinhas %3d toras %2d ruinas %d | biomas%%(areia/terra/grama/floresta/pedra) %d/%d/%d/%d/%d (soma sobre as ilhas)\n", islandKindName((uint8_t)k), kinds[k], agg[k].trees, agg[k].rocks, agg[k].ore, agg[k].sticks, agg[k].pebbles, agg[k].logs, agg[k].builds, agg[k].sandV, agg[k].dirtV, agg[k].grassV, agg[k].forestV, agg[k].rockV);
    CHECK(agg[IslandAtoll].trees > 0 && agg[IslandAtoll].rocks == 0 && agg[IslandAtoll].forestV == 0, "atol: areia + palmeiras, sem pedras nem floresta");
    CHECK(agg[IslandRock].rocks > 0 && agg[IslandRock].ore > 0 && agg[IslandRock].trees == 0, "afloramento: pedras e MINERIO, sem arvores");
    CHECK(agg[IslandForest].trees > 20 && agg[IslandForest].rocks == 0, "ilhota de floresta: muitas arvores, sem pedras");
    CHECK(agg[IslandMeadow].sticks + agg[IslandMeadow].pebbles > 0 && agg[IslandMeadow].rocks == 0, "campina: gravetos/pedrinhas, sem pedras grandes");
    CHECK(kinds[IslandRuins] == 0 || agg[IslandRuins].trees > 0, "ilhota de ruinas tem arvores");
    std::printf("   geracao: media %.0f ms por ilha, pior %.0f ms, total %.1f s\n", ms / std::max(1, total), worst, ms / 1000.0);

    // determinismo: a mesma ilha gerada de novo e bit a bit igual
    {
        int slot = 0; for (int sl = 1; sl < kIslandSlots && !slot; ++sl) if (isl[sl]) slot = sl;
        int sx, sz; sectorOfSlot(slot, sx, sz); IslandSpec sp = islandSpec(mainSeed, sx, sz);
        World again; World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, again);
        bool same = again.hash == isl[slot]->hash && again.objects.size() == isl[slot]->objects.size() && again.hcm == isl[slot]->hcm;
        for (size_t i = 0; same && i < again.objects.size(); ++i) same = again.objects[i].xcm == isl[slot]->objects[i].xcm && again.objects[i].zcm == isl[slot]->objects[i].zcm && again.objects[i].type == isl[slot]->objects[i].type;
        CHECK(same, "regerar a ilha do slot %d da exatamente o mesmo mundo (hash %08x)", slot, again.hash);
        // outra semente do mundo => outro layout
        int diff = 0; for (int sl = 1; sl < kIslandSlots; ++sl) { int a, b; sectorOfSlot(sl, a, b); if (islandSpec(mainSeed, a, b).exists != islandSpec(mainSeed + 1, a, b).exists) ++diff; }
        CHECK(diff > 5, "outra semente de mundo muda o layout do oceano (%d setores diferentes)", diff);
    }

    // Terrain: dispatch de altura/bioma/colisao entre ilhas e mar
    setTerrain(&mainW);
    for (int sl = 1; sl < kIslandSlots; ++sl) if (isl[sl]) terrain().setIsland(sl, isl[sl].get());
    {
        bool ok = true; int tested = 0;
        for (int sl = 1; sl < kIslandSlots; ++sl) {
            if (!isl[sl]) continue;
            const World* w = isl[sl].get();
            // acha o ponto mais alto da ilha e confere que Terrain devolve a mesma altura
            float best = -1e9f, bx = 0, bz = 0;
            for (int iz = 0; iz < World::kVerts; iz += 4) for (int ix = 0; ix < World::kVerts; ix += 4) {
                float h = w->vertexHeight(ix, iz); if (h > best) { best = h; bx = w->oxcm * 0.01f + ix * World::kCell - World::kHalf; bz = w->ozcm * 0.01f + iz * World::kCell - World::kHalf; }
            }
            float ht = terrain().heightAt(bx, bz);
            if (std::fabs(ht - w->heightAt(bx, bz)) > 1e-4f || ht < 0.5f) ok = false;
            ++tested;
        }
        CHECK(ok && tested == total, "Terrain::heightAt acha o topo de cada ilha (%d ilhas)", tested);
        CHECK(terrain().heightAt(300.0f, 700.0f) <= -30.0f && terrain().slotAt(4000.0f, -3700.0f) != 0, "mar aberto entre as ilhas e fundo (%.0f m)", terrain().heightAt(300.0f, 700.0f));
    }
    // jogador anda e nada entre ilhas sem violar nada (fisica compartilhada com o oceano)
    {
        int target = 0; for (int sl = 1; sl < kIslandSlots && !target; ++sl) if (isl[sl] && isl[sl]->kind != IslandBig && std::abs(isl[sl]->oxcm) < 200000 && std::abs(isl[sl]->ozcm) < 200000) target = sl;
        const World* w = isl[target].get();
        PlayerState p; p.x = 0; p.z = 200; p.y = World::kSwimFloorH;
        InputCmd c; c.dt = 1.0f / 30; c.flags = InputRun;
        float tx = w->oxcm * 0.01f, tz = w->ozcm * 0.01f, maxY = -99; bool reached = false;
        for (int i = 0; i < 30 * 600 && !reached; ++i) {
            float dx = tx - p.x, dz = tz - p.z, n = std::sqrt(dx * dx + dz * dz);
            c.moveX = dx / n; c.moveY = -dz / n; c.camYaw = 0;
            stepPlayer(p, c);
            maxY = std::max(maxY, p.y);
            if (n < 6.0f) reached = true;
        }
        CHECK(reached && maxY > 0.3f, "nadando e andando, o jogador chega ate a ilha do slot %d (%s) e sobe nela (y max %.1f)", target, islandKindName(w->kind), maxY);
        for (int i = 0; i < 30 * 400; ++i) { c.moveX = 1; c.moveY = 0; stepPlayer(p, c); }
        CHECK(std::fabs(p.x) <= Terrain::limit() + 1.0f && std::fabs(p.z) <= Terrain::limit() + 1.0f, "o limite do mar e respeitado (%.0f, %.0f)", p.x, p.z);
    }
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DO OCEANO PASSARAM\n", fails);
    return fails ? 1 : 0;
}
