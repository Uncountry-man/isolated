// ================= NAVEGAÇÃO (chamar navio) =================
// Incluído DENTRO de `struct Engine` (engine.cpp). Item NAVEGAÇÃO da roda de ações: abre um painel no meio da tela com os navios (um cartão por tipo,
// kMaxShips = 4). Tocar num cartão DISPONÍVEL chama o navio: o servidor o põe no mar aberto à frente de quem chamou (MsgSummonShip / SummonResult,
// regras em server_main.cpp onSummon: ninguém a bordo, não afundando, cooldown de kSummonCooldown s, jogador fora de navio e vivo).

bool navOpen = false;
float navT = 0.0f;                 // animação de entrada
double navCoolUntil = -1.0;        // relógio local: até quando o MEU cooldown vale (preenchido pelo resultado do servidor)
int navPendKind = -1; double navPendAt = 0.0; uint8_t navGen[e0net::kMaxShips] = {};   // chamado aceito: espera o snapshot trazer o navio na posição nova (gen novo) para marcar a ROTA
// rolagem horizontal dos cartões (como as listas do editor de BANDEIRA): arrastar rola com inércia; tocar sem arrastar escolhe ao soltar
float navScroll = 0.0f, navVel = 0.0f, navScroll0 = 0.0f, navDownX = 0.0f, navLastX = 0.0f;
double navLastT = 0.0;
int navPtr = -1; bool navMoved = false;
std::string navMsg; float navMsgT = 0.0f;
static double navNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void navSay(const char* m, float t = 2.2f) { navMsg = m; navMsgT = t; }

// layout em TELA CHEIA (mesma linguagem do editor de bandeira: título dourado no canto, X vermelho, fundo grafite, cartões escuros com borda dourada)
struct NavGeo { float u, closeX, closeY, closeR, cw, gap; Rect2D area; };
NavGeo navGeo() const {
    NavGeo G; G.u = std::min((float)h / 1080.0f, (float)w / 1900.0f); const float u = G.u;
    G.closeR = 44.0f * u; G.closeX = (float)w - 80.0f * u; G.closeY = 75.0f * u;
    G.area = {40.0f * u, 160.0f * u, (float)w - 80.0f * u, (float)h - 160.0f * u - 64.0f * u};
    G.cw = 600.0f * u; G.gap = 34.0f * u;
    return G;
}
Rect2D navCard(const NavGeo& G, int i) const { return {G.area.x + (float)i * (G.cw + G.gap) - navScroll, G.area.y, G.cw, G.area.h}; }
Rect2D navBtn(const NavGeo& G, int i) const { const Rect2D c = navCard(G, i); return {c.x + 30.0f * G.u, c.y + c.h - 30.0f * G.u - 96.0f * G.u, c.w - 60.0f * G.u, 96.0f * G.u}; }
float navMaxScroll(const NavGeo& G) const { return std::max(0.0f, (float)e0net::kMaxShips * (G.cw + G.gap) - G.gap - G.area.w); }

void openNav() {
    if (navOpen || selfDead) return;
    navOpen = true; navT = 0.0f; navPtr = -1; navVel = 0.0f; navMsgT = 0.0f; wheelOpen = false;
    joyT.id = -1; lookT.id = -1;
}
void closeNav() { navOpen = false; navPtr = -1; }

// estado de um navio para a tela: 0 disponível, 1 em uso, 2 afundando, 3 indisponível (afundado/renascendo), 4 aguarde (cooldown), 5 estou a bordo
int navStatus(int k, int& extra) const {
    extra = 0;
    if (!((net.shipsOwned() >> k) & 1u)) { extra = e0net::kShipPrice[k & 3]; return 6; }   // a conta não é dona deste navio: COMPRAR
    const e0net::ShipState& S = e0net::shipState(k);
    if (S.exists && !e0net::shipAfloat(S)) return 2;
    const e0net::PlayerState me = net.predicted();
    if (me.ship) return 5;
    if (S.exists && S.helmer >= 0) return 1;
    if (S.exists && S.owner >= 0 && S.owner != (int)me.id) return 1;   // navio de outro jogador
    if (S.exists) for (const auto& r : remotes) if (r.ship && r.shipIdx == k) return 1;
    const double left = ((net.shipsCrewOnly() >> k) & 1u) ? 0.0 : navCoolUntil - navNow();   // navio da tripulação: sem espera
    if (left > 0.0) { extra = (int)std::ceil(left); return 4; }
    return 0;
}

