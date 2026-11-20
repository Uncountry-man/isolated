// Isolated - renderizador OpenGL ES 3.0
//   * céu procedural (sol, lua, estrelas), chão com grade
//   * personagens (Y Bot) com 3 níveis de detalhe (LOD) por distância
//   * SOMBRA: mapa de sombra direcional (sol de dia, lua à noite) centrado no jogador local, com PCF por hardware
//   * SEM reflexo planar (removido): a água reflete só o céu analiticamente; os personagens têm brilho do céu no shader
//   * 3 níveis de qualidade (BAIXA/MEDIA/ALTA) trocáveis em tempo de execução (o Engine ajusta sozinho pelo FPS)
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "anim.h"
#include "camera.h"
#include "environment.h"
#include "terrain_renderer.h"
#include "../../shared/protocol.h"
#include "skins.h"

struct AAssetManager;

namespace e0 {

struct CharInst {  // um jogador a desenhar (pés em `pos`)
    Vec3 pos; float yaw; Vec3 color;
    bool hidden = false;  // não desenha no passe principal (1ª pessoa), mas continua projetando sombra
    const Vec4* boneRows = nullptr;  // 195 vec4 (65 ossos x 3 linhas); nullptr = pose de repouso
    float speed = 0.0f;   // velocidade horizontal (m/s), usada p/ rastro na água (boiar vs nadar)
    uint8_t carried = 0;  // sem uso: ferramentas NÃO são mais desenhadas guardadas no corpo (campo mantido por compatibilidade do protocolo)
    uint8_t held = 0;     // e0net::HeldTool: ferramenta na mão direita (0 = nada); acompanha o osso da mão via skinning
    bool hasLocal = false; Mat4 local;   // transformação extra depois de translate(pos)*rotY(-yaw) (corpo caído carregado no ombro)
    int skin = -1;        // SKIN (skins.h): -1 = só `color` (juntas escuras padrão); >= 0 sobrepõe cores e material
    const Vec4* attachRows = nullptr;   // AnimPlayer::attachRows(): ferramenta/itens/TAG presos ao corpo (nullptr = boneRows)
};

// model: 0 caixa, 1 graveto, 2 pedrinha, 3 pedra, 4 tora (modelos de props_data.h), 5 machadinha, 6 picareta, 7 coco, 8 folha de palmeira (.e0m das ferramentas); seed varia a forma
struct DropVis { Vec3 pos; float yaw; Vec3 size; Vec3 color; int model = 0; uint32_t seed = 0; int shine = 0; float scale = 1.0f; };   // shine: 1 ouro, 2 diamante (modelo 2 pintado com `color`)   // item solto no chão: caixa colorida (pos = base; size = largura X, altura Y, profundidade Z em m)

// Bandeira pirata pronta para desenhar (Renderer::uiFlag). Cores já resolvidas; padrão/recorte/desgaste = índices do shader kFlagFS.
struct FlagLook {
    Vec3 c0{0.06f, 0.06f, 0.06f}, c1{0.75f, 0.10f, 0.10f}, c2{0.95f, 0.92f, 0.85f};   // fundo, detalhe do padrão, figura
    int pattern = 0;   // 0..30 (nomes em flagPatternName, engine_flag.inc): 0 lisa, 1 faixa, 2 metades, 3 listras, 4 faixa no mastro, 5 diagonal, 6 cruz, 7 aspa, 8 xadrez, 9 moldura, 10 cantão, 11 ondas, 12 raios, 13 triângulo, 14 disco, 15 duas faixas
    int edge = 0;      // 0 reta, 1 rabo de andorinha, 2 flâmula, 3 bico, 4 serrilhada, 5 rasgada
    int wear = 0;      // 0 nova, 1 gasta, 2 farrapo
    int icon = -1;     // figura: índice no atlas de ícones (-1 = sem)
    float iconX = 0.5f, iconY = 0.5f, iconSize = 0.30f;   // centro da figura (s, v: 0..1) e meia altura (fração da altura do pano)
    bool mirror = false, outline = true;
    float seed = 0.0f;   // variação das manchas/rasgos
};

struct GfxStats {  // do último frame (para o painel de debug)
    int quality = 1, tris = 0, draws = 0, shadowCasters = 0;
    int lod[3] = {0, 0, 0};
    int shadowRes = 0;
    float shadowRadius = 0;
    float gpuMs = -1;   // tempo de GPU do frame 3D (sombra+cena), suavizado; -1 = sem timer
    bool shadowOn = false;
    int terrChunks = 0, terrTris = 0, propChunks = 0, propTris = 0, terrCulled = 0, grassChunks = 0, grassTufts = 0, smallChunks = 0, smallTris = 0;
};

// MASTRO DE DOMINAÇÃO de uma ilha (shared/conquest.h): poste + bandeira 3D da tripulação que tremula. key = os 11 bytes da bandeira
// (a textura do pano é gerada 1 vez por desenho, num FBO, com o mesmo shader da UI). prog 0..1 = altura da bandeira no mastro.
struct IslandFlagDraw { Vec3 base; bool cloth = false; float prog = 1.0f; FlagLook look; std::string key; float seed = 0; };

// Navio pirata (assets/ship.e0s): pose do casco (origem do modelo, linha d'água em y = 0) e ângulo do timão (rad)
struct ShipDraw { bool exists = false; float x = 0, z = 0, yaw = 0, wheel = 0, chestOpen = 0, sinkT = 0; };   // chestOpen: tampa do baú 0..1; sinkT > 0: afundando (ShipState.sinkT)

class Renderer {
public:
    void setAssetManager(AAssetManager* m) { assets_ = m; }  // antes de init()
    bool init();      // requer contexto GL atual
    void shutdown();
    void resize(int w, int h);
    void setTerrain(const e0net::World* w);  // terreno + objetos da ILHA INICIAL (nullptr = só água); reconstrói se o contexto GL for recriado
    void setIsland(int slot, const e0net::World* w);   // ilha do oceano aberto (slot 1..80; a inicial é o slot 0 = setTerrain)
    void clearIslands();
    // Coletável pego: some do mundo (estado guardado aqui e reaplicado se o contexto GL for recriado).
    void setCollectibleHidden(uint32_t gid, bool hidden);   // gid = (ilha << 20) | índice (terrain.h)
    // Árvore/pedra/tora removida do mundo: some do terreno; com `effect` ela encolhe girando até sumir (~0.8 s).
    void removeWorldProp(uint32_t gid, bool effect);
    void restoreWorldProp(uint32_t gid);   // o objeto renasceu: volta a ser desenhado
    void setGrade(float k) { gradeK_ = k; }    // filtro de cor pirata (0 = desligado, 1 = cheio)
    void setFogScale(float k) { fogK_ = k; }   // 1 = névoa normal; <1 = enxerga mais longe (câmera de canhoneiro)
    void setEnvironment(const Environment& e) { env_ = e; }  // céu/luz do frame (antes de renderFrame)

