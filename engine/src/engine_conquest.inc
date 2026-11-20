// Isolated - DOMINAÇÃO DE ILHAS no app (incluído dentro de struct Engine; regras em shared/conquest.h, autoridade no servidor).
//  * Mastro em cada ilha do oceano (conquestPoint: o mesmo ponto do servidor), com a bandeira da tripulação dona tremulando em 3D;
//    durante a disputa a bandeira sobe/desce no mastro conforme o progresso (MsgIslandCap).
//  * Painel no topo quando você está perto do mastro: dona, estado (hasteando, arriando, contestada, protegida) e a barra.
//  * Bandeiras das ilhas dominadas no minimapa e no mapa grande; avisos de dominação, ataque e renda.

struct IslePt { int state = 0; float x = 0, y = 0, z = 0; };   // state: 0 a calcular, 1 ok, -1 ilha sem lugar
IslePt islePt[e0net::kIslandSlots];
const e0net::World* islePtW[e0net::kIslandSlots] = {};
int conqSlot = -1;              // ilha do painel
float conqA = 0.0f;             // alfa do painel
float conqDist = 1e9f;          // distância até o mastro dessa ilha
std::vector<IslandFlagDraw> isleDraw;
int conqLostSlot = -1; double conqLostAt = -1e9;

bool islePoint(int slot, float& x, float& y, float& z) const {
    if (!e0net::islandConquerable(slot) || islePt[slot].state != 1 || islePtW[slot] != net.island(slot)) return false;
    x = islePt[slot].x; y = islePt[slot].y; z = islePt[slot].z; return true;
}
uint32_t myTeamId() const { const NetClient::TeamInfo& t = net.teamInfo(net.myId()); return t.known ? t.team : 0; }
static bool flagBytesValid(const uint8_t* b) { for (int i = 0; i < e0net::kFlagBytes; ++i) if (b[i]) return true; return false; }