static const char* navStatusText(int st) {
    static const char* k[7] = {"CHAMAR", "EM USO", "AFUNDANDO", "INDISPON\xCDVEL", "AGUARDE", "VOC\xCA EST\xC1 A BORDO", "COMPRAR"};
    return k[std::max(0, std::min(6, st))];
}
void navTap(int i) {
    int extra; const int st = navStatus(i, extra);
    if (st == 6) {   // COMPRAR
        char t[80];
        if (coins < extra) { std::snprintf(t, sizeof t, "MOEDAS INSUFICIENTES: PRECISA DE %d", extra); navSay(t); }
        else if (net.actBusy() >= 3) navSay("AGUARDE...");
        else { net.requestAct(e0net::ActBuyShip, (uint16_t)i, 0); navSay("COMPRANDO..."); }
        return;
    }
    if (st == 0) {
        navGen[i] = e0net::shipState(i).gen;
        net.requestSummon(i);
        char t[64]; std::snprintf(t, sizeof t, "CHAMANDO %s...", e0net::shipDef(i).name); notify(t);
        closeNav();
        return;
    }
    static const char* kMsg[6] = {"", "NAVIO EM USO POR OUTRO JOGADOR", "NAVIO AFUNDANDO", "NAVIO INDISPON\xCDVEL", "AGUARDE %d S PARA CHAMAR DE NOVO", "DES\xC7" "A DO NAVIO PARA CHAMAR OUTRO"};
    char t[64]; std::snprintf(t, sizeof t, kMsg[st], extra); navSay(t);
}
void navTouchDown(int pid, float x, float y) {
    const NavGeo G = navGeo();
    { const float dx = x - G.closeX, dy = y - G.closeY; if (dx * dx + dy * dy <= G.closeR * G.closeR * 1.4f) { closeNav(); return; } }
    if (G.area.contains(x, y) && navPtr < 0) {
        navPtr = pid; navDownX = x; navLastX = x; navLastT = navNow(); navScroll0 = navScroll;
        navMoved = std::fabs(navVel) > 200.0f;   // lista ainda deslizando: o toque só a segura
        navVel = 0.0f;
    }
}
void navTouchMove(int pid, float x, float /*y*/) {
    if (pid != navPtr) return;
    const NavGeo G = navGeo();
    const float delta = x - navDownX;
    if (!navMoved && std::fabs(delta) > 14.0f * G.u) navMoved = true;
    if (!navMoved) return;
    const float maxS = navMaxScroll(G);
    float sc = navScroll0 - delta;
    if (sc < 0.0f) sc *= 0.35f; else if (sc > maxS) sc = maxS + (sc - maxS) * 0.35f;   // elástico nas pontas
    navScroll = sc;
    const double t = navNow(); const float dtt = (float)std::max(1e-3, t - navLastT);
    navVel = navVel * 0.4f + 0.6f * (-(x - navLastX) / dtt);
    navLastX = x; navLastT = t;
}
void navTouchUp(int pid, float x, float y, bool cancel) {
    if (pid != navPtr) return;
    navPtr = -1;
    if (navMoved) { if (navNow() - navLastT > 0.08) navVel = 0.0f; return; }
    navVel = 0.0f;
    if (cancel) return;
    const NavGeo G = navGeo();
    for (int i = 0; i < e0net::kMaxShips; ++i) if (navCard(G, i).contains(x, y)) { navTap(i); return; }
}

