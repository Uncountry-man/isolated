// Isolated - malha do terreno e dos objetos do mundo (árvores, pedras, construções).
//  * Terreno em pedaços ("chunks") de 32x32 células (64 m): 15x15 sobre a ilha de 960 m. Chunks 100% sob a água nem são criados.
//  * 3 LODs por chunk (células de 2 m / 4 m / 8 m) com "saias" que escondem rachaduras entre LODs vizinhos.
//  * Cores por bioma nos vértices (sem texturas); normais da grade de alturas; objetos = malhas procedurais coloridas
//    (sombreamento chapado), fundidas por chunk => 1 draw call de terreno + 1 de objetos por chunk visível.
//  * GRAMA: tufos instanciados (7 lâminas, 7 triângulos cada) com balanço de vento, gerados da grade de biomas (visual apenas); só perto da câmera.
//  * Culling por distância e por frustum.
#pragma once
#include <vector>

#include "vecmath.h"
#include "../../shared/world.h"

namespace e0 {

class TerrainRenderer {
public:
    struct Params {
        Mat4 vp;               // para o frustum
        Vec3 cam;
        float viewDist = 380;  // alcance do terreno (m)
        float lod0 = 80, lod1 = 180;  // distâncias de troca de LOD
        float propDist = 170;  // alcance dos objetos (m)
        float grassDist = 50, grassDensity = 1.0f;  // alcance (m) e fração dos tufos desenhados
        float smallDist = 60;                       // alcance dos coletáveis (gravetos, pedrinhas)
    };
    struct Counts { int chunks = 0, tris = 0, propChunks = 0, propTris = 0, culled = 0, grassChunks = 0, grassTufts = 0, smallChunks = 0, smallTris = 0; };

    void build(const e0net::World& w);   // requer contexto GL atual
    void destroy();
    bool empty() const { return chunks_.empty(); }

    // O programa de shader (com layout: 0 = pos float3, 1 = normal byte3 normalizada, 2 = cor ubyte4 normalizada) deve estar ativo.
    void drawTerrain(const Params& p, Counts& c);      // LODs por distância
    void drawTerrainLow(const Params& p, Counts& c);   // sempre LOD2 (reflexo)
    void drawProps(const Params& p, Counts& c);
    void drawSmall(const Params& p, Counts& c);   // gravetos e pedrinhas coletáveis (só perto da câmera)
    void drawGrass(const Params& p, Counts& c);   // programa de grama ativo (instanciado)
    void drawPropsAround(const Vec3& focus, float radius, Counts& c);  // objetos dos chunks perto do foco (passe de sombra)
    // Esconde/mostra um coletável (índice em world.objects) sem reconstruir o chunk: troca só a faixa de vértices dele no buffer.
    void setSmallHidden(uint32_t objIdx, bool hidden);
    // Objetos grandes (árvores, pedras, toras): some de vez (vértices zerados no buffer do chunk). `ghost` recebe uma cópia dos vértices
    // do objeto (20 B cada: x y z, normal i8x4, cor u8x4) para o efeito de sumir; `base` = ponto de apoio (pé do objeto).
    void hideProp(uint32_t objIdx);
    void showProp(uint32_t objIdx);   // desfaz hideProp (recria os vértices do objeto)
    bool propGhost(uint32_t objIdx, std::vector<uint8_t>& ghost, Vec3& base) const;

private:
    struct Chunk {
        bool valid = false;
        unsigned vbo = 0, ibo[3] = {0, 0, 0}, vao[3] = {0, 0, 0};
        int idx[3] = {0, 0, 0};
        unsigned pvbo = 0, pvao = 0;
        int pcount = 0;
        unsigned gvbo = 0, gvao = 0;
        int gcount = 0;
        unsigned svbo = 0, svao = 0;
        int scount = 0;
        std::vector<uint8_t> scpu;   // cópia dos vértices dos coletáveis (p/ reexibir depois de esconder)
        float cx = 0, cy = 0, cz = 0, radius = 0, minH = 0, maxH = 0;
    };
    void drawTerrainImpl(const Params& p, Counts& c, bool forceLow);
    std::vector<Chunk> chunks_;
    struct SmallRef { int chunk = -1; int first = 0, count = 0; };
    std::vector<SmallRef> propRef_;    // por objeto grande: faixa de vértices dele no buffer de props do chunk
    const e0net::World* src_ = nullptr;
    std::vector<SmallRef> smallRef_;   // por objeto do mundo: onde estão os vértices do coletável
    unsigned tuftVbo_ = 0;   // malha de um tufo (compartilhada por todas as instâncias)
};

}  // namespace e0
