// ================= LOJA (tela cheia) =================
// Incluído DENTRO de `struct Engine` (engine.cpp). Produtos num array estático (shopItems) e abas em shopTabs.
// - ShopRealMoney (pacotes de MOEDAS em R$): compra pelo GOOGLE PLAY BILLING (`sku` = id do produto na Play Console).
// - ShopAd: MOEDAS GRATIS assistindo um anúncio PREMIADO do AdMob (price = moedas ganhas).
// - Itens com preço em moedas: ainda não fazem nada (aviso "EM BREVE"; não há o que entregar no jogo ainda).
// Monetização: ver engine/src/monetize.h e CLAUDE.md "Monetização".

enum ShopFlag : uint8_t { ShopSoldOut = 1, ShopRealMoney = 2, ShopAd = 4 };   // ESGOTADO; preço em R$ (Google Play); anúncio premiado (price = moedas ganhas)
struct ShopItem {
    const char* sku;       // id estável do produto (backend futuro)
    const char* tag;       // pílula no topo (NOVO, RARO, LENDARIO, NAVIO, VIP...)
    uint8_t tagCol;        // 0 roxo, 1 laranja, 2 cinza, 3 vermelho, 4 verde, 5 azul, 6 dourado
    const char* title;     // título grande (1 palavra curta)
    const char* sub;       // subtítulo menor
    const char* name;      // nome embaixo à esquerda
    int price, oldPrice;   // oldPrice > 0 = promoção (preço antigo riscado). R$: em centavos
    uint8_t flags;
    float r, g, b;         // cor do cartão
    int16_t ic[3];         // arte: ícones do atlas assets/icons.e0i (0..11 itens coloridos; >= 12 ícones da HUD/game-icons.net, pintados com o tom claro da cor do cartão); -1 = nenhum
};
static const ShopItem* shopItems(int& n) {
    static const ShopItem k[] = {
        /* 0*/ {"vip_capitao_30d",  "VIP",       6, "CAPIT\xC3O", "PACOTE VIP 30 DIAS",   "PACOTE CAPIT\xC3O (30D)",  49990, 80000, 0, 1.00f, 0.55f, 0.10f, {50, 9, 8}},
        /* 1*/ {"vip_marujo_30d",   "VIP",       0, "MARUJO",   "PACOTE VIP 30 DIAS",   "PACOTE MARUJO (30D)",      24990, 0,     0, 0.62f, 0.25f, 1.00f, {43, 4, 11}},
        /* 2*/ {"ship_galeao_ouro", "NAVIO",     2, "GALE\xC3O", "DOURADO",              "GALE\xC3O DOURADO",        89900, 0,     ShopSoldOut, 0.95f, 0.75f, 0.25f, {42, -1, -1}},
        /* 3*/ {"bau_tesouro",      "RARO",      5, "TESOURO",  "BA\xDA COM ITENS RAROS","BA\xDA DE TESOURO",       12500, 15000, 0, 0.20f, 0.60f, 1.00f, {48, 8, 9}},
        /* 4*/ {"tool_machado_ouro","NOVO",      4, "OURO",     "MACHADINHA",           "MACHADINHA DE OURO",        8500, 0,     0, 1.00f, 0.80f, 0.20f, {1, 9, -1}},
        /* 5*/ {"ship_chalupa",     "NAVIO",     5, "CHALUPA",  "R\xC1PIDA",            "CHALUPA R\xC1PIDA",        34900, 0,     0, 0.15f, 0.75f, 0.85f, {19, 29, -1}},
        /* 6*/ {"ship_fantasma",    "LEND\xC1RIO",3,"FANTASMA", "NAVIO AMALDI\xC7OADO", "NAVIO FANTASMA",           99990, 0,     0, 0.30f, 1.00f, 0.65f, {46, 42, -1}},
        /* 7*/ {"ship_bergantim",   "NAVIO",     2, "BRIGUE",   "DE GUERRA",            "BRIGUE DE GUERRA",         64900, 0,     ShopSoldOut, 0.70f, 0.70f, 0.75f, {42, 22, -1}},
        /* 8*/ {"ship_canhoes",     "RARO",      1, "BRONZE",   "CANH\xD5" "ES PARA O NAVIO", "CANH\xD5" "ES DE BRONZE", 15000, 19900, 0, 0.95f, 0.45f, 0.15f, {22, -1, -1}},
        /* 9*/ {"tool_picareta_dia","LEND\xC1RIO",3,"DIAMANTE", "PICARETA",             "PICARETA DE DIAMANTE",     12000, 0,     0, 0.35f, 0.85f, 1.00f, {2, 8, -1}},
        /*10*/ {"tool_adaga_cors",  "RARO",      0, "CORS\xC1RIO","ADAGA AFIADA",       "ADAGA DO CORS\xC1RIO",      6500, 9000,  0, 0.80f, 0.20f, 0.30f, {0, 47, -1}},
        /*11*/ {"tool_tocha_eterna","NOVO",      1, "ETERNA",   "TOCHA QUE N\xC3O APAGA","TOCHA ETERNA",             3200, 0,     0, 1.00f, 0.45f, 0.10f, {3, -1, -1}},
        /*12*/ {"tool_kit_madeira", "PACOTE",    2, "LENHADOR", "KIT DE MADEIRA",       "KIT DO LENHADOR",            800, 0,     ShopSoldOut, 0.55f, 0.40f, 0.25f, {10, 11, 7}},
        /*13*/ {"skin_pirata_verm", "SKIN",      3, "PIRATA",   "VERMELHO",             "SKIN: PIRATA VERMELHO",    19990, 0,     0, 1.00f, 0.15f, 0.15f, {49, 44, -1}},
        /*14*/ {"chapeu_capitao",   "NOVO",      6, "CHAP\xC9U","DE CAPIT\xC3O",        "CHAP\xC9U DE CAPIT\xC3O",   9900, 12900, 0, 0.95f, 0.70f, 0.25f, {37, -1, -1}},
        /*15*/ {"bandana_vermelha", "COMUM",     2, "BANDANA",  "VERMELHA",             "BANDANA",                   2500, 0,     0, 0.90f, 0.20f, 0.20f, {40, -1, -1}},
        /*16*/ {"papagaio",         "LEND\xC1RIO",4,"LORO",     "PAPAGAIO NO OMBRO",    "PAPAGAIO",                 29990, 0,     0, 0.20f, 0.90f, 0.35f, {38, -1, -1}},
        /*17*/ {"tapa_olho",        "RARO",      2, "OLHO",     "TAPA-OLHO",            "TAPA-OLHO",                 4500, 0,     ShopSoldOut, 0.60f, 0.60f, 0.65f, {39, -1, -1}},
        /*18*/ {"vip_almirante_30d","VIP",       5, "ALMIRANTE","PACOTE VIP 30 DIAS",   "PACOTE ALMIRANTE (30D)",   74990, 0,     0, 0.25f, 0.50f, 1.00f, {51, 42, 8}},
        /*19*/ {"vip_lenda_90d",    "LEND\xC1RIO",3,"LENDA",    "DOS MARES - 90 DIAS",  "PACOTE LENDA (90D)",      149990, 199990, 0, 1.00f, 0.25f, 0.45f, {43, 51, 9}},
        /*20*/ {"coins_500",        "MOEDAS",    6, "500",      "SACO DE MOEDAS",       "PACOTE DE MOEDAS 500",       490, 0,     ShopRealMoney, 1.00f, 0.80f, 0.25f, {54, -1, -1}},
        /*21*/ {"coins_2500",       "MOEDAS",    6, "2500",     "+10% DE B\xD4NUS",     "PACOTE DE MOEDAS 2500",     1990, 2450,  ShopRealMoney, 1.00f, 0.65f, 0.15f, {45, -1, -1}},
        /*22*/ {"coins_10000",      "MAIS VALOR",1, "10000",    "+25% DE B\xD4NUS",     "PACOTE DE MOEDAS 10000",    6990, 0,     ShopRealMoney, 1.00f, 0.50f, 0.10f, {45, 54, -1}},
        /*23*/ {"coins_50000",      "LEND\xC1RIO",3,"50000",    "COFRE REAL",           "COFRE REAL 50000",         29990, 0,     ShopRealMoney | ShopSoldOut, 0.95f, 0.30f, 0.20f, {48, 45, 9}},
        /*24*/ {"id_curto",         "ID",        5, "123",      "3 D\xCDGITOS",         "ID CURTO (3 D\xCDGITOS)",  15000, 0,     0, 0.30f, 0.60f, 1.00f, {41, -1, -1}},
        /*25*/ {"id_dourado",       "ID",        6, "DOURADO",  "ID COM BRILHO",        "ID DOURADO",                9900, 0,     0, 1.00f, 0.78f, 0.25f, {41, 9, -1}},
        /*26*/ {"id_personal",      "NOVO",      4, "MEU ID",   "ESCOLHA O N\xDAMERO",  "ID PERSONALIZADO",         25000, 30000, 0, 0.30f, 0.90f, 0.60f, {41, 55, -1}},
        /*27*/ {"id_007",           "RARO",      2, "007",      "ID ESPECIAL",          "ID 007",                   99000, 0,     ShopSoldOut, 0.70f, 0.70f, 0.70f, {41, -1, -1}},
        /*28*/ {"ad_moedas",        "GR\xC1TIS", 4, "GR\xC1TIS","ASSISTA UM AN\xDANCIO","+50 MOEDAS GR\xC1TIS",        50, 0,     ShopAd, 0.20f, 0.80f, 0.40f, {54, -1, -1}},
        /*29*/ {"licenca_tripulacao","NOVO",     6, "TRIPULA\xC7\xC3" "O","LICEN\xC7" "A PARA CRIAR","LICEN\xC7" "A DE TRIPULA\xC7\xC3" "O", e0net::kTeamLicenseCost, 0, 0, 0.85f, 0.18f, 0.16f, {43, 47, -1}},   // ActBuyTeamLicense (servidor)
    };
    n = (int)(sizeof k / sizeof k[0]); return k;
}
struct ShopTab { const char* name; int8_t items[8]; };   // índices em shopItems, -1 termina
static const ShopTab* shopTabs(int& n) {
    static const ShopTab k[] = {
        {"MAIS VENDIDOS", {28, 29, 0, 1, 2, 3, 4, 16}},
        {"NAVIOS",        {6, 5, 2, 7, 8, -1}},
        {"FERRAMENTAS",   {4, 9, 10, 11, 12, -1}},
        {"PERSONAGEM",    {13, 14, 15, 16, 17, -1}},
        {"PACOTES VIP",   {1, 0, 18, 19, -1}},
        {"MOEDAS",        {28, 20, 21, 22, 23, -1}},
        {"IDS",           {24, 25, 26, 27, -1}},
    };
    n = (int)(sizeof k / sizeof k[0]); return k;
}
int shopTabCount(int tab) const { int n; const ShopTab* T = shopTabs(n); int c = 0; while (c < 8 && T[tab].items[c] >= 0) ++c; return c; }
const ShopItem& shopItemAt(int tab, int i) const { int n, m; const ShopTab* T = shopTabs(n); return shopItems(m)[T[tab].items[i]]; }