void updateNav(float dt) {
    if (navOpen) {
        navT += dt; if (selfDead) closeNav();
        if (navMsgT > 0.0f) navMsgT -= dt;
        if (navPtr < 0) {   // inércia e volta para dentro dos limites
            const float maxS = navMaxScroll(navGeo());
            navScroll += navVel * dt; navVel *= std::exp(-4.5f * dt);
            if (std::fabs(navVel) < 5.0f) navVel = 0.0f;
            if (navScroll < 0.0f) { navVel = 0.0f; navScroll *= std::exp(-14.0f * dt); if (navScroll > -0.5f) navScroll = 0.0f; }
            if (navScroll > maxS) { navVel = 0.0f; navScroll = maxS + (navScroll - maxS) * std::exp(-14.0f * dt); if (navScroll - maxS < 0.5f) navScroll = maxS; }
        }
    }
    // ROTA até o navio chamado (mapa, minimapa e rótulo no mundo): marca assim que o navio aparece na posição nova e SEGUE o navio enquanto ele estiver parado/à deriva;
    // some ao embarcar, ao chegar perto (< 12 m do centro), se o navio afundar ou se o jogador limpar a rota.
    if (navPendKind >= 0) {
        const e0net::ShipState& S = e0net::shipState(navPendKind);
        if (S.exists && S.gen != navGen[navPendKind]) { wpShip = navPendKind; wpSet = true; wpX = S.x; wpZ = S.z; navPendKind = -1; }
        else if (navNow() - navPendAt > 3.0) navPendKind = -1;
    }
    if (wpShip >= 0) {
        const e0net::ShipState& S = e0net::shipState(wpShip);
        if (!wpSet || net.predicted().ship || !e0net::shipAfloat(S)) { wpShip = -1; wpSet = false; }
        else { wpX = S.x; wpZ = S.z; }
    }
    for (NetClient::SummonRes s; net.popSummon(s); ) {
        char t[80];
        const char* nm = e0net::shipDef(s.kind).name;
        switch (s.result) {
            case e0net::SumOk:
                navCoolUntil = navNow() + e0net::kSummonCooldown;
                navPendKind = s.kind; navPendAt = navNow();
                std::snprintf(t, sizeof t, "%s CHEGOU: A %d M A FRENTE", nm, (int)s.dist); break;
            case e0net::SumNoWater: std::snprintf(t, sizeof t, "SEM MAR ABERTO A FRENTE (VIRE PARA A AGUA)"); break;
            case e0net::SumInUse: std::snprintf(t, sizeof t, "%s EM USO POR OUTRO JOGADOR", nm); break;
            case e0net::SumCooldown: navCoolUntil = navNow() + (double)s.dist; std::snprintf(t, sizeof t, "AGUARDE %d S PARA CHAMAR DE NOVO", (int)s.dist); break;
            case e0net::SumSinking: std::snprintf(t, sizeof t, "%s AFUNDADO: RENASCE SOZINHO EM BREVE", nm); break;
            case e0net::SumAboard: std::snprintf(t, sizeof t, "DESCA DO NAVIO PARA CHAMAR OUTRO"); break;
            case e0net::SumNotOwned: std::snprintf(t, sizeof t, "VOCE AINDA NAO TEM O %s: COMPRE NA NAVEGACAO", nm); break;
            case e0net::SumBusy: std::snprintf(t, sizeof t, "NAO DA PARA CHAMAR AGORA"); break;
            default: std::snprintf(t, sizeof t, "NAVIO INDISPONIVEL"); break;
        }
        notify(t);
    }
}