void updateConquest(float dt) {
    if (!net.worldReady()) { isleDraw.clear(); renderer.setIslandFlags(isleDraw); conqSlot = -1; conqA = 0.0f; return; }
    // pontos dos mastros: no máx. 1 cálculo por quadro (busca inteira na grade da ilha, alguns ms)
    bool did = false;
    for (int s = 1; s < e0net::kIslandSlots; ++s) {
        const e0net::World* w = net.island(s);
        if (w != islePtW[s]) { islePtW[s] = w; islePt[s].state = 0; }
        if (!w || islePt[s].state != 0 || did) continue;
        int32_t xc, zc;
        if (e0net::conquestPoint(*w, xc, zc)) { islePt[s].x = xc * 0.01f; islePt[s].z = zc * 0.01f; islePt[s].y = w->heightAt(islePt[s].x, islePt[s].z); islePt[s].state = 1; }
        else islePt[s].state = -1;
        did = true;
    }
    // mastros para desenhar (perto da câmera) + qual painel mostrar
    const e0net::PlayerState me = net.visual();
    isleDraw.clear();
    int near = -1; float nearD = 1e9f;
    for (int s = 1; s < e0net::kIslandSlots; ++s) {
        float x, y, z; if (!islePoint(s, x, y, z)) continue;
        const float d = std::hypot(x - me.x, z - me.z);
        if (d < nearD) { nearD = d; near = s; }
        if (d > 650.0f) continue;
        IslandFlagDraw f; f.base = {x, y, z}; f.seed = (float)s * 1.7f;
        const NetClient::IsleCap& C = net.isleCap(s);
        const NetClient::IsleOwner& O = net.isleOwner(s);
        const uint8_t* fb = nullptr;
        if (net.isleCapAge(s) < 2.0 && C.pole) { fb = C.poleFlag; f.prog = C.prog; }        // ao vivo: a bandeira que está no mastro, na altura da disputa
        else if (O.team) { fb = O.flag; f.prog = 1.0f; }
        FlagDesign fd;
        if (fb && flagBytesValid(fb) && flagFromBytes(fb, fd)) { f.cloth = true; f.look = flagLook(fd); f.key.assign((const char*)fb, e0net::kFlagBytes); }
        isleDraw.push_back(f);
    }
    renderer.setIslandFlags(isleDraw);
    const int here = e0net::terrain().slotAt(me.x, me.z);   // a linha de texto vale em qualquer ponto de uma ilha do oceano
    const bool show = e0net::islandConquerable(here) && !selfDead && !uiFull() && !isleMode;
    if (show) { conqSlot = here; float hx, hy, hz; conqDist = islePoint(here, hx, hy, hz) ? std::hypot(hx - me.x, hz - me.z) : 1e9f; }
    else conqDist = near > 0 ? nearD : 1e9f;
    conqA = std::max(0.0f, std::min(1.0f, conqA + (show ? 5.0f : -4.0f) * dt));
    if (conqA <= 0.0f) conqSlot = -1;
    // avisos do servidor
    for (NetClient::IsleEvent e; net.popIsleEvent(e); ) {
        const e0net::World* w = net.island(e.slot);
        const std::string nm = e.slot ? e0net::islandLabel(e.slot, w ? w->kind : (uint8_t)e0net::IslandBig) : std::string();
        char t[128];
        const uint32_t mine = myTeamId();
        switch (e.kind) {
            case e0net::IeCaptured:   // value = tripulação que dominou (para todos)
                if (e.slot == conqLostSlot && teamNow() - conqLostAt < 3.0) continue;   // quem perdeu já recebeu o IeLost
                if (mine && (uint32_t)e.value == mine) { std::snprintf(t, sizeof t, "SUA TRIPULA\xC7\xC3O DOMINOU %s!", nm.c_str()); rig.addTrauma(0.15f); }
                else std::snprintf(t, sizeof t, "%s DOMINOU %s", e.name.c_str(), nm.c_str());
                break;
            case e0net::IeLost: conqLostSlot = e.slot; conqLostAt = teamNow(); std::snprintf(t, sizeof t, "PERDEMOS %s PARA %s", nm.c_str(), e.name.c_str()); break;
            case e0net::IeIncome: std::snprintf(t, sizeof t, "ILHAS: +%d MOEDAS NO COFRE DA TRIPULA\xC7\xC3O", (int)e.value); break;
            case e0net::IeUnderAttack: std::snprintf(t, sizeof t, "%s SOB ATAQUE DE %s!", nm.c_str(), e.name.c_str()); break;
            default: continue;
        }
        notify(t);
    }
}

