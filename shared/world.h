// Isolated - MUNDO / TERRENO (compartilhado entre servidor, cliente e testes; C++17 puro, sem Android).
//
//  * Uma ILHA de no máximo 500 m (mundo de 320x320 células de 2 m = 640 m, com mar em volta). O NÍVEL DO MAR É y = 0.
//  * Gerada a partir de uma SEMENTE só com aritmética INTEIRA (ruído de valor em ponto fixo Q16): o resultado é
//    bit a bit igual em qualquer CPU/compilador (PC x86, celular ARM). O hash (CRC32) do conteúdo prova isso.
//  * Cada semente sorteia uma "receita": areia, terra, floresta, pedras e construções são OPCIONAIS — uma ilha pode
//    não ter todos. Grama sempre existe.
//  * O mundo é SALVO em arquivo (.e0w) com CRC. O servidor carrega o arquivo se existir (mudar o gerador depois NÃO
//    altera um mundo já salvo); os clientes baixam do servidor e guardam em cache (ver world_sync.h).
//  * Sem texturas: o terreno tem cores por seção (bioma) e os objetos são malhas procedurais coloridas.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace e0net {

enum Biome : uint8_t {
    BiomeSeaFloor = 0,  // fundo do mar (sob a água)
    BiomeSand,          // areia
    BiomeDirt,          // terra
    BiomeGrass,         // grama
    BiomeForest,        // chão de floresta (onde nascem árvores)
    BiomeRock,          // pedra / penhasco
    BiomePad,           // piso das construções
    BiomeCount
};

enum ObjType : uint8_t {
    ObjPalm = 1,   // palmeira
    ObjCoconut = 2,    // coqueiro (tronco mais inclinado, com cocos)
    ObjRock = 3,   // pedra
    ObjCabin = 4,  // cabana
    ObjShed = 5,   // galpão
    ObjTower = 6,  // torre de vigia
    ObjLog = 7,    // árvore caída (tronco deitado; colisão em cápsula)
    ObjStick = 8,  // graveto coletável (SÓ GERADO: sem sistema de coleta ainda; sem colisão). variant: bits 0-1 forma, bit 2 = tom claro
    ObjPebble = 9, // pedra coletável (pedrinha; idem)
    ObjCloth = 10, // TECIDO coletável: lixo trazido pelo mar, SÓ na praia (areia perto da água); variant bits 0-1 = cor; sem colisão
};

// Tipos de ilha do OCEANO ABERTO (ver terrain.h): as pequenas têm 1 ou poucos biomas e recursos específicos; IslandBig = ilha grande completa (rara), igual à inicial.
enum IslandKind : uint8_t {
    IslandBig = 0,      // ilha grande (receita completa de makeRecipe): rara
    IslandAtoll,        // atol: areia + palmeiras/coqueiros (cocos!)
    IslandPalm,         // ilhota de praia: areia, grama e palmeiras
    IslandForest,       // ilhota de floresta: árvores e toras
    IslandRock,         // afloramento de pedra: pedras, pedrinhas e MINÉRIO (ouro/diamante)
    IslandMeadow,       // campina: grama, terra, gravetos e pedrinhas
    IslandRuins,        // ilhota com ruínas e pedras
    IslandKindCount
};

enum WorldFeature : uint32_t {  // recursos que esta ilha tem (sorteados pela semente)
    FeatSand = 1, FeatDirt = 2, FeatForest = 4, FeatRocks = 8, FeatBuildings = 16, FeatCliffCoast = 32,
};

struct WorldObject {
    uint8_t type = 0, variant = 0;
    uint8_t rot = 0;          // yaw = rot * 2π/256 (construções só em múltiplos de 64 = 90°; troncos usam rot>>2 em 64 direções)
    uint16_t scale100 = 100;  // escala em %
    int32_t xcm = 0, zcm = 0; // posição em centímetros (inteiros: idêntico em qualquer máquina)
    int16_t ycm = 0;
};