bool shopOpen = false;
int shopTab = 0, shopSel = -1, shopPtr = -1;
float shopScroll = 0.0f, shopVel = 0.0f, shopPrevScroll = 0.0f, shopX0 = 0.0f, shopScroll0 = 0.0f;
bool shopMoved = false;
std::string shopMsg; float shopMsgT = 0.0f;

struct ShopGeo { float u, cw, gap, top, artH, priceH, tabY; };
ShopGeo shopGeo() const {
    ShopGeo g; g.u = std::min((float)h / 1080.0f, (float)w / 1900.0f); const float u = g.u;
    g.cw = 655 * u; g.gap = 40 * u; g.top = 40 * u; g.priceH = 90 * u; g.tabY = (float)h - 100 * u;
    g.artH = g.tabY - 40 * u - g.priceH - g.top;
    return g;
}
float shopMaxScroll() const { ShopGeo g = shopGeo(); int n = shopTabCount(shopTab); return std::max(0.0f, n * (g.cw + g.gap) - g.gap - (float)w); }
Rect2D shopCardRect(int i) const { ShopGeo g = shopGeo(); return {i * (g.cw + g.gap) - shopScroll, g.top, g.cw, g.artH + g.priceH}; }
Rect2D shopPriceRect(int i) const { ShopGeo g = shopGeo(); Rect2D c = shopCardRect(i); return {c.x, g.top + g.artH, g.cw, g.priceH}; }
// canto superior direito: saldo (pílula), + dourado, X
void shopTopGeo(float& pillL, float& pillR, float& cy, float& plusX, float& closeX, float& br) const {
    const float u = shopGeo().u;
    cy = 90 * u; br = 42 * u; closeX = (float)w - 92 * u; plusX = closeX - 112 * u;
    pillR = plusX - br - 16 * u;   // da direita p/ esquerda: X, +, saldo (sem sobrepor)
    char ct[16]; coinText(ct, sizeof ct, coins);
    const int ts = std::max(2, (int)std::lround(6 * u));
    pillL = pillR - std::max(300.0f * u, 96 * u + Renderer::textWidth(ct, ts) + 44 * u);   // cresce com o número
}
int shopTabTs() const { return std::max(2, (int)std::lround(3.8f * shopGeo().u)); }
Rect2D shopTabRect(int t) const {
    ShopGeo g = shopGeo(); int n; const ShopTab* T = shopTabs(n); const int ts = shopTabTs(); const float pad = 28 * g.u;
    float x = 16 * g.u;
    for (int i = 0; i < t; ++i) x += Renderer::textWidth(T[i].name, ts) + 2 * pad;
    return {x, g.tabY, Renderer::textWidth(T[t].name, ts) + 2 * pad, (float)h - g.tabY};
}
Rect2D shopOpenBtnRect() const {   // ao lado do CRAFT, mesmo tamanho
    return menuBtnRect(3);
}
void openShop() {
    shopOpen = true; shopPtr = -1; shopVel = 0.0f; shopMsgT = 0.0f;
    joyT.id = lookT.id = runPtr = collectPtr = actionPtr = -1; collectDown = false; input.moveX = input.moveY = 0;
}
void closeShop() { shopOpen = false; shopPtr = -1; shopSel = -1; }
void shopSay(const char* t) { shopMsg = t; shopMsgT = 2.6f; }
static constexpr int kShopCoinsTab = 5;   // aba MOEDAS (o + dourado do saldo abre ela)
std::unordered_map<std::string, std::string> shopPlayPrice;   // preço formatado pela Google Play (evento PRICE), por sku
// COMPRA: R$ -> Google Play; anúncio -> AdMob premiado; moedas -> ainda nada (não há o que entregar).
void shopBuy(const ShopItem& it) {
    if (it.flags & ShopSoldOut) { shopSay("ITEM ESGOTADO"); return; }
    if (it.flags & ShopAd) {
        if (!pay.active()) { shopSay("ANUNCIOS INDISPONIVEIS"); return; }
        if (!(pay.status() & Monetize::RewardedReady)) { shopSay("ANUNCIO CARREGANDO... TENTE EM INSTANTES"); return; }
        pay.showRewarded(kAdTagShop); return;
    }
    if (it.flags & ShopRealMoney) {
        if (!pay.active()) { shopSay("COMPRAS INDISPONIVEIS"); return; }
        shopSay("ABRINDO A GOOGLE PLAY..."); pay.buy(it.sku); return;
    }
    if (std::strcmp(it.sku, "licenca_tripulacao") == 0) {   // TRIPULAÇÃO: o servidor confere e desconta as moedas (ActBuyTeamLicense)
        if (!net.connected() || !invServer()) { shopSay("ENTRE NUM SERVIDOR PARA COMPRAR"); return; }
        if (coins < e0net::kTeamLicenseCost) { shopSay("MOEDAS INSUFICIENTES"); return; }
        net.requestAct(e0net::ActBuyTeamLicense, 0, 0); shopSay("COMPRANDO..."); return;
    }
    shopSay("COMPRAS COM MOEDAS EM BREVE");
}
// produtos da Play: "sku:1" = consumível (pacotes de moedas: podem ser comprados de novo)
static std::string shopPlaySkus() {
    int n; const ShopItem* k = shopItems(n); std::string s;
    for (int i = 0; i < n; ++i) if (k[i].flags & ShopRealMoney) { if (!s.empty()) s += ','; s += k[i].sku; s += ":1"; }
    return s;
}
// compra confirmada pela Play (ou simulada no modo teste): entrega
void shopDeliver(const std::string& sku, bool fake) {
    int n; const ShopItem* k = shopItems(n);
    for (int i = 0; i < n; ++i) {
        if (sku != k[i].sku) continue;
        int add = 0;
        if (std::strncmp(k[i].sku, "coins_", 6) == 0) add = std::atoi(k[i].sku + 6);
        coins += add;
        char m[64]; std::snprintf(m, sizeof m, "COMPRA OK: +%d MOEDAS%s", add, fake ? " (TESTE)" : "");
        if (shopOpen) shopSay(m); else notify(m);
        LOG("compra entregue: %s +%d moedas%s", sku.c_str(), add, fake ? " (simulada)" : "");
        return;
    }
    LOG("compra de produto desconhecido: %s", sku.c_str());
}
void shopSetTab(int t) { if (t == shopTab) return; shopTab = t; shopScroll = shopVel = 0.0f; shopSel = -1; shopPtr = -1; }
void shopTouchDown(int pid, float x, float y) {
    float pl, pr, cy, px, cx, br; shopTopGeo(pl, pr, cy, px, cx, br);
    if (std::hypot(x - cx, y - cy) < br * 1.25f) { closeShop(); return; }
    if (std::hypot(x - px, y - cy) < br * 1.25f) { shopSetTab(kShopCoinsTab); return; }   // + = comprar moedas
    if (x > pl && x < pr && std::fabs(y - cy) < br) return;   // pílula do saldo: não faz nada
    int nt; shopTabs(nt);
    for (int t = 0; t < nt; ++t) if (shopTabRect(t).contains(x, y)) { shopSetTab(t); return; }
    ShopGeo g = shopGeo();
    if (shopPtr < 0 && y >= g.top && y < g.top + g.artH + g.priceH) {
        shopPtr = pid; shopX0 = x; shopScroll0 = shopScroll; shopMoved = false; shopVel = 0.0f; shopPrevScroll = shopScroll;
    }
}
void shopTouchMove(int pid, float x, float /*y*/) {
    if (pid != shopPtr) return;
    const float dx = x - shopX0;
    if (!shopMoved && std::fabs(dx) > 16.0f * shopGeo().u) { shopMoved = true; shopX0 = x; shopScroll0 = shopScroll; return; }
    if (!shopMoved) return;
    float s = shopScroll0 - dx, mx = shopMaxScroll();
    if (s < 0.0f) s *= 0.35f; else if (s > mx) s = mx + (s - mx) * 0.35f;   // elástico além dos limites
    shopScroll = s;
}
void shopTouchUp(int pid, float x, float y) {
    if (pid != shopPtr) return;
    shopPtr = -1;
    if (shopMoved) return;   // arrasto: a inércia continua em updateShop
    shopVel = 0.0f;
    const int n = shopTabCount(shopTab);
    for (int i = 0; i < n; ++i) {
        if (!shopCardRect(i).contains(x, y)) continue;
        if (shopPriceRect(i).contains(x, y) || shopSel == i) shopBuy(shopItemAt(shopTab, i));   // barra de preço (ou 2º toque no cartão) = comprar
        shopSel = i;
        return;
    }
}
void updateShop(float dt) {
    shopMsgT = std::max(0.0f, shopMsgT - dt);
    if (shopOpen && (selfDead || isleMode)) closeShop();
    if (!shopOpen || dt <= 0.0f) return;
    const float mx = shopMaxScroll();
    if (shopPtr >= 0) {   // arrastando: mede a velocidade para a inércia
        if (shopMoved) shopVel = shopVel * 0.4f + 0.6f * (shopScroll - shopPrevScroll) / dt;
    } else {
        shopScroll += shopVel * dt;
        shopVel *= std::exp(-3.2f * dt);
        if (shopScroll < 0.0f) { shopScroll += (0.0f - shopScroll) * std::min(1.0f, dt * 12.0f); if (shopVel < 0) shopVel *= std::exp(-20.0f * dt); }
        else if (shopScroll > mx) { shopScroll += (mx - shopScroll) * std::min(1.0f, dt * 12.0f); if (shopVel > 0) shopVel *= std::exp(-20.0f * dt); }
        if (std::fabs(shopVel) < 4.0f) shopVel = 0.0f;
        if (shopVel == 0.0f && std::fabs(shopScroll) < 0.5f) shopScroll = 0.0f;
    }
    shopPrevScroll = shopScroll;
}
static void shopTagColor(int c, float& r, float& g, float& b) {
    // cores sóbrias/rústicas de lacre e tecido (nunca neon plástico)
    static const float k[7][3] = {{0.42f, 0.22f, 0.52f}, {0.72f, 0.42f, 0.14f}, {0.26f, 0.23f, 0.19f}, {0.58f, 0.15f, 0.13f},
                                  {0.18f, 0.42f, 0.26f}, {0.16f, 0.36f, 0.54f}, {0.74f, 0.54f, 0.18f}};
    c = std::max(0, std::min(6, c)); r = k[c][0]; g = k[c][1]; b = k[c][2];
}
static void shopFmtPrice(char* out, size_t n, const ShopItem& it, int v) {
    if (it.flags & ShopRealMoney) { std::snprintf(out, n, "R$ %d,%02d", v / 100, v % 100); return; }
    if (v >= 1000000) std::snprintf(out, n, "%d.%03d.%03d", v / 1000000, (v / 1000) % 1000, v % 1000);
    else if (v >= 1000) std::snprintf(out, n, "%d.%03d", v / 1000, v % 1000);
    else std::snprintf(out, n, "%d", v);
}
void shopIcon(int idx, float cx, float cy, float half, float r, float g, float b, float a) {
    if (idx < 0) return;
    if (idx < 12) renderer.uiIcon(idx, cx, cy, half, a);
    else renderer.uiIconTint(idx, cx, cy, half, r, g, b, a);
}
// Arte central do cartão: ícones do atlas ampliados sobre brilho quente da cor enobrecida do item
void shopArt(const ShopItem& it, float cx, float cy, float S, float a) {
    const float lr = 0.68f + it.r * 0.28f, lg = 0.64f + it.g * 0.28f, lb = 0.52f + it.b * 0.26f;
    renderer.uiGlow(cx, cy, S * 1.25f, S * 1.10f, it.r * 0.85f + 0.12f, it.g * 0.78f + 0.08f, it.b * 0.65f + 0.04f, 0.42f * a);
    renderer.uiGlow(cx, cy, S * 0.60f, S * 0.55f, lr, lg, lb, 0.24f * a);
    renderer.uiGlow(cx, cy + S * 0.78f, S * 0.95f, S * 0.16f, 0, 0, 0, 0.58f * a);
    int cnt = 0; for (int k = 0; k < 3; ++k) if (it.ic[k] >= 0) ++cnt;
    if (cnt <= 1) shopIcon(it.ic[0], cx, cy, S * 0.95f, lr, lg, lb, a);
    else {
        shopIcon(it.ic[0], cx, cy - S * 0.10f, S * 0.85f, lr, lg, lb, a);
        shopIcon(it.ic[1], cx + S * 0.62f, cy + S * 0.50f, S * 0.42f, lr, lg, lb, a);
        if (cnt > 2) shopIcon(it.ic[2], cx - S * 0.62f, cy + S * 0.50f, S * 0.40f, lr, lg, lb, a);
    }
}
void drawShopShapes() {
    ShopGeo g = shopGeo(); const float u = g.u;
    // fundo: carvalho noturno quente com vinheta (mesmo tema de todas as telas)
    renderer.uiWoodPanel(w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f, theme::PanelBgWood, 1.0f, 3.0f);
    renderer.uiGlow(w * 0.5f, h * 0.42f, w * 0.70f, h * 0.80f, 0.92f, 0.64f, 0.28f, 0.06f);
    const int n = shopTabCount(shopTab);
    for (int i = 0; i < n; ++i) {
        Rect2D c = shopCardRect(i);
        if (c.x + c.w < 0.0f || c.x > (float)w) continue;
        const ShopItem& it = shopItemAt(shopTab, i);
        const bool out = it.flags & ShopSoldOut;
        const float cx = c.x + c.w * 0.5f, ay = g.top + g.artH * 0.5f;
        const float sat = out ? 0.15f : 0.78f;   // enobrece as cores (menos neon, mais pintura pirata)
        const float r = it.r * sat + 0.34f * (1 - sat), gg = it.g * sat + 0.28f * (1 - sat), b = it.b * sat + 0.20f * (1 - sat);
        // moldura externa do cartão em carvalho + latão
        renderer.uiWoodPanel(cx, c.y + c.h * 0.5f, c.w * 0.5f, c.h * 0.5f, 14.0f * u, i == shopSel ? theme::PanelCardSel : theme::PanelWood, 1.0f, (float)i + 1.0f);
        // interior da arte: carvalho escuro -> tom do produto
        const float ins = 8.0f * u;
        renderer.uiGrad(cx, ay + ins * 0.4f, c.w * 0.5f - ins, g.artH * 0.5f - ins * 0.6f, 8.0f * u,
                        0.07f + r * 0.05f, 0.055f + gg * 0.04f, 0.042f + b * 0.03f, 1.0f,
                        r * 0.38f, gg * 0.34f, b * 0.30f, 1.0f, 0.0f, 1.0f, 0.55f, (float)(i * 13 + shopTab * 5));
        shopArt(it, cx, g.top + g.artH * 0.58f, g.artH * 0.22f, out ? 0.50f : 1.0f);
        // vinheta na base da arte para o nome ler bem
        renderer.uiGrad(cx, g.top + g.artH * 0.87f, c.w * 0.5f - ins, g.artH * 0.13f, 0.0f, 0.05f, 0.035f, 0.025f, 0.0f, 0.05f, 0.035f, 0.025f, 0.78f, 0, 1);
        if (out) renderer.uiRoundRect(cx, ay, c.w * 0.5f - ins, g.artH * 0.5f - ins * 0.5f, 8.0f * u, 0.08f, 0.07f, 0.06f, 0.58f);
        // selo da etiqueta com fio de latão
        {
            float tr, tg, tb; shopTagColor(it.tag ? it.tagCol : 2, tr, tg, tb);
            if (out) { tr = 0.20f; tg = 0.18f; tb = 0.16f; }
            const int ts = std::max(2, (int)std::lround(4.5f * u));
            float pw = Renderer::textWidth(it.tag, ts) * 0.5f + 28 * u, ph = 22 * u;
            renderer.uiRoundRect(cx, g.top + 66 * u, pw, ph, 8.0f * u, tr, tg, tb, 0.95f);
            renderer.uiRoundRectBorder(cx, g.top + 66 * u, pw, ph, 8.0f * u, 1.6f * u, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, out ? 0.35f : 0.78f);
        }
        // barra de preço: PERGAMINHO ENVELHECIDO (ou carvalho apagado se esgotado)
        Rect2D p = shopPriceRect(i);
        if (out) renderer.uiButton(cx, p.y + p.h * 0.5f - 4.0f * u, p.w * 0.5f - ins, p.h * 0.5f - 4.0f * u, 8.0f * u, theme::BtnDisabled, 0.96f);
        else     renderer.uiWoodPanel(cx, p.y + p.h * 0.5f - 4.0f * u, p.w * 0.5f - ins, p.h * 0.5f - 4.0f * u, 8.0f * u, theme::PanelParchment, 1.0f, (float)i * 3.0f);
        if (!out && !(it.flags & (ShopRealMoney | ShopAd))) {   // ícone da moeda em tinta sépia
            char pt[24]; shopFmtPrice(pt, sizeof pt, it, it.oldPrice > 0 ? it.oldPrice : it.price);
            const int ps = std::max(2, (int)std::lround(5.5f * u));
            float tw = Renderer::textWidth(pt, ps), ic = 25 * u;
            if (it.oldPrice > 0) { char np[24]; shopFmtPrice(np, sizeof np, it, it.price); tw += Renderer::textWidth(np, ps) + 30 * u; }
            float x0 = cx - (tw + ic * 2 + 16 * u) * 0.5f;
            renderer.uiShape(Renderer::UiRing, x0 + ic, p.y + p.h * 0.5f - 4.0f * u, ic, theme::kInk.r, theme::kInk.g, theme::kInk.b, it.oldPrice > 0 ? 0.55f : 0.92f);
            renderer.uiIconTint(HiCoins, x0 + ic, p.y + p.h * 0.5f - 4.0f * u, ic * 0.66f, theme::kInk.r, theme::kInk.g, theme::kInk.b, it.oldPrice > 0 ? 0.55f : 0.95f);
        }
    }
    // barra de abas inferior: madeira escura + fio superior de latão; aba ativa em latão batido
    renderer.uiWoodPanel(w * 0.5f, (g.tabY + h) * 0.5f, w * 0.5f, (h - g.tabY) * 0.5f, 0.0f, theme::PanelCard, 0.98f, 11.0f);
    Rect2D at = shopTabRect(shopTab);
    renderer.uiButton(at.x + at.w * 0.5f, at.y + at.h * 0.5f, at.w * 0.5f - 4.0f * u, at.h * 0.5f - 6.0f * u, 10.0f * u, theme::BtnBrass, 1.0f);
}
// por cima do texto dos cartões: saldo/+/X e o aviso (desenhado num 2º passe de formas, ver drawShopText)
void drawShopOverlayShapes() {
    ShopGeo g = shopGeo(); const float u = g.u;
    // topo direito: saldo (carvalho+latão), + (latão batido), X (lacre+latão)
    float pl, pr, cy, px, cx, br; shopTopGeo(pl, pr, cy, px, cx, br);
    renderer.uiWoodPanel((pl + pr) * 0.5f, cy, (pr - pl) * 0.5f, br * 1.10f, br * 1.10f, theme::PanelCard, 0.97f, 2.0f);
    renderer.uiShape(Renderer::UiRing, pl + 52 * u, cy, 24 * u, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.95f);
    renderer.uiIconTint(HiCoins, pl + 52 * u, cy, 17 * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    renderer.uiButton(px, cy, br, br, br, theme::BtnBrass, 1.0f);
    renderer.uiIconTint(HiPlus, px, cy, br * 0.50f, theme::kInk.r, theme::kInk.g, theme::kInk.b, 1.0f);
    closeBtnShapes(cx, cy, br * 1.12f);
    // aviso (comprar / esgotado)
    if (shopMsgT > 0.0f) {
        const int ts = std::max(2, (int)std::lround(4.5f * u)); float tw = Renderer::textWidth(shopMsg.c_str(), ts);
        float a = std::min(1.0f, shopMsgT * 3.0f);
        renderer.uiWoodPanel(w * 0.5f, h * 0.5f, tw * 0.5f + 44 * u, 36 * u, 16 * u, theme::PanelCardSel, 0.98f * a, 8.0f);
    }
}
// texto em destaque (Cinzel já é SemiBold/Bold: desenha 1 vez nítida sem duplicar/borrar serifas)
void shopBold(float x, float y, int ts, const char* s, float r, float g, float b, float a, bool shadow = true) {
    renderer.text(x, y, ts, s, r, g, b, a, shadow);
}
void drawShopText() {
    ShopGeo g = shopGeo(); const float u = g.u;
    const int n = shopTabCount(shopTab);
    for (int i = 0; i < n; ++i) {
        Rect2D c = shopCardRect(i);
        if (c.x + c.w < 0.0f || c.x > (float)w) continue;
        const ShopItem& it = shopItemAt(shopTab, i);
        const bool out = it.flags & ShopSoldOut;
        const float cx = c.x + c.w * 0.5f, ta = out ? 0.55f : 1.0f;
        {   // etiqueta
            const int ts = std::max(2, (int)std::lround(4.5f * u));
            shopBold(cx - Renderer::textWidth(it.tag, ts) * 0.5f, g.top + 66 * u - 3.5f * ts, ts, it.tag, theme::kCream.r, theme::kCream.g, theme::kCream.b, out ? 0.65f : 1.0f, false);
        }
        {   // título grande (escala que caiba na largura)
            int ts = (int)std::lround(14 * u);
            while (ts > 3 && Renderer::textWidth(it.title, ts) > c.w - 84 * u) --ts;
            float tw = Renderer::textWidth(it.title, ts);
            shopBold(cx - tw * 0.5f, g.top + 114 * u, ts, it.title, out ? 0.60f : theme::kCream.r, out ? 0.56f : theme::kCream.g, out ? 0.50f : theme::kCream.b, 1.0f);
            const int ss = std::max(2, (int)std::lround(4.5f * u));
            int s2 = ss; while (s2 > 2 && Renderer::textWidth(it.sub, s2) > c.w - 64 * u) --s2;
            shopBold(cx - Renderer::textWidth(it.sub, s2) * 0.5f, g.top + 114 * u + 7.0f * ts + 22 * u, s2, it.sub, out ? 0.52f : theme::kBrassBright.r, out ? 0.48f : theme::kBrassBright.g, out ? 0.42f : theme::kBrassBright.b, 0.95f);
        }
        {   // nome embaixo à esquerda
            int ns = std::max(2, (int)std::lround(5.0f * u));
            while (ns > 2 && Renderer::textWidth(it.name, ns) > c.w - 84 * u) --ns;
            shopBold(c.x + 36 * u, g.top + g.artH - 54 * u - 3.5f * ns, ns, it.name, theme::kCream.r, theme::kCream.g, theme::kCream.b, ta);
        }
        Rect2D p = shopPriceRect(i);
        const float py = p.y + p.h * 0.5f - 4.0f * u;
        const int ps = std::max(2, (int)std::lround(5.5f * u));
        if (out) {
            const char* e = "ESGOTADO"; shopBold(cx - Renderer::textWidth(e, ps) * 0.5f, py - 3.5f * ps, ps, e, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
            continue;
        }
        if (it.flags & ShopAd) {
            const bool ready = pay.status() & Monetize::RewardedReady;
            const char* e = ready ? "ASSISTIR AN\xDANCIO" : "CARREGANDO...";
            int aps = ps; while (aps > 2 && Renderer::textWidth(e, aps) > p.w - 60 * u) --aps;
            shopBold(cx - Renderer::textWidth(e, aps) * 0.5f, py - 3.5f * aps, aps, e, ready ? 0.14f : 0.34f, ready ? 0.34f : 0.28f, ready ? 0.16f : 0.22f, 1.0f, false);
            continue;
        }
        const bool coin = !(it.flags & ShopRealMoney);
        char np[24]; shopFmtPrice(np, sizeof np, it, it.price);
        if (!coin) {
            auto pp = shopPlayPrice.find(it.sku);
            if (pp != shopPlayPrice.end()) std::snprintf(np, sizeof np, "%s", pp->second.c_str());
        }
        float ic = coin ? 25 * u : 0.0f, tw = Renderer::textWidth(np, ps);
        char op[24] = ""; float ow = 0.0f;
        if (it.oldPrice > 0) { shopFmtPrice(op, sizeof op, it, it.oldPrice); ow = Renderer::textWidth(op, ps) + 30 * u; }
        float x = cx - (tw + ow + ic * 2 + (coin ? 16 * u : 0.0f)) * 0.5f + ic * 2 + (coin ? 16 * u : 0.0f);
        if (it.oldPrice > 0) {   // preço antigo riscado em sépia apagado + traço carmesim
            renderer.text(x, py - 3.5f * ps, ps, op, theme::kInkSoft.r, theme::kInkSoft.g, theme::kInkSoft.b, 0.85f, false);
            renderer.rect(x - 6 * u - (coin ? ic * 2 + 16 * u : 0.0f), py - 1.5f * u, Renderer::textWidth(op, ps) + 12 * u + (coin ? ic * 2 + 16 * u : 0.0f), 3 * u, 0.65f, 0.14f, 0.12f, 0.92f);
            x += ow;
        }
        shopBold(x, py - 3.5f * ps, ps, np, theme::kInk.r, theme::kInk.g, theme::kInk.b, 1.0f, false);
    }
    // abas (ativa = tinta sépia sobre latão; inativas = pergaminho creme sobre carvalho)
    int nt; const ShopTab* T = shopTabs(nt); const int ts = shopTabTs();
    for (int t = 0; t < nt; ++t) {
        Rect2D r = shopTabRect(t);
        const bool on = (t == shopTab);
        const theme::Col tc = on ? theme::kInk : theme::kCreamSoft;
        shopBold(r.x + (r.w - Renderer::textWidth(T[t].name, ts)) * 0.5f, r.y + (r.h - 7.0f * ts) * 0.5f, ts, T[t].name, tc.r, tc.g, tc.b, 1.0f, !on);
    }
    // 2º passe: saldo/+/X e aviso por CIMA do texto dos cartões
    renderer.endText(); renderer.beginUi(); drawShopOverlayShapes(); renderer.endUi(); renderer.beginText();
    float pl, pr, cy, px, cx, br; shopTopGeo(pl, pr, cy, px, cx, br);
    {
        const int ts = std::max(2, (int)std::lround(5.5f * u));
        char ct[16]; coinText(ct, sizeof ct, coins);
        shopBold(pl + 96 * u, cy - 3.5f * ts, ts, ct, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
    }
    if (shopMsgT > 0.0f) {
        const int ms = std::max(2, (int)std::lround(4.5f * u)); float a = std::min(1.0f, shopMsgT * 3.0f);
        shopBold(w * 0.5f - Renderer::textWidth(shopMsg.c_str(), ms) * 0.5f, h * 0.5f - 3.5f * ms, ms, shopMsg.c_str(), theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, a, false);
    }
}
