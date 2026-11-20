// Pré-visualização do mapa no PC: gera mini e mapa grande para PPM. Uso: build/maptest [x z] -> /tmp-like ./map_mini.ppm ./map_full.ppm
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "../engine/src/map_gen.h"
#include "../shared/protocol.h"
using namespace e0;
using namespace e0net;
static void save(const char* path, const std::vector<uint8_t>& px, int res) {
    FILE* f = std::fopen(path, "wb"); std::fprintf(f, "P6\n%d %d\n255\n", res, res);
    for (int i = 0; i < res * res; ++i) { uint8_t c[3] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2]}; if (px[i * 4 + 3] == 0) c[0] = c[1] = c[2] = 40; std::fwrite(c, 1, 3, f); }
    std::fclose(f);
}
int main(int argc, char** argv) {
    float cx = argc > 2 ? (float)std::atof(argv[1]) : 0.0f, cz = argc > 2 ? (float)std::atof(argv[2]) : 100.0f;
    static World mainW; World::generate(kDefaultWorldSeed, mainW); setTerrain(&mainW);
    std::vector<std::unique_ptr<World>> isl(kIslandSlots);
    for (int sl = 1; sl < kIslandSlots; ++sl) { int sx, sz; sectorOfSlot(sl, sx, sz); IslandSpec sp = islandSpec(kDefaultWorldSeed, sx, sz); if (!sp.exists) continue;
        auto w = std::make_unique<World>(); if (World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, *w)) { terrain().setIsland(sl, w.get()); isl[sl] = std::move(w); } }
    std::vector<MapMark> marks = {{cx, cz, 0.6f, 0}, {cx + 60, cz + 120, 1.2f, 1}, {cx - 90, cz - 40, 0, 2}};
    std::vector<uint8_t> px;
    MapView mini; mini.cx = cx; mini.cz = cz; mini.half = 330; mini.viewRadius = 310; mini.res = 192; mini.circle = true;
    renderMapTexture(mini, marks, px); save("map_mini.ppm", px, mini.res);
    MapView big; big.cx = cx; big.cz = cz; big.half = 1150; big.viewRadius = 1050; big.res = 512; big.folds = true;
    renderMapTexture(big, marks, px); save("map_full.ppm", px, big.res);
    std::printf("ok\n");
}
