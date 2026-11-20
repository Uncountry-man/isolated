// Isolated - MAPA / MINIMAPA estilo "papel de pergaminho sujo": textura RGBA gerada na CPU (puro C++, testado no PC por tools/maptest.cpp).
//  * Norte para cima (z decrescente = para cima). Só mostra uma ÁREA em volta do centro (círculo de raio `viewRadius`): fora dele é só papel em branco, e as bordas
//    do papel são queimadas/manchadas. Nada do mundo além dessa área aparece.
//  * Terra (altura > 0) em TINTA PRETA; o raso (0 a -9 m) vira curvas de nível finas em sépia; marcadores (você, navio, outros jogadores) desenhados por cima.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace e0net { struct TerrainSnapshot; }

namespace e0 {

struct MapMark { float x = 0, z = 0, yaw = 0; int kind = 0; };   // kind: 0 você (seta vermelha), 1 navio, 2 outro jogador (ponto)

struct MapView {
    float cx = 0, cz = 0;      // centro (m)
    float half = 300.0f;       // meia-largura da textura em metros (a textura cobre 2*half x 2*half)
    float viewRadius = 280.0f; // raio (m) da área que o mapa revela; fora disso = papel liso
    float fadeFrac = 0.86f;    // a tinta começa a se apagar em viewRadius * fadeFrac (perto de 1 = corte seco: o mapa grande acaba na borda da folha)
    int res = 192;             // lado da textura (px)
    bool circle = false;       // minimapa redondo: fora do círculo é transparente
    bool folds = false;        // vincos de dobra (mapa grande)
};

// Gera res*res*4 bytes RGBA. `seed` varia as manchas do papel (fixo = o mesmo papel sempre).
void renderMapTexture(const MapView& v, const std::vector<MapMark>& marks, std::vector<uint8_t>& rgba, uint32_t seed = 0x5EED1234u);

// Camadas separadas (para o mapa ser LISO: o papel é fixo, a tinta é gerada numa thread e só é movida/ampliada pela GPU, e os marcadores são desenhados pela UI):
//  * paper: papel (com bordas queimadas; circle = recorte redondo do minimapa, folds = vincos do mapa grande)
//  * ink: só a TINTA (terra preta, curvas de nível, halo), transparente no resto. A área revelada é o círculo (revealX, revealZ, viewRadius); fora dele não há tinta.
void renderPaper(int res, bool circle, bool folds, std::vector<uint8_t>& rgba, uint32_t seed = 0x5EED1234u);
// snap = cópia segura das ilhas (thread do mapa); nullptr = lê o Terrain global (só na thread dona do mundo, ex.: tools/maptest)
void renderInk(const MapView& v, float revealX, float revealZ, std::vector<uint8_t>& rgba, uint32_t seed = 0x5EED1234u,
               const e0net::TerrainSnapshot* snap = nullptr);

// Gera as camadas numa thread própria (a thread principal nunca espera): pedidos novos substituem os pendentes do mesmo slot.
// slots: 0 papel mini, 1 tinta mini, 2 papel grande, 3 tinta grande.
class MapWorker {
public:
    MapWorker();
    ~MapWorker();
    void requestPaper(int slot, int res, bool circle, bool folds);
    // snap: cópia das ilhas feita na thread principal (NetClient::terrainSnapshot); a thread do mapa nunca lê o Terrain global.
    void requestInk(int slot, const MapView& v, float revealX, float revealZ, std::shared_ptr<const e0net::TerrainSnapshot> snap);
    // Se há resultado novo no slot: devolve true e entrega os pixels (res*res*4) e a vista com que foi gerado.
    bool take(int slot, MapView& v, std::vector<uint8_t>& rgba);
    bool busy(int slot) const;
private:
    struct Impl; Impl* d_;
};

}  // namespace e0