class World {
public:
    static constexpr int kCells = 320;
    static constexpr int kVerts = kCells + 1;
    static constexpr float kCell = 2.0f;                     // metros por célula
    static constexpr float kHalf = kCells * kCell * 0.5f;    // 320 m: o mundo vai de -320 a +320
    static constexpr int kHalfCm = kCells * 100;             // o mesmo em centímetros
    static constexpr uint32_t kGenVersion = 4;               // 4 = + tecidos na praia; 3 = + gravetos e pedras coletáveis (mundos v2 e v3 são ATUALIZADOS ao carregar)
    static bool isBuilding(uint8_t t) { return t == ObjCabin || t == ObjShed || t == ObjTower; }
    static bool isCollectible(uint8_t t) { return t == ObjStick || t == ObjPebble || t == ObjCloth; }
    static constexpr float kSwimFloorH = -1.20f;             // piso de flutuação ao nadar no mar (mantém cabeça/braços na linha d'água y = 0)
    static constexpr float kMinWalkH = kSwimFloorH;          // abaixo disso o personagem passa a boiar/nadar
    static constexpr float kMaxSlope = 1.15f;                // inclinação máx. subindo (~49°)

    uint32_t seed = 0, features = 0, hash = 0;               // hash = CRC32 do conteúdo
    bool upgraded = false;                                   // true se foi carregado de um arquivo de versão antiga e atualizado (o servidor então regrava)
    int32_t spawnXcm = 0, spawnZcm = 0;
    int32_t oxcm = 0, ozcm = 0;    // ORIGEM da ilha no mundo (cm): 0 na ilha inicial; as do oceano aberto ficam deslocadas. Objetos, spawn e consultas (heightAt, biomeAt, pushOut) usam coordenadas GLOBAIS
    uint8_t kind = IslandBig;
    std::vector<int16_t> hcm;      // altura em cm por vértice (kVerts x kVerts, índice iz*kVerts+ix)
    std::vector<uint8_t> biome;    // bioma por vértice
    std::vector<WorldObject> objects;

    // ---- geração / arquivo ----
    static bool generate(uint32_t seed, World& out);                 // determinístico
    // Ilha do oceano aberto (não salva em arquivo: os clientes a regeneram da semente, bit a bit igual). kind = IslandKind; origem em cm.
    static bool generateIsland(uint32_t seed, uint8_t kind, int32_t oxcm, int32_t ozcm, World& out);
    std::vector<uint8_t> toBytes() const;                            // formato .e0w (com CRC32 no final)
    static bool fromBytes(const uint8_t* d, size_t n, World& out);   // valida magic, versão e CRC
    bool save(const std::string& path) const;
    static bool load(const std::string& path, World& out);

    // ---- consultas (somente leitura; seguras entre threads depois de carregado) ----
    float heightAt(float x, float z) const {                         // mesma triangulação da malha (diagonal 00-11)
        float gx = (x - oxcm * 0.01f + kHalf) * (1.0f / kCell), gz = (z - ozcm * 0.01f + kHalf) * (1.0f / kCell);
        if (gx < 0) gx = 0; else if (gx > kCells - 0.001f) gx = kCells - 0.001f;
        if (gz < 0) gz = 0; else if (gz > kCells - 0.001f) gz = kCells - 0.001f;
        int ix = (int)gx, iz = (int)gz;
        float u = gx - (float)ix, v = gz - (float)iz;
        const int16_t* p = &hcm[(size_t)iz * kVerts + ix];
        float h00 = p[0] * 0.01f, h10 = p[1] * 0.01f, h01 = p[kVerts] * 0.01f, h11 = p[kVerts + 1] * 0.01f;
        return u >= v ? h00 + (h10 - h00) * u + (h11 - h10) * v : h00 + (h11 - h01) * u + (h01 - h00) * v;
    }
    uint8_t biomeAt(float x, float z) const {
        int ix = (int)std::floor((x - oxcm * 0.01f + kHalf) / kCell + 0.5f), iz = (int)std::floor((z - ozcm * 0.01f + kHalf) / kCell + 0.5f);
        ix = ix < 0 ? 0 : (ix > kCells ? kCells : ix); iz = iz < 0 ? 0 : (iz > kCells ? kCells : iz);
        return biome[(size_t)iz * kVerts + ix];
    }
    float vertexHeight(int ix, int iz) const { return hcm[(size_t)iz * kVerts + ix] * 0.01f; }
    float spawnX() const { return spawnXcm * 0.01f; }
    float spawnZ() const { return spawnZcm * 0.01f; }