// cor de destaque de cada navio na arte do cartão (navio, galeão, chalupa, bergantim)
static void navAccent(int i, float& r, float& g, float& b) {
    static const float k[4][3] = {{0.62f, 0.38f, 0.16f}, {0.92f, 0.68f, 0.18f}, {0.14f, 0.62f, 0.66f}, {0.70f, 0.10f, 0.12f}};
    r = k[i & 3][0]; g = k[i & 3][1]; b = k[i & 3][2];
}
void drawNavCardShapes(const NavGeo& G, int i) {
    const float u = G.u;
    const Rect2D c = navCard(G, i);
    const float cx = c.x + c.w * 0.5f;
    int extra; const int st = navStatus(i, extra);
    const bool ok = st == 0 || st == 6;
    const e0net::ShipDef& D = e0net::shipDef(i);
    float ar, ag, ab; navAccent(i, ar, ag, ab);
    const float sat = ok ? 1.0f : 0.35f;
    ar = ar * sat + 0.36f * (1.0f - sat); ag = ag * sat + 0.32f * (1.0f - sat); ab = ab * sat + 0.28f * (1.0f - sat);
    renderer.uiWoodPanel(cx, c.y + c.h * 0.5f, c.w * 0.5f, c.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, (float)i + 1.0f);
    // arte do navio: janela emoldurada com tom náutico quente e brilho central
    const float artH = 374.0f * u, ay = c.y + 18.0f * u + artH * 0.5f;
    renderer.uiGrad(cx, ay, c.w * 0.5f - 18.0f * u, artH * 0.5f, 10.0f * u, 0.055f, 0.045f, 0.036f, 1.0f, ar * 0.48f, ag * 0.44f, ab * 0.42f, 1.0f, 0.0f, 1.0f, 0.55f, (float)i * 7.0f);
    renderer.uiRoundRectBorder(cx, ay, c.w * 0.5f - 18.0f * u, artH * 0.5f, 10.0f * u, 1.6f * u, theme::kBrassDim.r, theme::kBrassDim.g, theme::kBrassDim.b, 0.70f);
    renderer.uiGlow(cx, ay + 6.0f * u, c.w * 0.38f, artH * 0.40f, ar, ag, ab, 0.22f * sat + 0.05f);
    static const int kIcon[4] = {HiPirateFlag, HiGalleon, HiBoat, HiSkull};
    renderer.uiIconTint(kIcon[i & 3], cx, ay, 126.0f * u, ok ? theme::kCream.r : 0.68f, ok ? theme::kCream.g : 0.64f, ok ? theme::kCream.b : 0.56f, ok ? 1.0f : 0.62f);
    renderer.uiDivider(cx, c.y + 474.0f * u, c.w * 0.36f, 5.5f * u, 0.65f);
    // barras dos atributos (relativas ao melhor navio)
    float maxSpd = 0.0f, maxHp = 0.0f;
    for (int k = 0; k < e0net::kMaxShips; ++k) { maxSpd = std::max(maxSpd, e0net::shipDef(k).maxFwd); maxHp = std::max(maxHp, (float)e0net::shipDef(k).maxHp); }
    const float bx = c.x + 34.0f * u, bw = c.w - 68.0f * u, bh = 12.0f * u;
    const float vals[2] = {D.maxFwd / std::max(0.1f, maxSpd), (float)D.maxHp / std::max(1.0f, maxHp)};
    for (int r = 0; r < 2; ++r) {
        const float by = c.y + 528.0f * u + (float)r * 78.0f * u;
        renderer.uiRoundRect(bx + bw * 0.5f, by, bw * 0.5f, bh * 0.5f, bh * 0.5f, 0.06f, 0.045f, 0.032f, 0.85f);
        renderer.uiRoundRectBorder(bx + bw * 0.5f, by, bw * 0.5f, bh * 0.5f, bh * 0.5f, 1.2f * u, theme::kBrassDim.r, theme::kBrassDim.g, theme::kBrassDim.b, 0.55f);
        const float fw = std::max(bh, bw * vals[r]);
        renderer.uiGrad(bx + fw * 0.5f, by, fw * 0.5f, bh * 0.5f, bh * 0.5f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, ok ? 0.96f : 0.45f, 0.66f, 0.46f, 0.16f, ok ? 0.96f : 0.45f, 1.0f, 0.0f);
    }
    // botão: latão batido se disponível; lacre escuro se indisponível
    const Rect2D bt = navBtn(G, i);
    renderer.uiButton(bt.x + bt.w * 0.5f, bt.y + bt.h * 0.5f, bt.w * 0.5f, bt.h * 0.5f, 12.0f * u, ok ? theme::BtnBrass : theme::BtnDisabled, 1.0f);
}
void drawNavCardText(const NavGeo& G, int i) {
    const float u = G.u;
    const Rect2D c = navCard(G, i);
    const e0net::ShipDef& D = e0net::shipDef(i);
    const e0net::ShipState& S = e0net::shipState(i);
    const e0net::PlayerState me = net.predicted();
    int extra; const int st = navStatus(i, extra);
    const bool ok = st == 0 || st == 6;
    const float dim = ok ? 1.0f : 0.6f;
    const int t7 = std::max(3, (int)std::lround(7.0f * u)), t4 = std::max(2, (int)std::lround(4.0f * u)), t5 = std::max(2, (int)std::lround(5.0f * u));
    {   // nome
        int t = t7; while (t > 2 && Renderer::textWidth(D.name, t) > c.w - 60.0f * u) --t;
        shopBold(c.x + (c.w - Renderer::textWidth(D.name, t)) * 0.5f, c.y + 406.0f * u, t, D.name, 1.0f * dim, 0.86f * dim, 0.40f * dim, 1.0f);
    }
    auto row = [&](float y, const char* lab, const char* val) {   // rótulo à esquerda, valor à direita (mesma linha)
        renderer.text(c.x + 34.0f * u, y - 3.5f * (float)t4, t4, lab, 0.62f, 0.60f, 0.56f, 1.0f, false);
        renderer.text(c.x + c.w - 34.0f * u - Renderer::textWidth(val, t4), y - 3.5f * (float)t4, t4, val, 0.92f * dim, 0.88f * dim, 0.78f * dim, 1.0f, false);
    };
    char v1[24], v2[24], v4[48];
    std::snprintf(v1, sizeof v1, "%d N\xD3S", (int)std::lround(D.maxFwd * 1.944f));
    std::snprintf(v2, sizeof v2, "%d", D.maxHp);
    if (S.exists) std::snprintf(v4, sizeof v4, "A %d M", (int)std::hypot(S.x - me.x, S.z - me.z)); else std::snprintf(v4, sizeof v4, "NO PORTO");
    row(c.y + 500.0f * u, "VELOCIDADE", v1);
    row(c.y + 578.0f * u, "VIDA", v2);
    row(c.y + 640.0f * u, "CANH\xD5" "ES", D.hasCannons ? "SIM" : "N\xC3" "O");
    row(c.y + 690.0f * u, "ONDE EST\xC1", v4);
    const Rect2D bt = navBtn(G, i);
    char lb[48];
    if (st == 4) std::snprintf(lb, sizeof lb, "AGUARDE %d S", extra); else if (st == 6) std::snprintf(lb, sizeof lb, "COMPRAR  %d", extra); else std::snprintf(lb, sizeof lb, "%s", navStatusText(st));
    int t = t5; while (t > 2 && Renderer::textWidth(lb, t) > bt.w - 40.0f * u) --t;
    shopBold(bt.x + (bt.w - Renderer::textWidth(lb, t)) * 0.5f, bt.y + (bt.h - 7.0f * (float)t) * 0.5f, t, lb, ok ? 0.16f : 1.0f, ok ? 0.10f : 0.62f, ok ? 0.02f : 0.52f, 1.0f, false);
}
void drawNavScreen() {
    const NavGeo G = navGeo();
    const float u = G.u;
    renderer.beginUi();
    renderer.uiWoodPanel((float)w * 0.5f, (float)h * 0.5f, (float)w * 0.5f, (float)h * 0.5f, 0.0f, theme::PanelBgWood, 1.0f, 4.0f);
    renderer.uiGlow((float)w * 0.5f, (float)h * 0.52f, (float)w * 0.6f, (float)h * 0.6f, 0.92f, 0.62f, 0.26f, 0.07f);
    renderer.uiIconTint(HiWheel, 74.0f * u, 68.0f * u, 36.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    renderer.uiDivider(420.0f * u, 130.0f * u, 290.0f * u, 6.0f * u, 0.72f);
    closeBtnShapes(G.closeX, G.closeY, G.closeR);
    renderer.endUi();
    renderer.beginText();
    {
        const int t6 = std::max(3, (int)std::lround(6.0f * u)), t3 = std::max(2, (int)std::lround(3.0f * u));
        shopBold(130.0f * u, 30.0f * u, t6, "NAVEGA\xC7\xC3" "O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        renderer.text(130.0f * u, 30.0f * u + 7.0f * (float)t6 + 18.0f * u, t3, "CHAME UM NAVIO PARA O MAR \xC0 SUA FRENTE", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);
    }
    renderer.endText();
    // cartões (rolam na horizontal; recortados na área)
    const Rect2D& a = G.area;
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)a.x, (GLint)((float)h - a.y - a.h), (GLsizei)a.w, (GLsizei)a.h);
    renderer.beginUi();
    for (int i = 0; i < e0net::kMaxShips; ++i) { const Rect2D c = navCard(G, i); if (c.x + c.w > a.x && c.x < a.x + a.w) drawNavCardShapes(G, i); }
    renderer.endUi();
    renderer.beginText();
    for (int i = 0; i < e0net::kMaxShips; ++i) { const Rect2D c = navCard(G, i); if (c.x + c.w > a.x && c.x < a.x + a.w) drawNavCardText(G, i); }
    renderer.endText();
    glDisable(GL_SCISSOR_TEST);
    // barra de rolagem + aviso
    renderer.beginUi();
    const float maxS = navMaxScroll(G);
    if (maxS > 1.0f) {
        const float len = a.w, view = len / (len + maxS), pos = std::max(0.0f, std::min(1.0f, navScroll / maxS));
        const float bl = std::max(60.0f * u, len * view), off = (len - bl) * pos, by = a.y + a.h + 26.0f * u;
        renderer.uiRoundRect(a.x + a.w * 0.5f, by, a.w * 0.5f, 3.0f * u, 3.0f * u, 1.0f, 1.0f, 1.0f, 0.08f);
        renderer.uiRoundRect(a.x + off + bl * 0.5f, by, bl * 0.5f, 3.0f * u, 3.0f * u, 1.0f, 0.78f, 0.25f, 0.85f);
    }
    Rect2D tr{(float)w * 0.40f, G.closeY - 34.0f * u, G.closeX - G.closeR - 24.0f * u - (float)w * 0.40f, 68.0f * u};
    const float ta = std::min(1.0f, navMsgT * 3.0f);
    if (navMsgT > 0.0f) {
        renderer.uiRoundRect(tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f, tr.w * 0.5f, tr.h * 0.5f, tr.h * 0.5f, 0.06f, 0.05f, 0.03f, 0.96f * ta);
        renderer.uiRoundRectBorder(tr.x + tr.w * 0.5f, tr.y + tr.h * 0.5f, tr.w * 0.5f, tr.h * 0.5f, tr.h * 0.5f, 2.5f * u, 1.0f, 0.78f, 0.25f, ta);
    }
    renderer.endUi();
    if (navMsgT > 0.0f) {
        renderer.beginText();
        int t = std::max(2, (int)std::lround(5.0f * u)); while (t > 2 && Renderer::textWidth(navMsg.c_str(), t) > tr.w - 50.0f * u) --t;
        shopBold(tr.x + (tr.w - Renderer::textWidth(navMsg.c_str(), t)) * 0.5f, tr.y + (tr.h - 7.0f * (float)t) * 0.5f, t, navMsg.c_str(), 1.0f, 0.85f, 0.35f, ta, false);
        renderer.endText();
    }
}