// LINHA DE TEXTO da dominação (passe de texto), sem fundo, no canto superior direito logo abaixo da vida/fome (onde o painel de debug ficava).
// A COR diz o estado: verde = ilha livre, vermelho = dominada por outra tripulação (ou sob ataque), dourado = da sua tripulação.
void drawConquestHud() {
    if (conqSlot <= 0 || conqA <= 0.0f || uiFull() || isleMode || selfDead) return;
    const float a = conqA;
    const NetClient::IsleCap& C = net.isleCap(conqSlot);
    const NetClient::IsleOwner& O = net.isleOwner(conqSlot);
    const bool live = net.isleCapAge(conqSlot) < 2.0;
    const uint32_t mine = myTeamId();
    const uint32_t owner = live ? C.owner : O.team;
    const e0net::World* W = net.island(conqSlot);
    const bool inArea = conqDist <= e0net::kCapRadius;
    const bool isMine = owner && owner == mine;
    const int inc = e0net::islandIncome(W ? W->kind : (uint8_t)e0net::IslandBig) + e0net::structIncome(conqSlot);   // + marcenaria/armazém
    char t[128]; float r = 0.55f, g = 0.95f, b = 0.50f;   // verde = livre
    if (owner) {
        if (isMine) { std::snprintf(t, sizeof t, "RECEBE %d MOEDAS A CADA 20 MIN", inc); r = 1.0f; g = 0.84f; b = 0.30f; }
        else { std::snprintf(t, sizeof t, "DOMINADA POR %s", live && !C.ownerName.empty() ? C.ownerName.c_str() : "OUTRA TRIPULA\xC7\xC3O"); r = 0.98f; g = 0.40f; b = 0.34f; }
    } else std::snprintf(t, sizeof t, "ILHA LIVRE");
    if (live && net.connected()) {   // disputa em andamento: o estado (com a porcentagem) toma o lugar da linha
        const int pct = (int)std::lround((C.state == e0net::CapLowering ? 1.0f - C.prog : C.prog) * 100.0f);
        switch (C.state) {
            case e0net::CapShielded: if (isMine) { std::snprintf(t, sizeof t, "PROTEGIDA POR %d:%02d", C.shield / 60, C.shield % 60); r = 0.60f; g = 0.82f; b = 1.0f; } break;
            case e0net::CapContested: std::snprintf(t, sizeof t, "CONTESTADA! %d x %d", (int)C.nAtt, (int)C.nDef); r = 1.0f; g = 0.45f; b = 0.30f; break;
            case e0net::CapLowering:
                if (isMine) std::snprintf(t, sizeof t, "SUA BANDEIRA EST\xC1 SENDO ARRIADA! DEFENDA");
                else std::snprintf(t, sizeof t, "ARRIANDO A BANDEIRA %d%%", pct);
                r = 1.0f; g = 0.45f; b = 0.30f; break;
            case e0net::CapRaising:
                if (C.pole && C.pole == mine) { std::snprintf(t, sizeof t, "HASTEANDO SUA BANDEIRA %d%%", pct); r = 1.0f; g = 0.84f; b = 0.30f; }
                else { std::snprintf(t, sizeof t, "%s EST\xC1 DOMINANDO %d%%", C.poleName.empty() ? "OUTRA TRIPULA\xC7\xC3O" : C.poleName.c_str(), pct); r = 1.0f; g = 0.45f; b = 0.30f; }
                break;
            default: break;
        }
    }
    if (!mine && !owner && inArea && net.connected() && (!live || C.state == e0net::CapIdle)) std::snprintf(t, sizeof t, "ILHA LIVRE - ENTRE NUMA TRIPULA\xC7\xC3O");
    const float s = (float)uiScale();
    int ts = std::max(2, uiScale());
    const float maxW = (float)w * 0.46f;
    while (ts > 2 && Renderer::textWidth(t, ts) > maxW) --ts;
    renderer.text((float)w - 10.0f * s - Renderer::textWidth(t, ts), 37.0f * s, ts, t, r, g, b, a);
}

// bandeirinha de ilha dominada no mapa (uiFlag; chamar entre beginUi/endUi)
void drawIsleMapFlag(int slot, float sx, float sy, float size, float alpha) {
    const NetClient::IsleOwner& O = net.isleOwner(slot);
    FlagDesign fd;
    if (!O.team || !flagBytesValid(O.flag) || !flagFromBytes(O.flag, fd)) return;
    const float hw = size, hh = size / 1.5f, pole = size * 1.35f;
    const bool mine = O.team == myTeamId() && O.team;
    renderer.uiRoundRect(sx, sy - pole * 0.5f, std::max(1.0f, size * 0.07f), pole * 0.5f, 0.0f, 0.18f, 0.11f, 0.05f, 0.95f * alpha);   // mastro
    renderer.uiRoundRect(sx + hw + 1.5f, sy - pole + hh + 1.5f, hw, hh, 1.0f, 0.0f, 0.0f, 0.0f, 0.40f * alpha);                      // sombra
    renderer.uiFlag(flagLook(fd), sx + hw, sy - pole + hh, hw, hh, (float)std::fmod(teamNow(), 1000.0), 0.25f, alpha);
    if (mine) renderer.uiShape(Renderer::UiRing, sx, sy, size * 0.32f, 0.30f, 0.85f, 0.40f, 0.95f * alpha);   // da minha tripulação: anel verde no pé
}