    enum { QLow = 0, QMed = 1, QHigh = 2 };
    void setQuality(int q);  // recria o alvo de sombra se mudar
    int quality() const { return quality_; }
    static const char* qualityName(int q);

    // Desenha o frame 3D completo: passe de sombra e passe principal.
    // `focus` = ponto em torno do qual o mapa de sombra é centrado (o jogador local).
    void renderFrame(const Camera& cam, const std::vector<CharInst>& chars, const Vec3& focus);
    // Personagem isolado num retângulo da tela (vx, vy = canto INFERIOR esquerdo, em pixels GL): tela PERSONAGEM do menu. Sem sombra, sem céu;
    // limpa só a profundidade do retângulo (o fundo 2D desenhado antes continua). Usa a luz de setEnvironment.
    // sx..sh = recorte (scissor) opcional dentro do retângulo; sw <= 0 = o próprio retângulo
    void drawCharPreview(const CharInst& c, int vx, int vy, int vw, int vh, const Camera& cam, int lod, int sx = 0, int sy = 0, int sw = 0, int sh = 0);

    // MELHORIAS DA ILHA (shared/isle_build.h): prévia translúcida da estrutura sendo posicionada (verde = pode, vermelho = não pode)
    struct StructGhost { bool active = false, ok = true; uint8_t kind = 0, rot = 0; Vec3 pos{}; int selId = 0; int slot = -1; };   // slot: ilha (a MURALHA usa o anel dela)   // selId != 0: destaca essa estrutura (demolir)
    void setStructGhost(const StructGhost& g) { ghost_ = g; }
    void requestStructThumbs() { for (unsigned t : thumbTex_) if (!t) { wantThumbs_ = true; break; } }   // gera no próximo renderFrame
    unsigned structThumb(int kind) const { return kind >= 0 && kind < e0net::SkKindCount ? thumbTex_[kind] : 0u; }
    void uiTexture(unsigned tex, float cx, float cy, float halfW, float halfH, float alpha);   // textura inteira no retângulo (entre beginUi/endUi)
    // Efeito pontual (usa as partículas dos respingos): 0 = bala na água (coluna d'água + ondas), 1 = bala em terra (terra/poeira), 2 = fumaça do tiro do canhão.
    void burst(const Vec3& pos, int kind);
    // BACAMARTE: clarão + fumaça na boca do cano (e um clarão de luz rápido), rastro de um chumbo de a até b, impacto (kind = e0net::ShotKind:
    // 1 terra/poeira, 2 água, 3 sangue no corpo, 4 sangue na cabeça). gunMuzzle: boca do cano e direção do personagem (arma na mão).
    void gunShot(const Vec3& muzzle, const Vec3& dir);
    void gunTracer(const Vec3& a, const Vec3& b);
    void gunImpact(const Vec3& p, const Vec3& dir, int kind);
    static void gunMuzzle(const CharInst& c, Vec3& pos, Vec3& dir);
    void setDrops(const std::vector<DropVis>& d) { drops_ = d; }
    void setIslandFlags(const std::vector<IslandFlagDraw>& f) { isleFlags_ = f; }
    void setShip(int i, const ShipDraw& s) { ships_[i] = s; }
    // Modo cinema: qualidade acima de ALTA (sombra cobre um navio inteiro, longe visível) e relógio do roteiro no lugar do relógio real
    void setCinema(bool on);
    void setClock(double sec) { clock_ = sec; }
    const GfxStats& gfx() const { return gfx_; }
    bool hasGpuTimer() const { return gpuTimer_; }
    const AnimBank& anims() const { return anims_; }
    const Skeleton* skeletonOfSkin(int skin) const { int m = skinDef(skin).model; return skel_[m < 0 || m >= kCharModels ? 0 : m].valid ? &skel_[m < 0 || m >= kCharModels ? 0 : m] : nullptr; }