    // Empurra (x,z) para fora de árvores, pedras e construções (círculo de raio `radius`). Determinístico.
    bool blocksShot(float x, float y, float z) const;   // ponto de tiro dentro de árvore/pedra/tronco/ruína (com altura)
    void pushOut(float& x, float& z, float radius, float feetY = -1e9f) const;   // feetY: altura dos pés; troncos caídos podem ser pulados por cima (pés acima de ~55% do tronco)
    // Remove a colisão de um objeto (árvore/pedra/tronco derrubado ou minerado). Estado dinâmico: NÃO vai no arquivo do mundo.
    // Minério de uma pedra grande (ObjRock): 0 pedra comum, 1 OURO, 2 DIAMANTE. Determinístico a partir das posições (NÃO vai no arquivo do mundo):
    // as pedras são ordenadas por um hash inteiro; as primeiras viram diamante (raro: ~4%, mín. 1) e as seguintes ouro (~8%, mín. 2).
    uint8_t oreOf(uint32_t idx) const { return idx < ore_.size() ? ore_[idx] : 0; }
    void setRemoved(uint32_t idx) { if (removed_.size() != objects.size()) removed_.assign(objects.size(), 0); if (idx < removed_.size()) removed_[idx] = 1; }
    void clearRemoved(uint32_t idx) { if (idx < removed_.size()) removed_[idx] = 0; }   // objeto renasceu (volta a ter colisão)
    void clearAllRemoved() { removed_.clear(); }
    void shiftTo(int32_t newOxcm, int32_t newOzcm);   // move a ilha (objetos, spawn e grade de colisão) para a origem dada; só para ilhas do oceano
    void buildIndex();   // grade espacial dos obstáculos (chamar depois de generate/load; generate/fromBytes já chamam)

    // Colisão de um objeto: círculo (árvores, pedras: a = raio), caixa alinhada aos eixos (construções: a,b = meias-larguras já
    // com a rotação) ou cápsula (troncos caídos: a = raio, b = meio comprimento, (dx,dz) = direção do eixo).
    struct Collider { int kind = 0; float a = 0, b = 0, dx = 1, dz = 0; };   // kind: 0 círculo, 1 caixa, 2 cápsula, 3 = sem colisão (coletáveis), 4 = ruína (várias caixas, ver ruins.h)
    static Collider colliderOf(const WorldObject& o);
    static void logDir(const WorldObject& o, float& dx, float& dz);
    static int logCosQ14(int i);   // cos/sin em Q14 das 64 direções (tabela de constantes; usadas também pelo gerador, sem float)
    static int logSinQ14(int i);       // direção do eixo de um tronco (tabela inteira)

    // estatísticas p/ debug e testes
    struct Stats { double pct[BiomeCount] = {}; int count[12] = {}; int landVerts = 0; float maxH = 0; float extentM = 0; };
    Stats stats() const;

private:
    std::vector<uint8_t> ore_;                     // por objeto: minério (0 nada, 1 ouro, 2 diamante, 3 ferro), montado em buildIndex()
    std::vector<uint8_t> removed_;                 // 1 = objeto removido (sem colisão)
    std::vector<uint32_t> cellStart_, cellItems_;  // CSR: objetos por balde de 16 m
};

uint32_t crc32(const uint8_t* d, size_t n);

}  // namespace e0net
