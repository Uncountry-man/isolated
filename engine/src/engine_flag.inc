// ================= BANDEIRA PIRATA (editor) =================
// Incluído DENTRO de `struct Engine` (engine.cpp). Botão BANDEIRA ao lado do CHAT (abaixo do minimapa) abre o editor em TELA CHEIA:
//   esquerda = a bandeira tremulando no mastro + ALEATÓRIA / DESFAZER / SALVAR;
//   direita  = painel com abas CORES (alvo + paleta e esquemas prontos com ROLAGEM HORIZONTAL), PADRÃO e FIGURA (grades com ROLAGEM VERTICAL)
//              e ESTILO (ponta, desgaste, tamanho/posição da figura, espelhar, contorno).
// Listas que rolam: arrastar o dedo (com inércia); tocar sem arrastar escolhe o item (a escolha só acontece ao soltar).
// A bandeira é SÓ DO CLIENTE por enquanto: salva em menu.cfg (chave flag=). No futuro vai para o navio de cada jogador (precisa de protocolo:
// mandar FlagDesign = 11 bytes). Desenho: Renderer::uiFlag (shader kFlagFS); as figuras são ícones do game-icons.net no atlas (tools/make_hud_icons.py).

struct FlagDesign {   // 11 bytes, todos índices (vale para salvar e, no futuro, para a rede)
    uint8_t bg = 0, detail = 1, figCol = 7, pattern = 0, edge = 0, wear = 1, fig = 1, size = 1, pos = 1, mirror = 0, outline = 0;
    bool operator==(const FlagDesign& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
    bool operator!=(const FlagDesign& o) const { return !(*this == o); }
};
static_assert(sizeof(FlagDesign) == 11, "FlagDesign sem padding");

struct FlagColor { const char* name; float r, g, b; };
static const FlagColor* flagColors(int& n) {   // a ordem é o índice salvo: só acrescentar NO FIM
    static const FlagColor k[] = {
        {"PRETO", 0.05f, 0.05f, 0.06f}, {"CARV\xC3" "O", 0.19f, 0.18f, 0.18f}, {"VERMELHO", 0.72f, 0.08f, 0.07f}, {"VINHO", 0.40f, 0.05f, 0.09f},
        {"LARANJA", 0.90f, 0.42f, 0.08f}, {"OURO", 0.90f, 0.68f, 0.17f}, {"AMARELO", 0.97f, 0.86f, 0.30f}, {"OSSO", 0.90f, 0.86f, 0.74f},
        {"BRANCO", 0.97f, 0.97f, 0.95f}, {"VERDE-MAR", 0.10f, 0.52f, 0.45f}, {"VERDE", 0.12f, 0.35f, 0.14f}, {"MARINHO", 0.06f, 0.12f, 0.30f},
        {"AZUL", 0.12f, 0.36f, 0.75f}, {"ROXO", 0.36f, 0.12f, 0.48f}, {"MARROM", 0.38f, 0.22f, 0.10f}, {"CINZA", 0.50f, 0.50f, 0.52f},
        {"SALM\xC3" "O", 0.92f, 0.50f, 0.42f}, {"ROSA", 0.85f, 0.35f, 0.55f}, {"MAGENTA", 0.65f, 0.10f, 0.45f}, {"LAVANDA", 0.62f, 0.55f, 0.82f},
        {"AZUL-C\xC9U", 0.45f, 0.70f, 0.92f}, {"TURQUESA", 0.15f, 0.75f, 0.78f}, {"PETR\xD3LEO", 0.05f, 0.28f, 0.32f}, {"MUSGO", 0.40f, 0.45f, 0.15f},
        {"OLIVA", 0.50f, 0.52f, 0.22f}, {"LIMA", 0.55f, 0.80f, 0.20f}, {"AREIA", 0.82f, 0.70f, 0.48f}, {"BRONZE", 0.70f, 0.45f, 0.20f},
        {"COBRE", 0.72f, 0.33f, 0.18f}, {"FERRUGEM", 0.55f, 0.22f, 0.10f}, {"CHUMBO", 0.30f, 0.32f, 0.36f}, {"ANIL", 0.20f, 0.18f, 0.55f},
    };
    n = (int)(sizeof k / sizeof k[0]); return k;
}
struct FlagFig { const char* name; int icon; };
static const FlagFig* flagFigs(int& n) {   // a ordem é o índice salvo: só acrescentar NO FIM
    static const FlagFig k[] = {
        {"NENHUMA", -1}, {"CAVEIRA", HiSkull}, {"CAVEIRA SINISTRA", HiFlSkull2}, {"CAVEIRA REAL", HiFlCrownSkull}, {"CAVEIRA RACHADA", HiFlSkullCrack},
        {"OSSOS CRUZADOS", HiFlBones}, {"SABRES", HiSabres}, {"ESPADAS", HiFlSwords}, {"\xC2" "NCORA", HiFlAnchor}, {"LEME", HiWheel},
        {"GALE\xC3" "O", HiGalleon}, {"CANH\xC3" "O", HiCannon}, {"POLVO", HiFlOctopus}, {"SERPENTE DO MAR", HiFlSerpent}, {"TUBAR\xC3" "O", HiFlShark},
        {"BARBATANA", HiFlFin}, {"CORVO", HiFlRaven}, {"PAPAGAIO", HiParrot}, {"AMPULHETA", HiFlHourglass}, {"CORA\xC7\xC3" "O", HiFlHeart},
        {"ROSA", HiFlRose}, {"TRIDENTE", HiFlTrident}, {"DRAG\xC3" "O", HiFlDragon}, {"LOBO", HiFlWolf}, {"LE\xC3" "O", HiFlLion},
        {"COBRA", HiFlSnake}, {"ESCORPI\xC3" "O", HiFlScorpion}, {"SOL", HiFlSun}, {"LUA", HiFlMoon}, {"ESTRELA", HiFlStar},
        {"RAIO", HiFlBolt}, {"COROA", HiCrown}, {"FANTASMA", HiGhost}, {"JOIA", HiGem}, {"CHAP\xC9U", HiPirateHat},
        {"BACAMARTE", HiGun},
        {"DRAG\xC3" "O DO MAR", HiFlSeaDragon}, {"\xC1" "GUA-VIVA", HiFlJelly}, {"CONCHA", HiFlShell}, {"NASCER DO SOL", HiFlSunrise}, {"CAVEIRA ESTELAR", HiFlStarSkull},
        {"TRIDENTE DE FOGO", HiFlFireTrident}, {"CORVO EM VOO", HiFlCrowDive}, {"CORA\xC7\xC3" "O PARTIDO", HiFlBrokenHeart}, {"ESPADA NA PEDRA", HiFlSwordStone}, {"LOBO UIVANDO", HiFlWolfHowl},
        {"REDEMOINHO", HiFlSwirl}, {"ESTRELA-DO-MAR", HiFlSeaStar}, {"L\xC2" "MINA", HiFlBlade}, {"CAVEIRA RAIO", HiFlSkullBolt}, {"\xC1" "GUIA", HiFlEagle},
        {"GARRAS", HiFlClaws}, {"OLHO", HiFlEye}, {"LUA SINISTRA", HiFlEvilMoon}, {"FOICE", HiFlScythe}, {"CEIFADOR", HiFlReaper},
        {"CAVEIRA KRAKEN", HiFlKrakenSkull}, {"BALEIA", HiFlWhale}, {"PEIXE-DIABO", HiFlAngler}, {"TARTARUGA", HiFlTurtle}, {"CARANGUEJO", HiFlCrab},
        {"CAVALO-MARINHO", HiFlSeahorse}, {"SEREIA", HiFlMermaid}, {"CHAVE", HiFlKey}, {"\xC1" "S", HiFlAce}, {"MORCEGO", HiFlBat},
        {"HIDRA", HiFlHydra}, {"ANEL DE FOGO", HiFlFireRing}, {"CAVEIRA CHIFRUDA", HiFlHornSkull}, {"DEM\xD4" "NIO", HiFlDemon}, {"TR\xCA" "S CAVEIRAS", HiFlTripleSkull},
        {"ARP\xC3" "O", HiFlHarpoon}, {"FAROL", HiFlLighthouse}, {"NAUFR\xC1" "GIO", HiFlWreck}, {"ONDA", HiFlWave}, {"TEIA", HiFlWeb},
    };
    n = (int)(sizeof k / sizeof k[0]); return k;
}
static constexpr int kFlagPatterns = 31, kFlagEdges = 6, kFlagWears = 3;
static const char* flagPatternName(int i) {   // = índices do shader kFlagFS
    static const char* k[kFlagPatterns] = {"LISA", "FAIXA", "METADES", "LISTRAS", "MASTRO", "DIAGONAL", "CRUZ", "ASPA", "XADREZ", "MOLDURA", "CANT\xC3" "O",
        "ONDAS", "RAIOS", "TRI\xC2" "NGULO", "DISCO", "DUAS FAIXAS", "PALA", "QUARTOS", "LOSANGO", "BOLINHAS", "LISTRAS DIAG.", "CHEVRON", "SOL NASCENTE",
        "CRUZ GREGA", "ANEL", "DENTES", "ARLEQUIM", "MOLDURA DUPLA", "GRADE", "FAIXA DIAG.", "SEMIC\xCD" "RCULO"};
    return k[std::max(0, std::min(kFlagPatterns - 1, i))];
}
static const char* flagEdgeName(int i) { static const char* k[6] = {"RETA", "ANDORINHA", "FL\xC2" "MULA", "BICO", "SERRILHADA", "RASGADA"}; return k[std::max(0, std::min(5, i))]; }
static const char* flagWearName(int i) { static const char* k[3] = {"NOVA", "GASTA", "FARRAPO"}; return k[std::max(0, std::min(2, i))]; }

struct FlagPreset { const char* name; FlagDesign d; };
static const FlagPreset* flagPresets(int& n) {
    //                                     fundo detalhe figura padrão ponta desgaste fig tam pos esp cont
    static const FlagPreset k[] = {
        {"JOLLY ROGER",  {0, 1, 7, 0, 0, 1, 1, 1, 1, 0, 0}},
        {"SANGUE",       {2, 0, 0, 9, 0, 1, 7, 1, 1, 0, 0}},
        {"REAL",         {11, 5, 5, 15, 1, 0, 3, 1, 1, 0, 1}},
        {"NEBLINA",      {1, 15, 8, 11, 5, 2, 32, 1, 1, 0, 0}},
        {"VENENO",       {10, 0, 6, 5, 3, 1, 25, 1, 1, 0, 1}},
        {"POENTE",       {4, 3, 0, 12, 4, 0, 27, 1, 1, 0, 0}},
        {"CORS\xC1" "RIO",     {3, 5, 5, 9, 0, 1, 6, 1, 1, 0, 0}},
        {"ALMIRANTE",    {11, 8, 8, 27, 1, 0, 8, 1, 1, 0, 0}},
        {"KRAKEN",       {22, 21, 7, 11, 5, 1, 56, 2, 1, 0, 1}},
        {"CEIFADOR",     {1, 2, 7, 0, 5, 2, 55, 2, 1, 0, 0}},
        {"SOL NEGRO",    {6, 0, 0, 22, 3, 0, 2, 1, 1, 0, 0}},
        {"MAR\xC9" " ALTA",    {20, 8, 11, 20, 0, 0, 57, 1, 1, 0, 0}},
        {"TESOURO",      {14, 5, 5, 17, 0, 1, 63, 1, 1, 0, 1}},
        {"SEREIA",       {21, 17, 8, 24, 4, 0, 62, 1, 1, 0, 0}},
        {"DEM\xD4" "NIO",      {0, 3, 2, 30, 1, 2, 69, 2, 1, 0, 1}},
        {"FAROL",        {30, 6, 6, 28, 0, 1, 72, 1, 1, 0, 0}},
    };
    n = (int)(sizeof k / sizeof k[0]); return k;
}

bool flagOpen = false;
FlagDesign flagSaved, flagEdit;   // salva (menu.cfg) e a que está sendo editada
int flagTab = 0, flagTarget = 0;  // aba (0 CORES, 1 PADRÃO, 2 FIGURA, 3 ESTILO); alvo das cores (0 fundo, 1 detalhe, 2 figura)
float flagT = 0.0f;               // tempo da ondulação
std::string flagMsg; float flagMsgT = 0.0f;
bool flagConfirmExit = false;     // 2º toque no X sai sem salvar
uint32_t flagRng = 0x9E3779B9u;
// rolagem: 0 = figuras (vertical), 1 = padrões (vertical), 2 = paleta (horizontal), 3 = esquemas (horizontal)
float flagScroll[4] = {0, 0, 0, 0}, flagVel[4] = {0, 0, 0, 0};
int flagPtr = -1, flagArea = -1;
float flagDownX = 0, flagDownY = 0, flagScroll0 = 0, flagLastP = 0;
double flagLastT = 0;
bool flagMoved = false;

static float flagLum(int c) { int n; const FlagColor* C = flagColors(n); c = std::max(0, std::min(n - 1, c)); return C[c].r * 0.30f + C[c].g * 0.59f + C[c].b * 0.11f; }
FlagLook flagLook(const FlagDesign& d) const {
    int nc, nf; const FlagColor* C = flagColors(nc); const FlagFig* F = flagFigs(nf);
    auto col = [&](int i) { const FlagColor& c = C[std::max(0, std::min(nc - 1, i))]; return Vec3{c.r, c.g, c.b}; };
    FlagLook L;
    L.c0 = col(d.bg); L.c1 = col(d.detail); L.c2 = col(d.figCol);
    L.pattern = d.pattern; L.edge = d.edge; L.wear = d.wear;
    L.icon = F[std::max(0, std::min(nf - 1, (int)d.fig))].icon;
    static const float kSize[3] = {0.20f, 0.29f, 0.38f}, kPos[3] = {0.33f, 0.50f, 0.64f};
    L.iconSize = kSize[std::min(2, (int)d.size)];
    L.iconX = kPos[std::min(2, (int)d.pos)] * (d.edge == 2 ? 0.82f : 1.0f);   // na flâmula o pano afina: a figura vai um pouco para o mastro
    L.mirror = d.mirror != 0; L.outline = d.outline != 0;
    L.seed = 3.7f;
    return L;
}
static bool flagValid(const FlagDesign& d) {
    int nc, nf; flagColors(nc); flagFigs(nf);
    return d.bg < nc && d.detail < nc && d.figCol < nc && d.pattern < kFlagPatterns && d.edge < kFlagEdges && d.wear < kFlagWears && d.fig < nf && d.size < 3 && d.pos < 3 && d.mirror < 2 && d.outline < 2;
}
std::string flagToCfg(const FlagDesign& d) const {
    char t[64]; std::snprintf(t, sizeof t, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", d.bg, d.detail, d.figCol, d.pattern, d.edge, d.wear, d.fig, d.size, d.pos, d.mirror, d.outline);
    return t;
}
bool flagFromCfg(const std::string& v, FlagDesign& out) const {
    int a[11]; if (std::sscanf(v.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &a[9], &a[10]) != 11) return false;
    FlagDesign d; uint8_t* p = &d.bg;
    for (int i = 0; i < 11; ++i) { if (a[i] < 0 || a[i] > 255) return false; p[i] = (uint8_t)a[i]; }
    if (!flagValid(d)) return false;
    out = d; return true;
}

int flagRand(int n) { flagRng = flagRng * 1664525u + 1013904223u; return (int)((flagRng >> 8) % (uint32_t)std::max(1, n)); }
void flagRandomize() {
    int nc, nf; flagColors(nc); flagFigs(nf);
    FlagDesign d;
    d.bg = (uint8_t)flagRand(nc);
    do d.detail = (uint8_t)flagRand(nc); while (d.detail == d.bg);
    for (int k = 0; k < 40; ++k) { d.figCol = (uint8_t)flagRand(nc); if (std::fabs(flagLum(d.figCol) - flagLum(d.bg)) > 0.35f) break; }
    d.pattern = (uint8_t)(flagRand(10) < 4 ? 0 : flagRand(kFlagPatterns));
    d.edge = (uint8_t)(flagRand(10) < 5 ? 0 : flagRand(kFlagEdges));
    d.wear = (uint8_t)flagRand(kFlagWears);
    d.fig = (uint8_t)(1 + flagRand(nf - 1));
    d.size = (uint8_t)flagRand(3); d.pos = (uint8_t)(d.edge == 2 ? flagRand(2) : flagRand(3));
    d.mirror = (uint8_t)flagRand(2); d.outline = (uint8_t)flagRand(2);
    flagEdit = d;
}
void flagSay(const char* m, float t = 2.2f) { flagMsg = m; flagMsgT = t; }
void openFlag() {
    if (flagOpen || selfDead) return;
    flagOpen = true; flagEdit = flagSaved; flagConfirmExit = false; flagMsgT = 0.0f; flagPtr = -1;
    for (float& v : flagVel) v = 0.0f;
    flagRng ^= (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    wheelOpen = false; joyT.id = -1; lookT.id = -1;
}
void closeFlag() {
    flagOpen = false; flagConfirmExit = false; flagPtr = -1;
    if (flagMode == 1) { flagSaved = flagOwnBackup; flagMode = 0; }   // editava a bandeira da TRIPULAÇÃO: volta a do jogador
    flagEdit = flagSaved;
}
void flagTryClose() {
    if (flagEdit != flagSaved && !flagConfirmExit) { flagConfirmExit = true; flagSay("N\xC3" "O SALVOU. TOQUE NO X DE NOVO PARA SAIR SEM SALVAR", 3.0f); return; }
    closeFlag();
}

// ---------- layout (o mesmo para desenhar e para os toques) ----------
struct FlagGeo {
    float u;
    Rect2D left, panel, content, btn[3], tabs[4];
    float closeX, closeY, closeR;
    float poleX, poleTop, poleBot, fcx, fcy, fhw, fhh;
};
FlagGeo flagGeo() const {
    FlagGeo G; G.u = std::min((float)h / 1080.0f, (float)w / 1900.0f); const float u = G.u;
    const float top = 150.0f * u, bot = (float)h - 40.0f * u;
    G.closeR = 44.0f * u; G.closeX = (float)w - 80.0f * u; G.closeY = 75.0f * u;
    G.panel = {(float)w * 0.54f, top, (float)w - 40.0f * u - (float)w * 0.54f, bot - top};
    G.left = {40.0f * u, top, G.panel.x - 80.0f * u, bot - top};
    const float bh = 92.0f * u, bw = (G.left.w - 40.0f * u) / 3.0f;
    for (int i = 0; i < 3; ++i) G.btn[i] = {G.left.x + (float)i * (bw + 20.0f * u), bot - bh, bw, bh};
    const float prevBot = bot - bh - 30.0f * u;
    G.poleX = G.left.x; G.poleTop = top; G.poleBot = prevBot;
    const float availW = G.left.w - 60.0f * u, availH = prevBot - top - 80.0f * u;
    G.fhw = std::min(availW * 0.5f, availH * 0.5f * 1.5f); G.fhh = G.fhw / 1.5f;
    G.fcx = G.left.x + G.left.w * 0.5f; G.fcy = (top + prevBot) * 0.5f;
    const float tw = (G.panel.w - 44.0f * u - 3.0f * 12.0f * u) / 4.0f;
    for (int i = 0; i < 4; ++i) G.tabs[i] = {G.panel.x + 22.0f * u + (float)i * (tw + 12.0f * u), G.panel.y + 22.0f * u, tw, 72.0f * u};
    G.content = {G.panel.x + 28.0f * u, G.panel.y + 120.0f * u, G.panel.w - 56.0f * u, G.panel.h - 140.0f * u};
    return G;
}
// CORES: alvos fixos; paleta (2 linhas) e esquemas rolam na horizontal
Rect2D flagTargetRect(const FlagGeo& G, int i) const { const float u = G.u, tw = (G.content.w - 32.0f * u) / 3.0f; return {G.content.x + (float)i * (tw + 16.0f * u), G.content.y, tw, 104.0f * u}; }
float flagSwatchS(const FlagGeo& G) const { return 104.0f * G.u; }
Rect2D flagPaletteArea(const FlagGeo& G) const { const float s = flagSwatchS(G); return {G.content.x, G.content.y + 174.0f * G.u, G.content.w, 2.0f * s + 14.0f * G.u}; }
Rect2D flagSwatchRect(const FlagGeo& G, int i) const {
    const float u = G.u, s = flagSwatchS(G); const Rect2D a = flagPaletteArea(G); int nc; flagColors(nc); const int cols = (nc + 1) / 2;
    return {a.x + (float)(i % cols) * (s + 14.0f * u) - flagScroll[2], a.y + (float)(i / cols) * (s + 14.0f * u), s, s};
}
Rect2D flagPresetArea(const FlagGeo& G) const { const Rect2D p = flagPaletteArea(G); return {G.content.x, p.y + p.h + 86.0f * G.u, G.content.w, 150.0f * G.u}; }
float flagPresetW(const FlagGeo& G) const { return 165.0f * G.u; }
Rect2D flagPresetRect(const FlagGeo& G, int i) const { const Rect2D a = flagPresetArea(G); return {a.x + (float)i * (flagPresetW(G) + 14.0f * G.u) - flagScroll[3], a.y, flagPresetW(G), a.h}; }
// PADRÃO (4 colunas) e FIGURA (6 colunas): rolam na vertical dentro da área de conteúdo
Rect2D flagPatternRect(const FlagGeo& G, int i) const {
    const float u = G.u, cw = (G.content.w - 3.0f * 16.0f * u) / 4.0f, ch = 175.0f * u;
    return {G.content.x + (float)(i % 4) * (cw + 16.0f * u), G.content.y + (float)(i / 4) * (ch + 16.0f * u) - flagScroll[1], cw, ch};
}
Rect2D flagFigRect(const FlagGeo& G, int i) const {
    const float u = G.u, cw = (G.content.w - 5.0f * 12.0f * u) / 6.0f, ch = 118.0f * u;
    return {G.content.x + (float)(i % 6) * (cw + 12.0f * u), G.content.y + (float)(i / 6) * (ch + 12.0f * u) - flagScroll[0], cw, ch};
}
// ESTILO
Rect2D flagEdgeRect(const FlagGeo& G, int i) const { const float u = G.u, tw = (G.content.w - 5.0f * 12.0f * u) / 6.0f; return {G.content.x + (float)i * (tw + 12.0f * u), G.content.y + 44.0f * u, tw, 140.0f * u}; }
Rect2D flagWearRect(const FlagGeo& G, int i) const { const float u = G.u, tw = (G.content.w - 5.0f * 12.0f * u) / 6.0f * 1.6f; return {G.content.x + (float)i * (tw + 12.0f * u), G.content.y + 244.0f * u, tw, 140.0f * u}; }
Rect2D flagSegRect(const FlagGeo& G, int row, int i) const {   // row 0 = TAMANHO, 1 = POSIÇÃO; 3 botões
    const float u = G.u, lw = 230.0f * u, sw = (G.content.w - lw - 2.0f * 10.0f * u) / 3.0f;
    return {G.content.x + lw + (float)i * (sw + 10.0f * u), G.content.y + 450.0f * u + (float)row * 86.0f * u, sw, 70.0f * u};
}
Rect2D flagToggleRect(const FlagGeo& G, int i) const { const float u = G.u, tw = (G.content.w - 14.0f * u) / 2.0f; return {G.content.x + (float)i * (tw + 14.0f * u), G.content.y + 636.0f * u, tw, 70.0f * u}; }

// área de rolagem k (retângulo visível, vertical?, rolagem máxima); false se não está na aba atual
bool flagScrollArea(const FlagGeo& G, int k, Rect2D& r, bool& vert, float& maxS) const {
    const float u = G.u;
    if (k == 0 && flagTab == 2) { int nf; flagFigs(nf); r = G.content; vert = true; const int rows = (nf + 5) / 6; maxS = std::max(0.0f, (float)rows * 130.0f * u - 12.0f * u - r.h); return true; }
    if (k == 1 && flagTab == 1) { r = G.content; vert = true; const int rows = (kFlagPatterns + 3) / 4; maxS = std::max(0.0f, (float)rows * 191.0f * u - 16.0f * u - r.h); return true; }
    if (k == 2 && flagTab == 0) { int nc; flagColors(nc); r = flagPaletteArea(G); vert = false; const int cols = (nc + 1) / 2; maxS = std::max(0.0f, (float)cols * (flagSwatchS(G) + 14.0f * u) - 14.0f * u - r.w); return true; }
    if (k == 3 && flagTab == 0) { int np; flagPresets(np); r = flagPresetArea(G); vert = false; maxS = std::max(0.0f, (float)np * (flagPresetW(G) + 14.0f * u) - 14.0f * u - r.w); return true; }
    return false;
}
void updateFlag(float dt) {
    if (!flagOpen) return;
    flagT += dt;
    if (flagMsgT > 0.0f) { flagMsgT -= dt; if (flagMsgT <= 0.0f) flagConfirmExit = false; }
    if (selfDead) { closeFlag(); return; }
    const FlagGeo G = flagGeo();
    for (int k = 0; k < 4; ++k) {   // inércia depois de soltar o dedo; volta para dentro dos limites
        Rect2D r; bool vert; float maxS;
        if (!flagScrollArea(G, k, r, vert, maxS)) continue;
        if (k == flagArea && flagPtr >= 0) continue;   // o dedo está arrastando esta lista
        flagScroll[k] += flagVel[k] * dt; flagVel[k] *= std::exp(-4.5f * dt);
        if (std::fabs(flagVel[k]) < 5.0f) flagVel[k] = 0.0f;
        if (flagScroll[k] < 0.0f) { flagScroll[k] *= std::exp(-14.0f * dt); flagVel[k] = 0.0f; if (flagScroll[k] > -0.5f) flagScroll[k] = 0.0f; }
        if (flagScroll[k] > maxS) { flagScroll[k] = maxS + (flagScroll[k] - maxS) * std::exp(-14.0f * dt); flagVel[k] = 0.0f; if (flagScroll[k] - maxS < 0.5f) flagScroll[k] = maxS; }
    }
}

// ---------- toques ----------
static double flagNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// toque (sem arrastar) dentro de uma lista que rola: escolhe o item
void flagTapArea(const FlagGeo& G, int k, float x, float y) {
    FlagDesign& d = flagEdit;
    if (k == 0) { int nf; const FlagFig* F = flagFigs(nf); for (int i = 0; i < nf; ++i) if (flagFigRect(G, i).contains(x, y)) { d.fig = (uint8_t)i; flagSay(F[i].name, 1.4f); return; } }
    else if (k == 1) { for (int i = 0; i < kFlagPatterns; ++i) if (flagPatternRect(G, i).contains(x, y)) { d.pattern = (uint8_t)i; return; } }
    else if (k == 2) {
        int nc; flagColors(nc);
        for (int i = 0; i < nc; ++i) if (flagSwatchRect(G, i).contains(x, y)) { uint8_t* t[3] = {&d.bg, &d.detail, &d.figCol}; *t[flagTarget] = (uint8_t)i; return; }
    } else if (k == 3) {
        int np; const FlagPreset* P = flagPresets(np);
        for (int i = 0; i < np; ++i) if (flagPresetRect(G, i).contains(x, y)) { d = P[i].d; char t[48]; std::snprintf(t, sizeof t, "ESQUEMA %s", P[i].name); flagSay(t); return; }
    }
}
void flagTouchDown(int pid, float x, float y) {
    const FlagGeo G = flagGeo();
    {   const float dx = x - G.closeX, dy = y - G.closeY; if (dx * dx + dy * dy <= G.closeR * G.closeR * 1.4f) { flagTryClose(); return; } }
    flagConfirmExit = false;
    for (int k = 0; k < 4; ++k) {   // listas que rolam: decide ao soltar (tocar = escolher, arrastar = rolar)
        Rect2D r; bool vert; float maxS;
        if (flagScrollArea(G, k, r, vert, maxS) && r.contains(x, y)) {
            if (flagPtr >= 0) return;
            flagPtr = pid; flagArea = k; flagDownX = x; flagDownY = y; flagScroll0 = flagScroll[k]; flagMoved = std::fabs(flagVel[k]) > 200.0f;   // lista ainda deslizando: o toque só a segura
            flagVel[k] = 0.0f; flagLastP = vert ? y : x; flagLastT = flagNow();
            return;
        }
    }
    if (G.btn[0].contains(x, y)) { flagRandomize(); flagSay("BANDEIRA ALEAT\xD3RIA"); return; }
    if (G.btn[1].contains(x, y)) { if (flagEdit != flagSaved) { flagEdit = flagSaved; flagSay("VOLTOU PARA A \xDALTIMA SALVA"); } else flagSay("NADA PARA DESFAZER"); return; }
    if (G.btn[2].contains(x, y)) {
        if (flagEdit == flagSaved) { flagSay("J\xC1 EST\xC1 SALVA"); return; }
        flagSaved = flagEdit; if (flagMode == 1) teamFlagSaved(); else flagSaved_changed(); flagSay(flagMode == 1 ? "BANDEIRA DA TRIPULA\xC7\xC3" "O SALVA" : "BANDEIRA SALVA"); return;
    }
    for (int i = 0; i < 4; ++i) if (G.tabs[i].contains(x, y)) { flagTab = i; return; }
    FlagDesign& d = flagEdit;
    if (flagTab == 0) {
        for (int i = 0; i < 3; ++i) if (flagTargetRect(G, i).contains(x, y)) { flagTarget = i; return; }
    } else if (flagTab == 3) {
        for (int i = 0; i < kFlagEdges; ++i) if (flagEdgeRect(G, i).contains(x, y)) { d.edge = (uint8_t)i; return; }
        for (int i = 0; i < kFlagWears; ++i) if (flagWearRect(G, i).contains(x, y)) { d.wear = (uint8_t)i; return; }
        for (int r = 0; r < 2; ++r) for (int i = 0; i < 3; ++i) if (flagSegRect(G, r, i).contains(x, y)) { (r == 0 ? d.size : d.pos) = (uint8_t)i; return; }
        if (flagToggleRect(G, 0).contains(x, y)) { d.mirror ^= 1; return; }
        if (flagToggleRect(G, 1).contains(x, y)) { d.outline ^= 1; return; }
    }
}
void flagTouchMove(int pid, float x, float y) {
    if (pid != flagPtr || flagArea < 0) return;
    const FlagGeo G = flagGeo();
    Rect2D r; bool vert; float maxS;
    if (!flagScrollArea(G, flagArea, r, vert, maxS)) return;
    const float p = vert ? y : x, delta = p - (vert ? flagDownY : flagDownX);
    if (!flagMoved && std::fabs(delta) > 14.0f * G.u) flagMoved = true;
    if (!flagMoved) return;
    float sc = flagScroll0 - delta;
    if (sc < 0.0f) sc *= 0.35f; else if (sc > maxS) sc = maxS + (sc - maxS) * 0.35f;   // passa um pouco da ponta (elástico)
    flagScroll[flagArea] = sc;
    const double t = flagNow(); const float dtt = (float)std::max(1e-3, t - flagLastT);
    flagVel[flagArea] = flagVel[flagArea] * 0.4f + 0.6f * (-(p - flagLastP) / dtt);
    flagLastP = p; flagLastT = t;
}
void flagTouchUp(int pid, float x, float y, bool cancel) {
    if (pid != flagPtr) return;
    const int k = flagArea; flagPtr = -1; flagArea = -1;
    if (k < 0) return;
    if (!flagMoved) { flagVel[k] = 0.0f; if (!cancel) flagTapArea(flagGeo(), k, x, y); }
    else if (flagNow() - flagLastT > 0.08) flagVel[k] = 0.0f;   // parou o dedo antes de soltar: sem inércia
}

// ---------- desenho ----------
void flagCard(const Rect2D& r, float u, bool sel, float alpha = 1.0f) {   // cartão rústico couro/carvalho + borda latão se selecionado
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    renderer.uiWoodPanel(cx, cy, r.w * 0.5f, r.h * 0.5f, 12.0f * u, sel ? theme::PanelCardSel : theme::PanelCard, alpha, r.x * 0.01f + r.y * 0.01f);
}
void flagGoldBtn(const Rect2D& r, float u, bool on) {
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    renderer.uiButton(cx, cy, r.w * 0.5f, r.h * 0.5f, 12.0f * u, on ? theme::BtnBrass : theme::BtnDisabled, 1.0f);
}
void flagMini(const Rect2D& r, float u, const FlagDesign& fd, float labelH) {   // bandeirinha parada centrada no cartão (acima do rótulo)
    const float aw = r.w - 30.0f * u, ah = r.h - labelH - 24.0f * u;
    const float hw = std::min(aw * 0.5f, ah * 0.5f * 1.5f), hh = hw / 1.5f;
    FlagLook L = flagLook(fd); L.iconSize = std::min(L.iconSize, 0.26f);
    renderer.uiFlag(L, r.x + r.w * 0.5f, r.y + 12.0f * u + ah * 0.5f, hw, hh, 0.0f, 0.0f);
}
static bool flagVisible(const Rect2D& r, const Rect2D& area) { return r.x + r.w > area.x && r.x < area.x + area.w && r.y + r.h > area.y && r.y < area.y + area.h; }
void drawFlagShapes(const FlagGeo& G) {   // tudo o que NÃO rola
    const float u = G.u;
    const FlagDesign& d = flagEdit;
    // fundo: carvalho noturno quente + brilho de lampião atrás da bandeira
    renderer.uiWoodPanel((float)w * 0.5f, (float)h * 0.5f, (float)w * 0.5f, (float)h * 0.5f, 0.0f, theme::PanelBgWood, 1.0f, 5.0f);
    renderer.uiGlow(G.fcx, G.fcy, G.fhw * 1.7f, G.fhh * 2.0f, 0.92f, 0.62f, 0.26f, 0.11f);
    renderer.uiGlow(G.fcx, G.fcy + G.fhh * 0.25f, G.fhw * 1.2f, G.fhh * 1.3f, 0.96f, 0.80f, 0.45f, 0.05f);
    renderer.uiIconTint(HiPirateFlag, 74.0f * u, 68.0f * u, 36.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    renderer.uiDivider(420.0f * u, 130.0f * u, 290.0f * u, 6.0f * u, 0.72f);
    closeBtnShapes(G.closeX, G.closeY, G.closeR);
    renderer.uiFlag(flagLook(d), G.fcx, G.fcy, G.fhw, G.fhh, flagT, 1.0f);   // A BANDEIRA (sem mastro, centralizada)
    renderer.uiButton(G.btn[0].x + G.btn[0].w * 0.5f, G.btn[0].y + G.btn[0].h * 0.5f, G.btn[0].w * 0.5f, G.btn[0].h * 0.5f, 12.0f * u, theme::BtnWood, 1.0f);
    renderer.uiButton(G.btn[1].x + G.btn[1].w * 0.5f, G.btn[1].y + G.btn[1].h * 0.5f, G.btn[1].w * 0.5f, G.btn[1].h * 0.5f, 12.0f * u, theme::BtnWood, 1.0f);
    flagGoldBtn(G.btn[2], u, flagEdit != flagSaved);
    // painel principal em carvalho escuro + latão + rebites e abas táteis
    const Rect2D& P = G.panel;
    renderer.uiWoodPanel(P.x + P.w * 0.5f, P.y + P.h * 0.5f, P.w * 0.5f, P.h * 0.5f, 20.0f * u, theme::PanelWood, 0.98f, 9.0f);
    for (int i = 0; i < 4; ++i) {
        const Rect2D& r = G.tabs[i];
        renderer.uiButton(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 12.0f * u, i == flagTab ? theme::BtnBrass : theme::BtnWood, 1.0f);
    }
    int nc; const FlagColor* C = flagColors(nc);
    if (flagTab == 0) {   // CORES: os 3 alvos
        const uint8_t cur[3] = {d.bg, d.detail, d.figCol};
        for (int i = 0; i < 3; ++i) {
            const Rect2D r = flagTargetRect(G, i);
            flagCard(r, u, i == flagTarget);
            const FlagColor& c = C[cur[i]];
            renderer.uiRoundRect(r.x + r.w - 52.0f * u, r.y + r.h * 0.5f, 30.0f * u, 30.0f * u, 10.0f * u, c.r, c.g, c.b, 1.0f);
            renderer.uiRoundRectBorder(r.x + r.w - 52.0f * u, r.y + r.h * 0.5f, 30.0f * u, 30.0f * u, 10.0f * u, 2.0f * u, 1.0f, 1.0f, 1.0f, 0.35f);
        }
    } else if (flagTab == 3) {   // ESTILO
        for (int i = 0; i < kFlagEdges; ++i) {
            const Rect2D r = flagEdgeRect(G, i); flagCard(r, u, d.edge == i);
            FlagDesign fd = d; fd.edge = (uint8_t)i; fd.wear = 0; flagMini(r, u, fd, 32.0f * u);
        }
        for (int i = 0; i < kFlagWears; ++i) {
            const Rect2D r = flagWearRect(G, i); flagCard(r, u, d.wear == i);
            FlagDesign fd = d; fd.wear = (uint8_t)i; flagMini(r, u, fd, 32.0f * u);
        }
        for (int row = 0; row < 2; ++row) for (int i = 0; i < 3; ++i) flagGoldBtn(flagSegRect(G, row, i), u, (row == 0 ? d.size : d.pos) == i);
        for (int i = 0; i < 2; ++i) flagGoldBtn(flagToggleRect(G, i), u, i == 0 ? d.mirror != 0 : d.outline != 0);
    }
}
void drawFlagAreaShapes(const FlagGeo& G, int k, const Rect2D& area) {   // conteúdo de uma lista que rola (desenhado com recorte)
    const float u = G.u;
    const FlagDesign& d = flagEdit;
    int nc; const FlagColor* C = flagColors(nc);
    if (k == 2) {   // paleta
        const uint8_t cur[3] = {d.bg, d.detail, d.figCol};
        for (int i = 0; i < nc; ++i) {
            const Rect2D r = flagSwatchRect(G, i);
            if (!flagVisible(r, area)) continue;
            const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, rr = 16.0f * u;
            renderer.uiRoundRect(cx + 3.0f * u, cy + 4.0f * u, r.w * 0.5f - 3.0f * u, r.h * 0.5f - 4.0f * u, rr, 0.0f, 0.0f, 0.0f, 0.4f);
            renderer.uiGrad(cx, cy, r.w * 0.5f - 3.0f * u, r.h * 0.5f - 3.0f * u, rr, C[i].r * 1.12f, C[i].g * 1.12f, C[i].b * 1.12f, 1.0f, C[i].r * 0.82f, C[i].g * 0.82f, C[i].b * 0.82f, 1.0f, 0.3f, 1.0f, 0.15f, (float)i);
            const bool sel = cur[flagTarget] == i;
            renderer.uiRoundRectBorder(cx, cy, r.w * 0.5f - 3.0f * u, r.h * 0.5f - 3.0f * u, rr, sel ? 5.0f * u : 1.5f * u, 1.0f, sel ? 0.80f : 1.0f, sel ? 0.25f : 1.0f, sel ? 1.0f : 0.15f);
            if (sel) { const float l = flagLum(i) > 0.5f ? 0.05f : 1.0f; renderer.uiShape(Renderer::UiFill, cx, cy, 15.0f * u, l, l, l, 0.85f); }
        }
    } else if (k == 3) {   // esquemas prontos
        int np; const FlagPreset* PR = flagPresets(np);
        for (int i = 0; i < np; ++i) { const Rect2D r = flagPresetRect(G, i); if (!flagVisible(r, area)) continue; flagCard(r, u, PR[i].d == d); flagMini(r, u, PR[i].d, 34.0f * u); }
    } else if (k == 1) {   // padrões
        for (int i = 0; i < kFlagPatterns; ++i) {
            const Rect2D r = flagPatternRect(G, i);
            if (!flagVisible(r, area)) continue;
            flagCard(r, u, d.pattern == i);
            FlagDesign fd = d; fd.pattern = (uint8_t)i; fd.wear = 0; fd.edge = 0;
            flagMini(r, u, fd, 36.0f * u);
        }
    } else if (k == 0) {   // figuras
        int nf; const FlagFig* F = flagFigs(nf);
        const FlagColor& bc = C[d.bg]; const FlagColor& fc = C[d.figCol];
        for (int i = 0; i < nf; ++i) {
            const Rect2D r = flagFigRect(G, i);
            if (!flagVisible(r, area)) continue;
            const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
            renderer.uiGrad(cx, cy, r.w * 0.5f, r.h * 0.5f, 14.0f * u, bc.r * 1.15f + 0.04f, bc.g * 1.15f + 0.04f, bc.b * 1.15f + 0.04f, 1.0f, bc.r * 0.7f, bc.g * 0.7f, bc.b * 0.7f, 1.0f, 0.0f, 1.0f, 0.30f, (float)i * 3.0f);
            const bool sel = d.fig == i;
            renderer.uiRoundRectBorder(cx, cy, r.w * 0.5f - 2.0f * u, r.h * 0.5f - 2.0f * u, 14.0f * u, sel ? 5.0f * u : 1.5f * u, 1.0f, sel ? 0.80f : 1.0f, sel ? 0.25f : 1.0f, sel ? 1.0f : 0.14f);
            if (F[i].icon >= 0) renderer.uiIconTint(F[i].icon, cx, cy, std::min(r.w, r.h) * 0.40f, fc.r, fc.g, fc.b, 1.0f);
            else renderer.uiShape(Renderer::UiRing, cx, cy, std::min(r.w, r.h) * 0.26f, fc.r, fc.g, fc.b, 0.55f);
        }
    }
}
void drawFlagAreaText(const FlagGeo& G, int k, const Rect2D& area) {
    const float u = G.u; const int t3 = std::max(2, (int)std::lround(3.0f * u));
    auto center = [&](const Rect2D& r, float y, const char* s, bool on) {
        int t = t3; while (t > 2 && Renderer::textWidth(s, t) > r.w - 16.0f * u) --t;
        renderer.text(r.x + (r.w - Renderer::textWidth(s, t)) * 0.5f, y - 3.5f * (float)t, t, s, on ? 1.0f : 0.90f, on ? 0.82f : 0.86f, on ? 0.35f : 0.78f, 1.0f, false);
    };
    if (k == 3) {
        int np; const FlagPreset* PR = flagPresets(np);
        for (int i = 0; i < np; ++i) { const Rect2D r = flagPresetRect(G, i); if (flagVisible(r, area)) center(r, r.y + r.h - 22.0f * u, PR[i].name, PR[i].d == flagEdit); }
    } else if (k == 1) {
        for (int i = 0; i < kFlagPatterns; ++i) { const Rect2D r = flagPatternRect(G, i); if (flagVisible(r, area)) center(r, r.y + r.h - 22.0f * u, flagPatternName(i), flagEdit.pattern == i); }
    }
}
void drawFlagText(const FlagGeo& G) {   // textos fixos
    const float u = G.u;
    const FlagDesign& d = flagEdit;
    int nc; const FlagColor* C = flagColors(nc);
    auto center = [&](const Rect2D& r, float y, int ts, const char* s, float cr, float cg, float cb) {
        int t = ts; while (t > 2 && Renderer::textWidth(s, t) > r.w - 16.0f * u) --t;
        renderer.text(r.x + (r.w - Renderer::textWidth(s, t)) * 0.5f, y - 3.5f * (float)t, t, s, cr, cg, cb, 1.0f, false);
    };
    const int t6 = std::max(3, (int)std::lround(6.0f * u)), t5 = std::max(2, (int)std::lround(5.0f * u)), t4 = std::max(2, (int)std::lround(4.0f * u)), t3 = std::max(2, (int)std::lround(3.0f * u));
    shopBold(130.0f * u, 30.0f * u, t6, flagMode == 1 ? "BANDEIRA DA TRIPULA\xC7\xC3" "O" : "BANDEIRA", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    renderer.text(130.0f * u, 30.0f * u + 7.0f * (float)t6 + 18.0f * u, t3, flagMode == 1 ? "A BANDEIRA QUE TODA A TRIPULA\xC7\xC3" "O VAI USAR" : "CRIE A BANDEIRA DO SEU NAVIO", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);
    {
        const char* lb[3] = {"ALEAT\xD3RIA", "DESFAZER", d == flagSaved ? "SALVA" : "SALVAR"};
        for (int i = 0; i < 3; ++i) {
            const bool gold = i == 2 && d != flagSaved;
            const Rect2D& r = G.btn[i];
            int t = t5; while (t > 2 && Renderer::textWidth(lb[i], t) > r.w - 30.0f * u) --t;
            shopBold(r.x + (r.w - Renderer::textWidth(lb[i], t)) * 0.5f, r.y + (r.h - 7.0f * (float)t) * 0.5f, t, lb[i], gold ? 0.16f : 0.95f, gold ? 0.10f : 0.88f, gold ? 0.02f : 0.72f, (i == 2 && !gold) ? 0.55f : 1.0f, false);
        }
    }
    {
        const char* tn[4] = {"CORES", "PADR\xC3" "O", "FIGURA", "ESTILO"};
        for (int i = 0; i < 4; ++i) {
            const Rect2D& r = G.tabs[i]; const bool on = i == flagTab;
            int t = t5; while (t > 2 && Renderer::textWidth(tn[i], t) > r.w - 24.0f * u) --t;
            shopBold(r.x + (r.w - Renderer::textWidth(tn[i], t)) * 0.5f, r.y + (r.h - 7.0f * (float)t) * 0.5f, t, tn[i], on ? 0.16f : 0.92f, on ? 0.10f : 0.86f, on ? 0.02f : 0.74f, 1.0f, false);
        }
    }
    const Rect2D& c = G.content;
    if (flagTab == 0) {
        const char* tn[3] = {"FUNDO", "DETALHE", "FIGURA"};
        const uint8_t cur[3] = {d.bg, d.detail, d.figCol};
        for (int i = 0; i < 3; ++i) {
            const Rect2D r = flagTargetRect(G, i); const bool on = i == flagTarget;
            renderer.text(r.x + 24.0f * u, r.y + 26.0f * u, t4, tn[i], on ? 1.0f : 0.85f, 0.82f, on ? 0.35f : 0.75f, 1.0f, false);
            renderer.text(r.x + 24.0f * u, r.y + 64.0f * u, t3, C[cur[i]].name, 0.75f, 0.72f, 0.66f, 1.0f, false);
        }
        char pl[48]; std::snprintf(pl, sizeof pl, "PALETA: COR D%s %s", flagTarget == 2 ? "A" : "O", tn[flagTarget]);
        renderer.text(c.x, c.y + 128.0f * u, t4, pl, 0.95f, 0.80f, 0.40f, 1.0f, false);
        const Rect2D pa = flagPresetArea(G);
        renderer.text(c.x, pa.y - 50.0f * u, t4, "ESQUEMAS PRONTOS", 0.95f, 0.80f, 0.40f, 1.0f, false);
    } else if (flagTab == 3) {
        renderer.text(c.x, c.y + 6.0f * u, t4, "PONTA", 0.95f, 0.80f, 0.40f, 1.0f, false);
        for (int i = 0; i < kFlagEdges; ++i) { const Rect2D r = flagEdgeRect(G, i); center(r, r.y + r.h - 20.0f * u, t3, flagEdgeName(i), d.edge == i ? 1.0f : 0.88f, d.edge == i ? 0.82f : 0.85f, d.edge == i ? 0.35f : 0.78f); }
        renderer.text(c.x, c.y + 206.0f * u, t4, "DESGASTE", 0.95f, 0.80f, 0.40f, 1.0f, false);
        for (int i = 0; i < kFlagWears; ++i) { const Rect2D r = flagWearRect(G, i); center(r, r.y + r.h - 20.0f * u, t3, flagWearName(i), d.wear == i ? 1.0f : 0.88f, d.wear == i ? 0.82f : 0.85f, d.wear == i ? 0.35f : 0.78f); }
        renderer.text(c.x, c.y + 410.0f * u, t4, "FIGURA", 0.95f, 0.80f, 0.40f, 1.0f, false);
        const char* rowN[2] = {"TAMANHO", "POSI\xC7\xC3" "O"};
        const char* segN[2][3] = {{"PEQUENA", "M\xC9" "DIA", "GRANDE"}, {"MASTRO", "CENTRO", "PONTA"}};
        for (int row = 0; row < 2; ++row) {
            const Rect2D r0 = flagSegRect(G, row, 0);
            renderer.text(c.x, r0.y + r0.h * 0.5f - 3.5f * (float)t4, t4, rowN[row], 0.85f, 0.82f, 0.75f, 1.0f, false);
            for (int i = 0; i < 3; ++i) {
                const Rect2D r = flagSegRect(G, row, i); const bool on = (row == 0 ? d.size : d.pos) == i;
                center(r, r.y + r.h * 0.5f, t4, segN[row][i], on ? 0.16f : 0.90f, on ? 0.10f : 0.86f, on ? 0.02f : 0.76f);
            }
        }
        const char* tg[2] = {"ESPELHAR", "CONTORNO"};
        for (int i = 0; i < 2; ++i) {
            const Rect2D r = flagToggleRect(G, i); const bool on = i == 0 ? d.mirror != 0 : d.outline != 0;
            char t[40]; std::snprintf(t, sizeof t, "%s: %s", tg[i], on ? "SIM" : "N\xC3" "O");
            center(r, r.y + r.h * 0.5f, t4, t, on ? 0.16f : 0.90f, on ? 0.10f : 0.86f, on ? 0.02f : 0.76f);
        }
    }
}
// aviso: pílula no topo, sobre a faixa do painel (entre o título e o X)
Rect2D flagToastRect(const FlagGeo& G) const { const float u = G.u; return {G.panel.x, G.closeY - 34.0f * u, G.closeX - G.closeR - 24.0f * u - G.panel.x, 68.0f * u}; }
void drawFlagScreen() {
    const FlagGeo G = flagGeo();
    const float u = G.u;
    renderer.beginUi(); drawFlagShapes(G); renderer.endUi();
    renderer.beginText(); drawFlagText(G); renderer.endText();
    for (int k = 0; k < 4; ++k) {   // listas que rolam: conteúdo recortado na área + barra de rolagem
        Rect2D a; bool vert; float maxS;
        if (!flagScrollArea(G, k, a, vert, maxS)) continue;
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)a.x, (GLint)((float)h - a.y - a.h), (GLsizei)a.w, (GLsizei)a.h);
        renderer.beginUi(); drawFlagAreaShapes(G, k, a); renderer.endUi();
        renderer.beginText(); drawFlagAreaText(G, k, a); renderer.endText();
        glDisable(GL_SCISSOR_TEST);
        if (maxS > 1.0f) {
            renderer.beginUi();
            const float len = vert ? a.h : a.w, view = len / (len + maxS), pos = std::max(0.0f, std::min(1.0f, flagScroll[k] / maxS));
            const float bl = std::max(40.0f * u, len * view), off = (len - bl) * pos;
            if (vert) {
                const float bx = G.panel.x + G.panel.w - 13.0f * u;
                renderer.uiRoundRect(bx, a.y + a.h * 0.5f, 3.0f * u, a.h * 0.5f, 3.0f * u, 1.0f, 1.0f, 1.0f, 0.08f);
                renderer.uiRoundRect(bx, a.y + off + bl * 0.5f, 3.0f * u, bl * 0.5f, 3.0f * u, 1.0f, 0.78f, 0.25f, 0.85f);
            } else {
                const float by = a.y + a.h + 9.0f * u;
                renderer.uiRoundRect(a.x + a.w * 0.5f, by, a.w * 0.5f, 3.0f * u, 3.0f * u, 1.0f, 1.0f, 1.0f, 0.08f);
                renderer.uiRoundRect(a.x + off + bl * 0.5f, by, bl * 0.5f, 3.0f * u, 3.0f * u, 1.0f, 0.78f, 0.25f, 0.85f);
            }
            renderer.endUi();
        }
    }
    if (flagMsgT > 0.0f) {
        const float a = std::min(1.0f, flagMsgT * 3.0f);
        const Rect2D r = flagToastRect(G);
        renderer.beginUi();
        renderer.uiRoundRect(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, r.h * 0.5f, 0.06f, 0.05f, 0.03f, 0.96f * a);
        renderer.uiRoundRectBorder(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, r.h * 0.5f, 2.5f * u, 1.0f, 0.78f, 0.25f, a);
        renderer.endUi();
        renderer.beginText();
        int t = std::max(2, (int)std::lround(5.0f * u)); while (t > 2 && Renderer::textWidth(flagMsg.c_str(), t) > r.w - 50.0f * u) --t;
        shopBold(r.x + (r.w - Renderer::textWidth(flagMsg.c_str(), t)) * 0.5f, r.y + (r.h - 7.0f * (float)t) * 0.5f, t, flagMsg.c_str(), 1.0f, 0.85f, 0.35f, a, false);
        renderer.endText();
    }
}
// botão da HUD (ao lado do CHAT, abaixo do minimapa)
Rect2D flagBtnRect() const { return menuBtnRect(2); }
void drawFlagBtn() {
    const Rect2D r = flagBtnRect();
    hudIcon(HiPirateFlag, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.h * 0.44f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
}