    // Linha vertical do mundo (indicador de coleta): do ponto `base` ao `top`, SEMPRE virada para a câmera (billboard cilíndrico) e
    // com largura constante em pixels. Chamar depois de renderFrame e antes da UI. alpha 0..1.
    void drawWorldLine(const Camera& cam, const Vec3& base, const Vec3& top, float alpha, float widthPx);

    // UI 2D em pixels (origem no canto superior esquerdo). Chamar entre beginUi() e endUi().
    enum UiShape { UiFill = 0, UiRing = 1, UiRunIcon = 2, UiRoundRectFill = 3, UiRoundRectBorder = 4 };
    void beginUi();
    void uiShape(UiShape s, float cx, float cy, float r, float red, float green, float blue, float alpha);
    void uiRoundRect(float cx, float cy, float halfW, float halfH, float radius,
                     float red, float green, float blue, float alpha);
    void uiRoundRectBorder(float cx, float cy, float halfW, float halfH, float radius, float thickness,
                           float red, float green, float blue, float alpha);
    // Ícone de inventário (atlas assets/icons.e0i, 4x3 células: ver tools/make_icons.py). index = linha*4 + coluna; half = meia largura em px.
    // Canto côncavo da tela (efeito de pouca vida/fome): quadrado de lado 2*half centrado em (cx,cy), cor só fora do arco do canto oposto. dir = (±1,±1) rumo ao canto da tela.
    void uiCornerGlow(float cx, float cy, float half, float dirx, float diry, float red, float green, float blue, float alpha);
    void uiArc(float cx, float cy, float r, float thickness, float frac, float red, float green, float blue, float alpha);   // progresso circular (horário a partir do topo)
    // Retângulo arredondado com DEGRADÊ linear de c0 para c1 na direção (dirx, diry) (vP -1..1; (0,1) = de cima para baixo) + textura "grunge" opcional
    // (pinceladas diagonais e pontinhos procedurais; grunge 0 = liso, ~1 = forte; seed muda o padrão). Usado pela LOJA.
    void uiGrad(float cx, float cy, float halfW, float halfH, float radius, float r0, float g0, float b0, float a0,
                float r1, float g1, float b1, float a1, float dirx, float diry, float grunge = 0.0f, float seed = 0.0f);
    // Brilho radial macio (elipse halfW x halfH; alfa cai até 0 na borda)
    void uiCompass(float cx, float cy, float r, float alpha);   // rosa dos ventos de mapa de piratas (procedural)
    void uiGlow(float cx, float cy, float halfW, float halfH, float red, float green, float blue, float alpha);
    // Tema oficial "Carta Nautica & Carvalho" (ui_theme.h): painéis de madeira/couro/pergaminho, botões táteis, medalhão X e divisória com losango
    void uiWoodPanel(float cx, float cy, float halfW, float halfH, float radius, int style, float alpha = 1.0f, float seed = 0.0f);
    void uiButton(float cx, float cy, float halfW, float halfH, float radius, int style, float alpha = 1.0f);
    void uiCloseBtn(float cx, float cy, float r, float alpha = 1.0f);
    void uiDivider(float cx, float cy, float halfW, float halfH, float alpha = 1.0f);
    void uiIcon(int index, float cx, float cy, float half, float alpha);
    void uiIconTint(int index, float cx, float cy, float half, float r, float g, float b, float alpha);   // ícone branco do atlas multiplicado por (r,g,b)
    // BANDEIRA: pano centrado em (cx, cy) com meia largura/altura halfW/halfH (px; o mastro fica à ESQUERDA). wave 0 = parada (miniaturas), 1 = tremulando.
    void uiFlag(const FlagLook& f, float cx, float cy, float halfW, float halfH, float time, float wave, float alpha = 1.0f);
    bool hasIcons() const { return texIcons_ != 0; }
    // Mapa (textura RGBA gerada na CPU, ver map_gen.h): slots 0/1 = papel/tinta do minimapa, 2/3 = papel/tinta do mapa grande. uiMap desenha o retângulo (ou círculo) centrado em (cx, cy).
    void mapUpload(int slot, int res, const uint8_t* rgba);
    // u0,v0,u1,v1 = trecho da textura mostrado no retângulo (0,0,1,1 = tudo): a GPU move/amplia a camada de tinta sem regerar.
    void uiMap(int slot, float cx, float cy, float halfW, float halfH, bool circle, float alpha, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
    // Mapa grande como FOLHA de papel: só o disco (centro sheetCx/sheetCy, raio sheetR, em px) aparece, com a borda irregular e QUEIMADA (estática, estilo cartoon).
    // paper = true: bandas de queimadura + grade (linhas a cada gridStep metros; (gridCx, gridCz) = ponto do mundo no centro da tela, mPerPx = metros por pixel); false (tinta): só recortada.
    void uiMapSheet(int slot, bool paper, float cx, float cy, float halfW, float halfH, float sheetCx, float sheetCy, float sheetR,
                    float gridCx, float gridCz, float mPerPx, float gridStep, float u0, float v0, float u1, float v1, float sheetH = 0.0f);
    // sheetH > 0: folha RETANGULAR (sheetR = meia-largura, sheetH = meia-altura, cantos arredondados) e, no papel, LINHAS DE RUMO saindo do ponto de tela (gridCx, gridCz) com a rosa de raio mPerPx (px) em vez da grade.
    // Marcador do mapa (desenhado pela GPU, liso): kind 0 = você (seta vermelha), 1 = navio (azul), 2 = outro jogador (ponto); yaw como o do jogador (0 = -Z).
    void uiMapTilt(int slot, float cx, float cy, float R, float alpha, float uc, float vc, float scale, float yaw, float gridX = 0, float gridZ = 0, bool grid = false);   // minimapa girado (yaw da câmera) e inclinado
    void uiMarker(int kind, float cx, float cy, float r, float yaw);
    void endUi();

    // Texto/retângulos em pixels (fonte Cinzel com acentos nativos, drop-in com scale). Chamar entre beginText() e endText().
    void beginText();
    void text(float x, float y, int scale, const char* s, float r, float g, float b, float a, bool shadow = true);
    void rect(float x, float y, float w, float h, float r, float g, float b, float a);
    void endText();
    static float textWidth(const char* s, int scale);  // largura em pixels (Cinzel proporcional)

    // Texto monoespaçado 5x7 reservado ao painel de DEBUG (compacto e alinhado).
    void beginMonoText();
    void monoText(float x, float y, int scale, const char* s, float r, float g, float b, float a, bool shadow = true);
    void monoRect(float x, float y, float w, float h, float r, float g, float b, float a);
    void endMonoText();
    static float monoTextWidth(const char* s, int scale);

private:
    struct Sub { unsigned vao = 0, vbo = 0, ibo = 0; int count = 0; Vec3 color; bool body = false; };
    struct Mesh { std::vector<Sub> subs; Vec3 bmin{1e9f, 1e9f, 1e9f}, bmax{-1e9f, -1e9f, -1e9f}; unsigned tex = 0; };   // tex != 0: malha com UV (E0M3) e textura (personagens)
    struct Lit {  // locais dos uniforms de iluminação/sombra (iguais em chão, personagem e céu)
        int cam = -1, ambient = -1, lightDir = -1, lightCol = -1, horizon = -1, zenith = -1, sunDir = -1, night = -1;
        int shadowOn = -1, shadowTexel = -1, shadowRadius = -1, pcf = -1, shadowCenter = -1, shadowInv = -1, shadowMat = -1, shadow = -1, torch = -1, fogK = -1;
    };
    bool loadMesh(const char* name, Mesh& out);
    void drawMesh(int lod, const Mat4& mvp, const Mat4& model, const Vec3& body, const Vec4* boneRows, bool depthOnly);
    void drawMeshRef(const Mesh& mesh, const Mat4& mvp, const Mat4& model, const Vec3& body, const Vec4* boneRows, bool depthOnly, float alpha = 1.0f);
    void drawTool(uint8_t held, const Mat4& mvp, const Mat4& model, const Vec4* boneRows, bool depthOnly);
    void applyLit(const Lit& L, const Vec3& cam);
    void drawSky(const Camera& cam, float aspect);
    void createTargets();
    void gpuTimerBegin();
    void gpuTimerEnd();
    void destroyTargets();
    void quad(float x, float y, float w, float h, int cell, float r, float g, float b, float a);

    AAssetManager* assets_ = nullptr;
    Environment env_;
    float fogK_ = 1.0f, gradeK_ = 0.0f;
    unsigned gradeProg_ = 0; int uGradeK_ = -1, uGradeAspect_ = -1;
    GfxStats gfx_;
    bool ready_ = false;
    bool gpuTimer_ = false;           // GL_EXT_disjoint_timer_query disponível
    unsigned gpuQ_[3] = {0, 0, 0};
    bool gpuPending_[3] = {false, false, false};
    int gpuIdx_ = 0;
    float gpuMs_ = -1;
    int quality_ = QMed;
    int w_ = 1, h_ = 1;

    Mesh chars_[kCharModels][3];                                 // PERSONAGENS (piratas): por modelo, LOD0 (completo), LOD1, LOD2 (assets/pirate_<m|f>[_lodN].e0m)
    unsigned charTex_[kCharModels][3] = {};                      // textura de cada modelo x LOD (assets/pirate_<m|f>[_lod1|_lod2].e0i: 1024/512/256)
    unsigned loadTexture(const char* name);                      // E0I1 RGBA -> textura GL com mipmaps (0 se falhar)
    int uCharTex_ = -1, uCharTexOn_ = -1;
    Mesh toolMeshes_[5];                                         // machadinha, picareta, tocha, adaga, bacamarte (.e0m presos ao osso da mão direita)
    Mesh cubeMesh_;                                              // caixa unitária (base em y=0) p/ itens soltos
    std::vector<DropVis> drops_;
    void makeCube();
    static constexpr int kShips = 4;   // = e0net::kMaxShips
    ShipDraw ships_[kShips];
    unsigned shipProg_ = 0, shipVao_[kShips] = {}, shipVbo_[kShips] = {}, shipIbo_[kShips] = {}, shipShadowVao_[kShips] = {}, shipShadowIbo_[kShips] = {}, wheelVao_[kShips] = {}, wheelVbo_[kShips] = {};
    int shipCount_[kShips] = {}, shipShadowCount_[kShips] = {}, wheelCount_[kShips] = {};
    float wheelLast_[kShips] = {-999.0f, -999.0f, -999.0f, -999.0f};
    float chestLast_[kShips] = {-999.0f, -999.0f, -999.0f, -999.0f};
    int uShipVP_ = -1, uShipModel_ = -1, uShipClip_ = -1, uShipTime_ = -1;
    Lit litShip_;
    bool loadShip(int i, const char* file);
    void drawShip(int i, const Camera& cam, const Mat4& vp, float nowSec, float clipY = -4.0f);
    bool cine_ = false; double clock_ = -1.0;
    void drawShipShadow(int i, const Mat4& lvp);
    Mat4 shipModel(int i) const;
    unsigned dropVao_ = 0, dropVbo_ = 0;   // itens soltos com modelo de props (vértices transformados na CPU a cada frame)
    void drawDropProps();
    // MELHORIAS DA ILHA: uma malha (cores de vértice) por ilha, refeita quando isleStructGen muda
    struct StructMesh { unsigned vao = 0, vbo = 0; int count = 0; uint32_t gen = 0; bool built = false; };
    StructMesh structMesh_[e0net::kIslandSlots];
    StructGhost ghost_;
    unsigned thumbTex_[e0net::SkKindCount] = {}; bool wantThumbs_ = false;
    void bakeStructThumbs();
    void drawStructs(const Camera& cam, const Mat4& vp, float nowSec);
    void drawStructShadow(const Vec3& focus, float reach, const Mat4& lvp);
    void drawStructGhost(const Mat4& vp);
    unsigned gizmoProg_ = 0, gizmoVao_ = 0, gizmoVbo_ = 0;
    int uGizmoVP_ = -1;
    Skeleton skel_[kCharModels];                                 // esqueleto próprio de cada personagem (pirate_<m|f>.e0k)
    AnimBank anims_;                                             // banco de animações (ybot.e0a)
    Vec4 identBones_[kBoneRows] = {};
    unsigned groundProg_ = 0, groundVao_ = 0, groundVbo_ = 0;
    unsigned charProg_ = 0, shadowProg_ = 0, skyProg_ = 0;
    Lit litGround_, litChar_, litSky_, litTerrain_, litGrass_, litProp_;
    unsigned grassProg_ = 0, markerProg_ = 0, markerVao_ = 0, markerVbo_ = 0;
    int uMarkerVP_ = -1;
    int uGrassVP_ = -1, uGrassTime_ = -1, uGrassDist_ = -1, uGrassClip_ = -1;
    TerrainRenderer terr_[e0net::kIslandSlots];   // [0] = ilha inicial; as demais = ilhas do oceano aberto (coordenadas globais, cada uma com a sua grade)
    const e0net::World* islSrc_[e0net::kIslandSlots] = {};
    bool anyTerrain() const { for (const auto& t : terr_) if (!t.empty()) return true; return false; }
    std::vector<uint8_t> hiddenProp_[e0net::kIslandSlots];    // 1 = objeto grande removido (reaplicado se o contexto GL for recriado)
    struct Ghost { std::vector<uint8_t> verts; Vec3 base; float t0 = 0; float spin = 1; };
    std::vector<Ghost> ghosts_;
    float lastNow_ = 0;
    void drawGhosts();
    unsigned ghostVao_ = 0, ghostVbo_ = 0;
    std::vector<uint8_t> hiddenSmall_[e0net::kIslandSlots];   // 1 = coletável escondido (por índice de objeto)
    unsigned terrainProg_ = 0, propProg_ = 0;
    int uTerrainVP_ = -1, uTerrainClip_ = -1, uTerrainAlbedo_ = -1, uTerrainNormal_ = -1;
    int uPropVP_ = -1, uPropClip_ = -1, uPropTime_ = -1;
    unsigned texTerrainAlbedo_ = 0, texTerrainNormal_ = 0, texIcons_ = 0, texMap_[4] = {0, 0, 0, 0};
    int mapRes_[4] = {0, 0, 0, 0};
    int uUiIcon_ = -1, uUiUv_ = -1;
    int iconRows_ = 3;   // linhas de 128 px do atlas de ícones (itens: 3; HUD: depois)
    bool loadIcons();
    int uGroundTime_ = -1, uGroundOff_ = -1;
    int uGroundVP_ = -1, uGroundGloss_ = -1, uGroundScreenInv_ = -1;
    struct SplashParticle {
        Vec3 pos{};
        Vec3 vel{};
        float size = 0.1f;
        float age = 0.0f;
        float life = 0.45f;
        float maxAlpha = 0.7f;
        bool flat = false;   // espuma/ondulação deitada na superfície da água (horizontal); false = gotícula (billboard que cai de volta na água)
    };
    std::vector<SplashParticle> splashes_;
    float splashTimer_[e0net::kMaxPlayers + 1] = {};
    float lastSplashTime_ = -1.0f;
    void updateSplashes(float nowSec, const std::vector<CharInst>& chars);
    void drawSplashes(const Camera& cam, const Mat4& vp);
    unsigned splashProg_ = 0, splashVao_ = 0, splashVbo_ = 0;
    unsigned flameProg_ = 0, glowProg_ = 0;                       // chama da tocha e halo (reusam o VAO/VBO dos respingos)
    int uFlameVP_ = -1, uFlameTime_ = -1, uGlowVP_ = -1;
    Vec4 torch_[4] = {};                                          // luzes das tochas acesas (xyz mundo, w intensidade) -> uTorch
    int nFlames_ = 0;
    void updateTorches(const Camera& cam, const std::vector<CharInst>& chars);
    void drawFlames(const Camera& cam, const Mat4& vp);
    int uSplashVP_ = -1;
    struct FxPart { Vec3 pos, vel; float size, grow, age, life, drag, grav; float r, g, b, a; bool add; };
    struct FxTracer { Vec3 a, b; float age, life, len; };
    std::vector<FxPart> fx_; std::vector<FxTracer> tracers_;
    float flashAt_ = -10.0f; Vec3 flashPos_{};
    unsigned fxProg_ = 0, fxVao_ = 0, fxVbo_ = 0; int uFxVP_ = -1;
    void updateFx(float dt);
    void drawFx(const Camera& cam, const Mat4& vp);
    int uCharSkin_ = -1, uCharSkin2_ = -1, uCharSkinT_ = -1;
    const SkinDef* pendSkin_ = nullptr;   // skin do próximo drawMesh/drawMeshRef (consumida por ele)
    void useSkin(int skin) { pendSkin_ = skin >= 0 ? &skinDef(skin) : nullptr; }
    int uCharMVP_ = -1, uCharModel_ = -1, uCharColor_ = -1, uCharSkinned_ = -1, uCharBones_ = -1, uCharClip_ = -1, uCharAlpha_ = -1;
    int uShadowMVP_ = -1, uShadowSkinned_ = -1, uShadowBones_ = -1;
    int uSkyFwd_ = -1, uSkyRight_ = -1, uSkyUp_ = -1, uSkyTan_ = -1;

    // alvos de render
    unsigned shadowFbo_ = 0, shadowTex_ = 0;
    int shadowRes_ = 0;
    // estado do frame (matriz de sombra etc.)
    Mat4 shadowMat_;
    Vec3 shadowCenter_;
    float shadowOn_ = 0, shadowTexel_ = 0.05f, shadowRadius_ = 14, pcf_ = 1;

    unsigned textProg_ = 0, textVao_ = 0, textVbo_ = 0, fontTex_ = 0, uiFontTex_ = 0;
    int uTextScreen_ = -1, uTextTex_ = -1;
    std::vector<float> tbatch_;
    bool textMono_ = false;
    void quadUv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, float r, float g, float b, float a);
    unsigned uiProg_ = 0, uiVao_ = 0, uiVbo_ = 0;
    int uUiRect_ = -1, uUiScreen_ = -1, uUiColor_ = -1, uUiMode_ = -1;
    int uUiRadius_ = -1, uUiThickness_ = -1, uUiSheet_ = -1, uUiGrid_ = -1;
    unsigned flagProg_ = 0;
    // mastros de dominação (drawIslandFlags): pano 3D com a textura da bandeira (cache por desenho)
    std::vector<IslandFlagDraw> isleFlags_;
    struct BakedFlag { unsigned tex = 0; double used = 0; };
    std::unordered_map<std::string, BakedFlag> bakedFlags_;
    unsigned clothProg_ = 0, clothVao_ = 0, clothVbo_ = 0, poleVao_ = 0, poleVbo_ = 0; int clothCount_ = 0;
    int uClothVP_ = -1, uClothBase_ = -1, uClothDir_ = -1, uClothSize_ = -1, uClothTime_ = -1, uClothTex_ = -1;
    Lit litCloth_;
    void bakeIslandFlags();   // antes do passe de sombra: gera as texturas que faltam (troca de FBO fora do passe principal)
    void drawIslandFlags(const Camera& cam, const Mat4& vp, float nowSec);
    int uFlagRect_ = -1, uFlagScreen_ = -1, uFlagCloth_ = -1, uFlagC0_ = -1, uFlagC1_ = -1, uFlagC2_ = -1, uFlagStyle_ = -1, uFlagIconUv_ = -1, uFlagIconT_ = -1, uFlagWave_ = -1, uFlagIcon_ = -1;
};

}  // namespace e0
