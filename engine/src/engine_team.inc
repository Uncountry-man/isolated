// ================= TRIPULAÇÃO (Quartel-General / Painel com Seções) =================
// Incluído DENTRO de `struct Engine` (engine.cpp). Abre pelo Menu Principal ou pelo botão da HUD no jogo.
// Fala com o servidor pelo serviço TCP da tripulação (shared/team.h; TeamClient faz os pedidos numa thread).
// Estrutura em painel com barra lateral de navegação e 4 seções:
//   0) VISÃO GERAL   : KPIs reais (membros, online, código, diário), últimas do diário, tripulantes ativos (ou entrar/fundar).
//   1) MEMBROS       : lista rolável com filtros (TODOS / NO MAR) + ficha lateral de inspeção (distância, quadrante, rota, promover, expulsar).
//   2) DIÁRIO & CHAT : chat em largura total, histórico 24h, rolagem suave e ordens rápidas náuticas de 1 toque.
//   4) COFRE & LOJA  : cofre da tripulação (todos depositam, só o capitão saca) e loja paga com o cofre (vagas e navios para todos os tripulantes).
//   3) ESTANDARTE    : bandeira grande no mastro, editor, nome, licença (kTeamLicenseCost), código de recrutamento e gestão/saída.

TeamClient teamNet;
bool teamOpen = false;
e0net::TeamView teamV;                 // último estado vindo do servidor
bool teamHave = false;                 // já chegou uma resposta
int teamConn = 0;                      // 0 conectando, 1 ok, 2 sem servidor/sem resposta, 3 build offline
struct TeamMsgL { e0net::TeamMsg m; double at; };   // at = relógio local em que a idade m.age foi medida
std::vector<TeamMsgL> teamMsgs;
uint32_t teamSince = 0, teamCurId = 0;
std::string teamDraftName;             // nome digitado antes de criar
FlagDesign teamDraftFlag;              // bandeira escolhida antes de criar
bool teamDraftFlagSet = false;
float teamT = 0.0f, teamPollT = 0.0f, teamMsgT = 0.0f;
std::string teamMsg;
int teamInput = 0;                     // caixa de texto aberta: 1 nome, 2 chat, 3 código
int teamWaitOp = -1;                   // operação (não TeamGet) esperando resposta
e0net::TeamRequest teamLastReq; bool teamRetried = false, teamTokenWait = false; float teamTokenT = 0.0f; std::string teamTokenBad;
int teamSec = 0;                       // seção ativa: 0 Visão Geral, 1 Membros, 2 Diário & Chat, 3 Estandarte & Gestão
int teamMemFilter = 0;                 // 0 = todos, 1 = apenas online
int teamSel = -1, teamActArm = -1; float teamArmT = 0.0f; bool teamLeaveArm = false;
int wpCrew = -1;                       // ROTA seguindo um tripulante (id no jogo)
float teamChatScroll = 0.0f, teamChatVel = 0.0f;
float teamMemScroll = 0.0f, teamMemVel = 0.0f;
int teamPtrMode = 0;                   // 1 = arrastando chat, 2 = arrastando lista de membros
bool teamFlagGesture = false;
float teamDownX = 0, teamDownY = 0, teamLastY = 0; bool teamMoved = false; double teamLastT = 0;
FlagDesign flagOwnBackup; int flagMode = 0;   // editor de bandeira: 0 = jogador, 1 = tripulação

static double teamNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void teamSay(const std::string& m, float t = 2.6f) { teamMsg = m; teamMsgT = t; }
bool teamLocked() const { return teamHave && !teamV.inTeam && !teamV.license; }

static std::string latin1ToUtf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) { if (c < 0x80) o += (char)c; else { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); } }
    return o;
}
std::string teamPlayerName() const {
    const Auth::Account& A = auth.account();
    std::string n = utf8ToLatin1(A.name);
    const size_t sp = n.find(' '); if (sp != std::string::npos && sp >= 3) n = n.substr(0, sp);
    n = e0net::teamCleanText(n, e0net::kPlayerNameMax);
    if (n.empty()) {
        std::string tail = A.uid.size() >= 4 ? A.uid.substr(A.uid.size() - 4) : A.uid;
        for (char& c : tail) c = (char)std::toupper((unsigned char)c);
        n = "PIRATA " + tail;
    }
    return n;
}
bool teamServer(std::string& ip, int& port) const {
    const std::string host = serverOverride.empty() ? builtinServer() : serverOverride;
    port = e0net::kDefaultPort;
    return !host.empty() && MainMenu::parseServer(host, ip, port);
}
bool teamSend(e0net::TeamRequest r) {
    if (!auth.signedIn()) { teamSay("ENTRE NUMA CONTA PRIMEIRO"); return false; }
    const Auth::Account& A = auth.account();
    if (A.cloud) {
        if (auth.idToken().empty()) { if (!teamTokenWait) auth.refreshToken(); teamTokenWait = true; teamTokenT = 12.0f; teamTokenBad.clear(); teamLastReq = r; return true; }
        r.authKind = e0net::LoginFirebase; r.token = auth.idToken();
    } else { r.authKind = e0net::LoginLocal; r.uid = A.uid; }
    r.playerName = teamPlayerName();
    if (r.op != e0net::TeamGet && r.since == 0) r.since = teamSince;
    if (r.op == e0net::TeamGet) r.since = teamSince;
    if (r.op != e0net::TeamGet) { teamWaitOp = r.op; teamLastReq = r; }
    teamNet.send(r);
    return true;
}
void teamOp(uint8_t op, const std::string& text = "", const uint8_t* flag = nullptr, uint8_t index = 0, uint32_t amount = 0) {
    if (teamWaitOp >= 0) { teamSay("AGUARDE..."); return; }
    if (teamConn == 3) { teamSay("MODO LOCAL: A TRIPULA\xC7\xC3O PRECISA DO SERVIDOR"); return; }
    e0net::TeamRequest r; r.op = op; r.text = text; r.index = index; r.amount = amount;
    if (flag) std::memcpy(r.flag, flag, e0net::kFlagBytes);
    teamRetried = false;
    teamSend(r);
}
void teamShareInvite() {
    if (!teamV.inTeam) return;
    const std::string t = "Entre na minha tripula\xC3\xA7\xC3\xA3o \"" + latin1ToUtf8(teamV.name) + "\" no Isolated! C\xC3\xB3" "digo: " + teamV.code;
    if (!e0::shareText(app->activity, t)) teamSay("C\xD3" "DIGO: " + teamV.code);
}
void openTeam() {
    teamOpen = true; teamT = 0.0f; teamPollT = 0.0f; teamMsgT = 0.0f; teamInput = 0; teamWaitOp = -1;
    teamSel = -1; teamActArm = -1; teamLeaveArm = false; teamSec = 0; teamMemFilter = 0;
    teamChatScroll = 0.0f; teamChatVel = 0.0f; teamMemScroll = 0.0f; teamMemVel = 0.0f; teamPtrMode = 0; teamTokenWait = false;
    if (!teamDraftFlagSet) { teamDraftFlag = flagSaved; teamDraftFlagSet = true; }
    wheelOpen = false; joyT.id = -1; lookT.id = -1; teamUnread = 0;
    std::string ip; int port;
    if (teamServer(ip, port)) { teamNet.setServer(ip, port); teamConn = teamHave ? 1 : 0; e0net::TeamRequest r; r.op = e0net::TeamGet; teamSend(r); }
    else { teamNet.setServer("", 0); teamConn = 3; }
    LOG("tela TRIPULACAO aberta (%s)", teamConn == 3 ? "sem servidor" : ip.c_str());
}
void closeTeam() {
    if (flagOpen) closeFlag();
    teamOpen = false; teamPtrMode = 0;
}
void teamFlagSaved() {
    uint8_t b[e0net::kFlagBytes]; std::memcpy(b, &flagSaved, e0net::kFlagBytes);
    if (teamV.inTeam) { if (teamV.owner) teamOp(e0net::TeamSetFlag, "", b); }
    else { teamDraftFlag = flagSaved; teamDraftFlagSet = true; }
}
void openTeamFlag() {
    FlagDesign cur = teamDraftFlag;
    if (teamV.inTeam) { FlagDesign fd; if (flagFromBytes(teamV.flag, fd)) cur = fd; }
    flagOwnBackup = flagSaved; flagMode = 1;
    flagSaved = cur;
    openFlag();
}
int teamOnlineCount() const {
    int n = 0; for (const auto& m : teamV.members) if (m.online) ++n;
    return n;
}
std::string teamCaptainName() const {
    for (const auto& m : teamV.members) if (m.owner) return m.name;
    return "--";
}
// índices dos membros ordenados (online primeiro, depois capitão) respeitando teamMemFilter
std::vector<int> teamFilteredMembers(int filterMode) const {
    std::vector<int> idx;
    for (int i = 0; i < (int)teamV.members.size(); ++i) {
        if (filterMode == 1 && !teamV.members[(size_t)i].online) continue;
        idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        const auto& ma = teamV.members[(size_t)a]; const auto& mb = teamV.members[(size_t)b];
        if (ma.online != mb.online) return ma.online > mb.online;
        if (ma.owner != mb.owner) return ma.owner > mb.owner;
        return a < b;
    });
    return idx;
}

void teamApply(const TeamClient::Reply& R) {
    if (R.op == e0net::TeamPushToken) { if (!R.net || R.result != e0net::TeamOk) teamPushSent.clear(); return; }
    if (!R.net) {
        teamConn = 2;
        if (R.op != e0net::TeamGet) teamSay("SEM CONEX\xC3O COM O SERVIDOR");
        if (R.op == teamWaitOp) teamWaitOp = -1;
        return;
    }
    teamConn = 1;
    if (R.result == e0net::TeamErrExpired) {
        if (!teamRetried || R.op == e0net::TeamGet) { teamRetried = R.op != e0net::TeamGet; teamTokenWait = true; teamTokenT = 12.0f; teamTokenBad = R.req.token; teamLastReq = R.req; auth.refreshToken(); if (R.op == teamWaitOp) teamWaitOp = -1; return; }
    }
    if (R.result == e0net::TeamErrAuth || R.result == e0net::TeamErrExpired) {
        if (R.op == teamWaitOp) teamWaitOp = -1;
        teamSay(e0net::teamResultText(R.result)); return;
    }
    const e0net::TeamView& v = R.view;
    if (!v.inTeam || v.teamId != teamCurId) { teamMsgs.clear(); teamSince = 0; teamChatScroll = 0.0f; teamMemScroll = 0.0f; teamSel = -1; }
    teamCurId = v.inTeam ? v.teamId : 0;
    const double now = teamNow();
    for (const e0net::TeamMsg& m : v.msgs) if (m.id > teamSince) { teamMsgs.push_back({m, now}); teamSince = m.id; }
    if (v.inTeam && v.lastMsgId > teamSince) teamSince = v.lastMsgId;
    while (teamMsgs.size() > (size_t)e0net::kTeamMsgKeep) teamMsgs.erase(teamMsgs.begin());
    const bool wasIn = teamV.inTeam, wasOwner = teamV.owner;
    teamV = v; teamHave = true;
    if (teamSel >= (int)teamV.members.size()) teamSel = -1;
    if (R.op == teamWaitOp) teamWaitOp = -1;
    if (R.op == e0net::TeamGet) return;
    char t[96];
    if (R.result == e0net::TeamOk) {
        switch (R.op) {
            case e0net::TeamBuyLicense: teamSay("LICEN\xC7" "A COMPRADA: D\xCA UM NOME E FUNDE SUA TRIPULA\xC7\xC3O", 3.5f); teamSec = 3; break;
            case e0net::TeamCreate: teamSay("TRIPULA\xC7\xC3O FUNDADA: CONVIDE SEUS AMIGOS", 3.5f); teamSec = 0; break;
            case e0net::TeamJoin: std::snprintf(t, sizeof t, "VOC\xCA ENTROU EM %s", v.name.c_str()); teamSay(t, 3.5f); teamSec = 0; break;
            case e0net::TeamLeave: if (wasIn) teamSay(wasOwner ? "TRIPULA\xC7\xC3O DESFEITA" : "VOC\xCA SAIU DA TRIPULA\xC7\xC3O"); teamSec = 0; break;
            case e0net::TeamRename: teamSay("NOME ALTERADO"); break;
            case e0net::TeamSetFlag: teamSay("BANDEIRA DA TRIPULA\xC7\xC3O SALVA"); break;
            case e0net::TeamKick: teamSay("MEMBRO REMOVIDO"); teamSel = -1; break;
            case e0net::TeamPromote: teamSay("AGORA ELE \xC9 O CAPIT\xC3O"); teamSel = -1; break;
            case e0net::TeamPushToken: return;
            case e0net::TeamChat: teamChatScroll = 0.0f; break;
            case e0net::TeamDeposit: teamSay("DEPOSITADO NO COFRE"); break;
            case e0net::TeamWithdraw: teamSay("SACADO DO COFRE PARA A SUA CONTA"); break;
            case e0net::TeamBuy: teamSay("COMPRA FEITA COM O COFRE!", 3.5f); break;
            default: break;
        }
    } else if (R.result == e0net::TeamErrCoins && R.op == e0net::TeamBuyLicense) {
        std::snprintf(t, sizeof t, "MOEDAS INSUFICIENTES: VOC\xCA TEM %d DE %d", (int)v.coins, e0net::kTeamLicenseCost); teamSay(t, 3.5f);
    } else teamSay(e0net::teamResultText(R.result), 3.0f);
}

void updateTeam(float dt) {
    teamT += dt;
    if (teamMsgT > 0.0f) teamMsgT -= dt;
    if (teamArmT > 0.0f) { teamArmT -= dt; if (teamArmT <= 0.0f) { teamActArm = -1; teamLeaveArm = false; } }
    {
        Auth::Fail f;
        if (!netGame) auth.poll(f);
        if (teamTokenWait && !auth.idToken().empty() && auth.idToken() != teamTokenBad) {
            teamTokenWait = false; e0net::TeamRequest r = teamLastReq; r.token.clear(); teamWaitOp = -1;
            teamSend(r);
        }
        if (teamTokenWait && (teamTokenT -= dt) <= 0.0f) { teamTokenWait = false; teamWaitOp = -1; teamConn = teamHave ? 1 : 2; teamSay("N\xC3O FOI POSS\xCDVEL CONFIRMAR SUA CONTA", 3.5f); teamPollT = 10.0f; }
    }
    for (TeamClient::Reply R; teamNet.poll(R); ) teamApply(R);
    if (teamConn != 3 && !flagOpen) {
        teamPollT -= dt;
        if (teamPollT <= 0.0f && !teamNet.busy() && !teamTokenWait) { teamPollT = teamV.inTeam ? 3.0f : 6.0f; e0net::TeamRequest r; r.op = e0net::TeamGet; teamSend(r); }
    }
    if (teamInput) {
        std::string r;
        if (e0::chatInputPoll(app->activity, r)) {
            const int mode = teamInput; teamInput = 0;
            if (!r.empty() && r != "\x01") {
                const std::string l1 = utf8ToLatin1(r);
                if (mode == 1) {
                    const std::string nm = e0net::teamCleanText(l1, e0net::kTeamNameMax);
                    if ((int)nm.size() < e0net::kTeamNameMin) teamSay("NOME MUITO CURTO (M\xCDNIMO 3 LETRAS)");
                    else if (teamV.inTeam) teamOp(e0net::TeamRename, nm);
                    else teamDraftName = nm;
                } else if (mode == 2) teamOp(e0net::TeamChat, l1);
                else if (mode == 3) {
                    const std::string c = e0net::teamCleanCode(l1);
                    if ((int)c.size() != e0net::kTeamCodeLen) teamSay("O C\xD3" "DIGO TEM 6 LETRAS/N\xDAMEROS");
                    else teamOp(e0net::TeamJoin, c);
                }
            }
        }
    }
    const double now = teamNow();
    teamMsgs.erase(std::remove_if(teamMsgs.begin(), teamMsgs.end(), [&](const TeamMsgL& m) { return (double)m.m.age + (now - m.at) > (double)e0net::kTeamMsgTtl; }), teamMsgs.end());
    if (teamPtrMode != 1) {
        teamChatScroll += teamChatVel * dt; teamChatVel *= std::exp(-4.5f * dt);
        if (std::fabs(teamChatVel) < 5.0f) teamChatVel = 0.0f;
    }
    if (teamPtrMode != 2) {
        teamMemScroll += teamMemVel * dt; teamMemVel *= std::exp(-5.0f * dt);
        if (std::fabs(teamMemVel) < 5.0f) teamMemVel = 0.0f;
    }
}

// ---------- layout do Painel Completo com Sidebar + 4 Seções ----------
struct TeamGeo {
    float u;
    float closeX, closeY, closeR;
    Rect2D coinPill, side, main;
    // Sidebar
    float sfX, sfY, sfHW, sfHH;            // mini-bandeira no topo da sidebar
    Rect2D navBtn[5], sideFootBtn;
    // Seção 0: VISÃO GERAL
    Rect2D kpi[4], ovLeft, ovRight, ovChatBtn, ovMemBtn, ovEstBtn;
    Rect2D ovJoin, ovFound, ovJoinBtn, ovFoundBtn, ovPerk[3];
    // Seção 1: MEMBROS
    Rect2D memPane, memInspect, memFilt[2], memList, memInviteBtn;
    float memRowH;
    // Seção 2: DIÁRIO & CHAT
    Rect2D chatList, chatQuick[4], chatBar, chatSendBtn;
    // Seção 3: ESTANDARTE & GESTÃO
    Rect2D estLeft, estRight, flagBtn, nameBox, mainBtn, inviteBtn, leaveBtn, buyBtn;
    float efX, efY, efHW, efHH;
    // Seção 4: COFRE & LOJA
    Rect2D vaultL, vaultR, depBtn[4], wdBtn[4], shopRow[5], shopBuyBtn[5];
};
TeamGeo teamGeo() const {
    TeamGeo G{}; G.u = std::min((float)h / 1080.0f, (float)w / 1900.0f); const float u = G.u;
    const float top = 136.0f * u, bot = (float)h - 26.0f * u, gap = 24.0f * u;
    G.closeR = 40.0f * u; G.closeX = (float)w - 72.0f * u; G.closeY = 64.0f * u;
    G.coinPill = {G.closeX - G.closeR - 22.0f * u - 300.0f * u, G.closeY - 32.0f * u, 300.0f * u, 64.0f * u};

    // Sidebar esquerda + Área Principal direita
    const float sideW = std::min(430.0f * u, (float)w * 0.21f);
    G.side = {36.0f * u, top, sideW, bot - top};
    G.main = {G.side.x + G.side.w + gap, top, (float)w - 36.0f * u - (G.side.x + G.side.w + gap), bot - top};

    // Sidebar interna: brasão + 4 abas + rodapé
    G.sfHW = std::min(G.side.w * 0.26f, 92.0f * u); G.sfHH = G.sfHW / 1.5f;
    G.sfX = G.side.x + G.side.w * 0.5f; G.sfY = G.side.y + 84.0f * u;
    const float navX = G.side.x + 20.0f * u, navW = G.side.w - 40.0f * u, navH = 74.0f * u, navGap = 12.0f * u;
    const float navY0 = G.side.y + 258.0f * u;
    for (int i = 0; i < 5; ++i) G.navBtn[i] = {navX, navY0 + (float)i * (navH + navGap), navW, navH};
    G.sideFootBtn = {navX, bot - 20.0f * u - 64.0f * u, navW, 64.0f * u};

    // Seção 0: VISÃO GERAL
    const float kpiH = 146.0f * u, kpiGap = 18.0f * u;
    const float kpiW = (G.main.w - 3.0f * kpiGap) * 0.25f;
    for (int i = 0; i < 4; ++i) G.kpi[i] = {G.main.x + (float)i * (kpiW + kpiGap), G.main.y, kpiW, kpiH};
    const float ovY = G.main.y + kpiH + 22.0f * u, ovH = bot - ovY;
    const float ovLW = (G.main.w - gap) * 0.54f, ovRW = G.main.w - gap - ovLW;
    G.ovLeft = {G.main.x, ovY, ovLW, ovH};
    G.ovRight = {G.main.x + ovLW + gap, ovY, ovRW, ovH};
    G.ovChatBtn = {G.ovLeft.x + 26.0f * u, bot - 24.0f * u - 68.0f * u, G.ovLeft.w - 52.0f * u, 68.0f * u};
    const float rbW = (G.ovRight.w - 52.0f * u - 16.0f * u) * 0.5f;
    G.ovMemBtn = {G.ovRight.x + 26.0f * u, bot - 24.0f * u - 68.0f * u, rbW, 68.0f * u};
    G.ovEstBtn = {G.ovMemBtn.x + rbW + 16.0f * u, G.ovMemBtn.y, rbW, 68.0f * u};
    // Visão geral SEM tripulação
    const float topH = G.main.h * 0.66f, pGap = 22.0f * u;
    const float halfW = (G.main.w - gap) * 0.5f;
    G.ovJoin = {G.main.x, G.main.y, halfW, topH};
    G.ovFound = {G.main.x + halfW + gap, G.main.y, halfW, topH};
    G.ovJoinBtn = {G.ovJoin.x + 36.0f * u, G.ovJoin.y + G.ovJoin.h - 32.0f * u - 82.0f * u, G.ovJoin.w - 72.0f * u, 82.0f * u};
    G.ovFoundBtn = {G.ovFound.x + 36.0f * u, G.ovFound.y + G.ovFound.h - 32.0f * u - 82.0f * u, G.ovFound.w - 72.0f * u, 82.0f * u};
    const float pkY = G.main.y + topH + pGap, pkH = bot - pkY, pkW = (G.main.w - 2.0f * kpiGap) / 3.0f;
    for (int i = 0; i < 3; ++i) G.ovPerk[i] = {G.main.x + (float)i * (pkW + kpiGap), pkY, pkW, pkH};

    // Seção 1: MEMBROS
    const float memLW = (G.main.w - gap) * 0.57f, memRW = G.main.w - gap - memLW;
    G.memPane = {G.main.x, G.main.y, memLW, G.main.h};
    G.memInspect = {G.main.x + memLW + gap, G.main.y, memRW, G.main.h};
    const float fw = 176.0f * u, fh = 50.0f * u;
    G.memFilt[1] = {G.memPane.x + G.memPane.w - 24.0f * u - fw, G.memPane.y + 22.0f * u, fw, fh};
    G.memFilt[0] = {G.memFilt[1].x - 12.0f * u - fw, G.memPane.y + 22.0f * u, fw, fh};
    G.memList = {G.memPane.x + 22.0f * u, G.memPane.y + 98.0f * u, G.memPane.w - 44.0f * u, G.memPane.h - 120.0f * u};
    G.memRowH = 78.0f * u;
    G.memInviteBtn = {G.memInspect.x + 30.0f * u, bot - 28.0f * u - 76.0f * u, G.memInspect.w - 60.0f * u, 76.0f * u};

    // Seção 2: DIÁRIO & CHAT
    const float barH = 76.0f * u, qH = 50.0f * u, sendW = 220.0f * u;
    G.chatBar = {G.main.x + 26.0f * u, bot - 24.0f * u - barH, G.main.w - 52.0f * u - sendW - 14.0f * u, barH};
    G.chatSendBtn = {G.chatBar.x + G.chatBar.w + 14.0f * u, G.chatBar.y, sendW, barH};
    const float qY = G.chatBar.y - 14.0f * u - qH, qGap = 14.0f * u, qW = (G.main.w - 52.0f * u - 3.0f * qGap) * 0.25f;
    for (int i = 0; i < 4; ++i) G.chatQuick[i] = {G.main.x + 26.0f * u + (float)i * (qW + qGap), qY, qW, qH};
    G.chatList = {G.main.x + 24.0f * u, G.main.y + 104.0f * u, G.main.w - 48.0f * u, qY - 16.0f * u - (G.main.y + 104.0f * u)};

    // Seção 3: ESTANDARTE & GESTÃO
    const float estLW = (G.main.w - gap) * 0.45f, estRW = G.main.w - gap - estLW;
    G.estLeft = {G.main.x, G.main.y, estLW, G.main.h};
    G.estRight = {G.main.x + estLW + gap, G.main.y, estRW, G.main.h};
    G.efHW = std::min((G.estLeft.w - 84.0f * u) * 0.48f, 236.0f * u); G.efHH = G.efHW / 1.5f;
    G.efX = G.estLeft.x + G.estLeft.w * 0.5f; G.efY = G.estLeft.y + 140.0f * u + G.efHH;
    G.flagBtn = {G.estLeft.x + 36.0f * u, G.efY + G.efHH + 44.0f * u, G.estLeft.w - 72.0f * u, 78.0f * u};
    const float rx = G.estRight.x + 36.0f * u, rw = G.estRight.w - 72.0f * u;
    G.nameBox = {rx, G.estRight.y + 146.0f * u, rw, 84.0f * u};
    G.mainBtn = {rx, bot - 30.0f * u - 86.0f * u, rw, 86.0f * u};
    G.buyBtn = G.mainBtn;
    G.inviteBtn = {rx + rw - 240.0f * u, G.nameBox.y + G.nameBox.h + 56.0f * u, 240.0f * u, 72.0f * u};
    G.leaveBtn = {rx, bot - 30.0f * u - 78.0f * u, rw, 78.0f * u};

    // Seção 4: COFRE & LOJA
    const float vLW = (G.main.w - gap) * 0.40f, vRW = G.main.w - gap - vLW;
    G.vaultL = {G.main.x, G.main.y, vLW, G.main.h};
    G.vaultR = {G.main.x + vLW + gap, G.main.y, vRW, G.main.h};
    {
        const float bx = G.vaultL.x + 30.0f * u, bw = G.vaultL.w - 60.0f * u, bg = 12.0f * u, bwid = (bw - 3.0f * bg) * 0.25f, bh = 70.0f * u;
        const float depY = G.vaultL.y + 456.0f * u, wdY = depY + bh + 112.0f * u;
        for (int k = 0; k < 4; ++k) { G.depBtn[k] = {bx + (float)k * (bwid + bg), depY, bwid, bh}; G.wdBtn[k] = {bx + (float)k * (bwid + bg), wdY, bwid, bh}; }
        const float rx4 = G.vaultR.x + 24.0f * u, rw4 = G.vaultR.w - 48.0f * u, ry0 = G.vaultR.y + 112.0f * u;
        const float rh = (G.vaultR.h - 112.0f * u - 22.0f * u - 4.0f * 12.0f * u) / 5.0f;
        for (int k = 0; k < 5; ++k) {
            G.shopRow[k] = {rx4, ry0 + (float)k * (rh + 12.0f * u), rw4, rh};
            const float bwid2 = std::min(250.0f * u, rw4 * 0.30f);
            G.shopBuyBtn[k] = {rx4 + rw4 - bwid2 - 16.0f * u, G.shopRow[k].y + (rh - 66.0f * u) * 0.5f, bwid2, 66.0f * u};
        }
    }
    return G;
}
Rect2D teamToastRect(const TeamGeo& G) const {
    const float u = G.u;
    const float x0 = G.main.x + 12.0f * u, x1 = G.coinPill.x - 20.0f * u;
    return {x0, G.closeY - 30.0f * u, std::max(280.0f * u, x1 - x0), 60.0f * u};
}
int teamTs(float k) const { return std::max(2, (int)std::lround(k * std::min((float)h / 1080.0f, (float)w / 1900.0f))); }

// mensagens do chat (quebra de linhas para a seção 2)
float teamMsgHeight(const TeamGeo& G, const TeamMsgL& m, int& nl, char lines[][72]) const {
    const float u = G.u; const int t = teamTs(3.8f);
    const float maxW = std::min(G.chatList.w * 0.72f, 1100.0f * u) - 56.0f * u;
    nl = wrapLinesPx(m.m.text.c_str(), t, maxW, lines, 4);
    if (m.m.author.empty()) return (float)nl * (7.0f * (float)t + 10.0f * u) + 22.0f * u;
    return 7.0f * (float)teamTs(2.8f) + 12.0f * u + (float)nl * (7.0f * (float)t + 10.0f * u) + 28.0f * u;
}
float teamChatContentH(const TeamGeo& G) const {
    float hsum = 0.0f; char lines[4][72]; int nl;
    for (const TeamMsgL& m : teamMsgs) hsum += teamMsgHeight(G, m, nl, lines) + 14.0f * G.u;
    return hsum;
}
static void teamAge(char* out, size_t n, double age) {
    if (age < 60.0) std::snprintf(out, n, "AGORA");
    else if (age < 3600.0) std::snprintf(out, n, "%d MIN", (int)(age / 60.0));
    else std::snprintf(out, n, "%d H", (int)(age / 3600.0));
}
static const char* teamQuickMsg(int k) {
    static const char* kQ[4] = {"REUNIR NA ILHA!", "NAVIO PRONTO!", "PRECISO DE AJUDA!", "VAMOS ZARPAR!"};
    return kQ[std::max(0, std::min(3, k))];
}
// membro selecionado: ações possíveis (2 ROTA no jogo, 1 PROMOVER CAPITÃO, 0 EXPULSAR)
int teamMemberActs(int* out) const {
    if (!teamV.inTeam || teamSel < 0 || teamSel >= (int)teamV.members.size()) return 0;
    const e0net::TeamMember& m = teamV.members[(size_t)teamSel];
    if (m.me) return 0;
    int n = 0;
    if (netGame && net.connected() && m.online && m.slot >= 0) out[n++] = 2;
    if (teamV.owner) { out[n++] = 1; out[n++] = 0; }
    return n;
}
Rect2D teamInspectActRect(const TeamGeo& G, int k, int n) const {
    const float u = G.u, bh = 74.0f * u, gap = 14.0f * u;
    const float botY = G.memInspect.y + G.memInspect.h - 26.0f * u;
    const float y = botY - (float)(n - k) * bh - (float)(n - 1 - k) * gap;
    return {G.memInspect.x + 30.0f * u, y, G.memInspect.w - 60.0f * u, bh};
}
Rect2D teamOvMemberRect(const TeamGeo& G, int i) const {
    const float u = G.u;
    return {G.ovRight.x + 26.0f * u, G.ovRight.y + 102.0f * u + (float)i * 78.0f * u, G.ovRight.w - 52.0f * u, 68.0f * u};
}
bool teamCrewPos(int slot, float& x, float& z) const {
    if (slot < 0 || !net.connected()) return false;
    if (slot == net.myId()) { const e0net::PlayerState me = net.predicted(); x = me.x; z = me.z; return true; }
    for (const auto& p : remotes) if (p.id == slot) { x = p.x; z = p.z; return true; }
    if (net.teamPosAge() < 4.0) for (const auto& p : net.teamPositions()) if (p.id == slot) { x = p.x; z = p.z; return true; }
    return false;
}

// ---------- toques ----------
void teamTouch(int ty, float x, float y) {
    if (ty == 0) teamFlagGesture = flagOpen;
    if (teamFlagGesture) {
        if (flagOpen) { if (ty == 0) flagTouchDown(0, x, y); else if (ty == 1) flagTouchMove(0, x, y); else flagTouchUp(0, x, y, ty == 3); }
        if (ty >= 2) teamFlagGesture = false;
        return;
    }
    const TeamGeo G = teamGeo();
    if (ty == 0) {
        teamDownX = x; teamDownY = y; teamLastY = y; teamMoved = false; teamLastT = teamNow();
        teamPtrMode = 0;
        if (teamV.inTeam && teamSec == 2 && G.chatList.contains(x, y)) { teamPtrMode = 1; teamChatVel = 0.0f; }
        else if (teamV.inTeam && teamSec == 1 && G.memList.contains(x, y)) { teamPtrMode = 2; teamMemVel = 0.0f; }
        return;
    }
    if (ty == 1) {
        if (std::fabs(x - teamDownX) + std::fabs(y - teamDownY) > 16.0f * G.u) teamMoved = true;
        if (teamMoved && (teamPtrMode == 1 || teamPtrMode == 2)) {
            const double t = teamNow(); const float dtt = (float)std::max(1e-3, t - teamLastT);
            const float dy = y - teamLastY;
            if (teamPtrMode == 1) {
                teamChatScroll += dy;
                teamChatVel = teamChatVel * 0.4f + 0.6f * (dy / dtt);
                const float maxS = std::max(0.0f, teamChatContentH(G) - G.chatList.h);
                teamChatScroll = std::max(0.0f, std::min(maxS, teamChatScroll));
            } else {
                teamMemScroll -= dy;   // arrastar para cima avança a lista para baixo
                teamMemVel = teamMemVel * 0.4f + 0.6f * (-dy / dtt);
                const float total = (float)teamFilteredMembers(teamMemFilter).size() * G.memRowH;
                const float maxS = std::max(0.0f, total - G.memList.h);
                teamMemScroll = std::max(0.0f, std::min(maxS, teamMemScroll));
            }
            teamLastY = y; teamLastT = t;
        }
        return;
    }
    const int mode = teamPtrMode; teamPtrMode = 0;
    if (ty == 3) return;
    if (teamMoved) {
        if (mode == 1 && teamNow() - teamLastT > 0.08) teamChatVel = 0.0f;
        if (mode == 2 && teamNow() - teamLastT > 0.08) teamMemVel = 0.0f;
        return;
    }
    teamTap(G, x, y);
}

void teamTap(const TeamGeo& G, float x, float y) {
    { const float dx = x - G.closeX, dy = y - G.closeY; if (dx * dx + dy * dy <= G.closeR * G.closeR * 1.4f) { closeTeam(); return; } }
    if (teamInput) return;

    // 1) Navegação da Sidebar (sempre responsiva!)
    for (int i = 0; i < 5; ++i) if (G.navBtn[i].contains(x, y)) {
        teamSec = i; teamActArm = -1; teamLeaveArm = false;
        return;
    }
    if (!teamHave) { teamSay(teamConn == 3 ? "MODO LOCAL: A TRIPULA\xC7\xC3O PRECISA DO SERVIDOR" : (teamConn == 2 ? "SEM CONEX\xC3O COM O SERVIDOR" : "CONECTANDO...")); return; }
    if (G.sideFootBtn.contains(x, y)) {
        if (teamV.inTeam) teamShareInvite();
        else if (e0::chatInputOpen(app->activity, "C\xC3\xB3" "digo da tripula\xC3\xA7\xC3\xA3o (6 letras)")) teamInput = 3;
        return;
    }

    const bool locked = teamLocked();
    const bool canEdit = !teamV.inTeam || teamV.owner;

    // 2) Seção 0: VISÃO GERAL
    if (teamSec == 0) {
        if (teamV.inTeam) {
            if (G.kpi[0].contains(x, y) || G.kpi[1].contains(x, y) || G.ovMemBtn.contains(x, y)) { teamSec = 1; return; }
            if (G.kpi[2].contains(x, y)) { teamShareInvite(); return; }
            if (G.kpi[3].contains(x, y) || G.ovChatBtn.contains(x, y)) { teamSec = 2; return; }
            if (G.ovEstBtn.contains(x, y)) { teamSec = 3; return; }
            std::vector<int> top = teamFilteredMembers(0);
            for (int i = 0; i < (int)top.size() && i < 4; ++i) {
                if (teamOvMemberRect(G, i).contains(x, y)) { teamSel = top[(size_t)i]; teamSec = 1; return; }
            }
        } else {
            if (G.ovJoinBtn.contains(x, y)) {
                if (e0::chatInputOpen(app->activity, "C\xC3\xB3" "digo da tripula\xC3\xA7\xC3\xA3o (6 letras)")) teamInput = 3;
                return;
            }
            if (G.ovFoundBtn.contains(x, y)) {
                if (locked) teamOp(e0net::TeamBuyLicense);
                else teamSec = 3;
                return;
            }
        }
        return;
    }

    // 3) Seção 1: MEMBROS
    if (teamSec == 1) {
        if (!teamV.inTeam) {
            const Rect2D cb{G.main.x + G.main.w * 0.5f - 260.0f * G.u, G.main.y + 560.0f * G.u, 520.0f * G.u, 82.0f * G.u};
            if (cb.contains(x, y)) {
                if (e0::chatInputOpen(app->activity, "C\xC3\xB3" "digo da tripula\xC3\xA7\xC3\xA3o (6 letras)")) teamInput = 3;
            }
            return;
        }
        for (int f = 0; f < 2; ++f) if (G.memFilt[f].contains(x, y)) { teamMemFilter = f; teamMemScroll = 0.0f; return; }
        if (G.memList.contains(x, y)) {
            std::vector<int> list = teamFilteredMembers(teamMemFilter);
            const float relY = (y - G.memList.y) + teamMemScroll;
            const int row = (int)std::floor(relY / G.memRowH);
            if (row >= 0 && row < (int)list.size()) {
                const int idx = list[(size_t)row];
                teamSel = (teamSel == idx) ? -1 : idx; teamActArm = -1;
            }
            return;
        }
        int acts[3]; const int na = teamMemberActs(acts);
        if (na > 0) {
            for (int k = 0; k < na; ++k) {
                if (!teamInspectActRect(G, k, na).contains(x, y)) continue;
                const e0net::TeamMember& m = teamV.members[(size_t)teamSel];
                if (acts[k] == 2) {
                    float px, pz;
                    if (teamCrewPos(m.slot, px, pz)) {
                        const e0net::PlayerState me = net.predicted();
                        if (std::hypot(px - me.x, pz - me.z) < 15.0f) { teamSay(m.name + " EST\xC1 AQUI PERTO"); return; }
                        wpCrew = m.slot; wpSet = true; wpX = px; wpZ = pz; wpShip = -1;
                    }
                    teamSay("ROTA MARCADA AT\xC9 " + m.name); closeTeam(); return;
                }
                if (teamActArm != acts[k]) {
                    teamActArm = acts[k]; teamArmT = 3.2f;
                    teamSay(acts[k] == 0 ? "TOQUE DE NOVO PARA EXPULSAR " + m.name : "TOQUE DE NOVO: " + m.name + " VIRA O CAPIT\xC3O", 3.2f);
                    return;
                }
                teamActArm = -1;
                teamOp(acts[k] == 0 ? e0net::TeamKick : e0net::TeamPromote, m.name, nullptr, (uint8_t)teamSel);
                return;
            }
        } else if (G.memInviteBtn.contains(x, y)) {
            teamShareInvite(); return;
        }
        return;
    }

    // 4) Seção 2: DIÁRIO & CHAT
    if (teamSec == 2) {
        if (!teamV.inTeam) { teamSec = 0; return; }
        for (int k = 0; k < 4; ++k) if (G.chatQuick[k].contains(x, y)) {
            teamOp(e0net::TeamChat, teamQuickMsg(k));
            return;
        }
        if (G.chatBar.contains(x, y) || G.chatSendBtn.contains(x, y)) {
            if (e0::chatInputOpen(app->activity, "Mensagem para a tripula\xC3\xA7\xC3\xA3o...")) teamInput = 2;
        }
        return;
    }

    // 5) Seção 3: ESTANDARTE & GESTÃO
    if (teamSec == 3) {
        if (locked) {
            if (G.buyBtn.contains(x, y)) teamOp(e0net::TeamBuyLicense);
            else teamSay("TRANCADO: COMPRE A LICEN\xC7" "A PARA LIBERAR");
            return;
        }
        if (G.flagBtn.contains(x, y) || (std::fabs(x - G.efX) < G.efHW && std::fabs(y - G.efY) < G.efHH)) {
            if (!canEdit) { teamSay("S\xD3 O CAPIT\xC3O PODE MUDAR A BANDEIRA"); return; }
            openTeamFlag(); return;
        }
        if (G.nameBox.contains(x, y)) {
            if (!canEdit) { teamSay("S\xD3 O CAPIT\xC3O PODE MUDAR O NOME"); return; }
            if (e0::chatInputOpen(app->activity, "Nome da tripula\xC3\xA7\xC3\xA3o (3 a 20 letras)")) teamInput = 1;
            return;
        }
        if (!teamV.inTeam && G.mainBtn.contains(x, y)) {
            if ((int)teamDraftName.size() < e0net::kTeamNameMin) { teamSay("ESCREVA O NOME DA TRIPULA\xC7\xC3O PRIMEIRO"); return; }
            uint8_t b[e0net::kFlagBytes]; std::memcpy(b, &teamDraftFlag, e0net::kFlagBytes);
            teamOp(e0net::TeamCreate, teamDraftName, b); return;
        }
        if (teamV.inTeam && G.inviteBtn.contains(x, y)) { teamShareInvite(); return; }
        if (teamV.inTeam && G.leaveBtn.contains(x, y)) {
            if (!teamLeaveArm) {
                teamLeaveArm = true; teamArmT = 3.2f;
                teamSay(teamV.owner ? "VOC\xCA \xC9 O CAPIT\xC3O: DESFAZER APAGA A TRIPULA\xC7\xC3O. TOQUE DE NOVO" : "TOQUE DE NOVO PARA SAIR DA TRIPULA\xC7\xC3O", 3.5f);
            } else { teamLeaveArm = false; teamOp(e0net::TeamLeave); }
            return;
        }
    }

    // 5) Seção 4: COFRE & LOJA
    if (teamSec == 4) {
        if (!teamV.inTeam) { teamSay("ENTRE NUMA TRIPULA\xC7\xC3O PARA USAR O COFRE"); return; }
        for (int k = 0; k < 4; ++k) {
            if (G.depBtn[k].contains(x, y)) {
                const int amt = k == 3 ? (int)teamV.coins : kTeamAmt[k];
                if (amt <= 0 || teamV.coins < amt) { teamSay("VOC\xCA N\xC3O TEM MOEDAS SUFICIENTES"); return; }
                teamOp(e0net::TeamDeposit, "", nullptr, 0, (uint32_t)std::min(amt, 1000000)); return;
            }
            if (G.wdBtn[k].contains(x, y)) {
                if (!teamV.owner) { teamSay("S\xD3 O CAPIT\xC3O PODE SACAR DO COFRE"); return; }
                const int amt = k == 3 ? (int)teamV.treasury : kTeamAmt[k];
                if (amt <= 0 || teamV.treasury < amt) { teamSay("O COFRE N\xC3O TEM MOEDAS SUFICIENTES"); return; }
                teamOp(e0net::TeamWithdraw, "", nullptr, 0, (uint32_t)std::min(amt, 1000000)); return;
            }
        }
        for (int k = 0; k < 5; ++k) if (G.shopRow[k].contains(x, y)) {
            int price; const char* st = teamShopState(k, price);
            if (std::strcmp(st, "COMPRAR") != 0) { teamSay(k == 0 ? "J\xC1 NO M\xC1XIMO" : "A TRIPULA\xC7\xC3O J\xC1 TEM ESTE NAVIO"); return; }
            if (!teamV.owner) { teamSay("S\xD3 O CAPIT\xC3O PODE COMPRAR NA LOJA DA TRIPULA\xC7\xC3O"); return; }
            if (teamV.treasury < price) { char t[80]; std::snprintf(t, sizeof t, "O COFRE PRECISA DE %d MOEDAS (TEM %d)", price, (int)teamV.treasury); teamSay(t, 3.5f); return; }
            if (teamActArm != 100 + k) { teamActArm = 100 + k; teamArmT = 3.2f; teamSay("TOQUE DE NOVO PARA CONFIRMAR A COMPRA", 3.2f); return; }
            teamActArm = -1; teamOp(e0net::TeamBuy, "", nullptr, (uint8_t)k); return;
        }
    }
}
// itens da loja da tripulação: 0 = +vagas, 1..4 = navios. Devolve o estado ("COMPRAR", "COMPRADO", "MAXIMO") e o preço.
static constexpr int kTeamAmt[3] = {100, 500, 1000};
const char* teamShopState(int k, int& price) const {
    if (k == 0) { price = teamV.slotLevel < e0net::kTeamSlotLevels ? e0net::kTeamSlotPrice[teamV.slotLevel] : 0; return teamV.slotLevel >= e0net::kTeamSlotLevels ? "MAXIMO" : "COMPRAR"; }
    price = e0net::kTeamShipPrice[(k - 1) & 3];
    return ((teamV.ships >> (k - 1)) & 1u) ? "COMPRADO" : "COMPRAR";
}

// ---------- desenho ----------
void teamField(const Rect2D& r, float u, bool on) {
    renderer.uiWoodPanel(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 12.0f * u, on ? theme::PanelSlotHot : theme::PanelSlot, 0.96f, 4.0f);
}
void teamDrawPoleFlag(float fcx, float fcy, float fhw, float fhh, float u, float dim, const FlagDesign& fd) {
    (void)u;
    renderer.uiGlow(fcx, fcy, fhw * 1.45f, fhh * 1.65f, 0.92f, 0.62f, 0.26f, 0.08f * dim);
    renderer.uiFlag(flagLook(fd), fcx, fcy, fhw, fhh, teamT, 1.0f, dim);
}
void drawTeamShapes(const TeamGeo& G) {
    const float u = G.u;
    const bool locked = teamLocked(), canEdit = !teamV.inTeam || teamV.owner;
    FlagDesign curFlag = teamDraftFlag;
    if (teamV.inTeam) { FlagDesign f2; if (flagFromBytes(teamV.flag, f2)) curFlag = f2; }

    // Fundo + cabeçalho unificado
    renderer.uiWoodPanel((float)w * 0.5f, (float)h * 0.5f, (float)w * 0.5f, (float)h * 0.5f, 0.0f, theme::PanelBgWood, 1.0f, 3.0f);
    renderer.uiGlow((float)w * 0.32f, (float)h * 0.42f, (float)w * 0.55f, (float)h * 0.60f, 0.92f, 0.62f, 0.24f, 0.06f);
    renderer.uiIconTint(HiSabres, 72.0f * u, 60.0f * u, 34.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    renderer.uiDivider(420.0f * u, 118.0f * u, 290.0f * u, 6.0f * u, 0.72f);
    closeBtnShapes(G.closeX, G.closeY, G.closeR);
    {
        const Rect2D& p = G.coinPill;
        renderer.uiWoodPanel(p.x + p.w * 0.5f, p.y + p.h * 0.5f, p.w * 0.5f, p.h * 0.5f, p.h * 0.5f, theme::PanelCard, 0.96f, 2.0f);
        renderer.uiIconTint(HiCoins, p.x + 40.0f * u, p.y + p.h * 0.5f, 21.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    }

    // ---- SIDEBAR ESQUERDA ----
    renderer.uiWoodPanel(G.side.x + G.side.w * 0.5f, G.side.y + G.side.h * 0.5f, G.side.w * 0.5f, G.side.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 1.0f);
    teamDrawPoleFlag(G.sfX, G.sfY, G.sfHW, G.sfHH, u, teamV.inTeam ? 1.0f : (locked ? 0.45f : 0.9f), curFlag);
    renderer.uiDivider(G.side.x + G.side.w * 0.5f, G.side.y + 236.0f * u, G.side.w * 0.40f, 5.0f * u, 0.65f);
    static const int kNavIco[5] = {HiWheel, HiCaptain, HiChat, HiPirateFlag, HiLockedChest};
    for (int i = 0; i < 5; ++i) {
        const Rect2D& b = G.navBtn[i];
        const bool on = (teamSec == i);
        renderer.uiWoodPanel(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12.0f * u, on ? theme::PanelCardSel : theme::PanelSlot, on ? 0.98f : 0.86f, (float)(i + 5));
        if (on) renderer.uiRoundRect(b.x + 5.0f * u, b.y + b.h * 0.5f, 3.5f * u, b.h * 0.34f, 2.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.98f);
        renderer.uiIconTint(kNavIco[i], b.x + 42.0f * u, b.y + b.h * 0.5f, 22.0f * u, on ? theme::kBrassBright.r : theme::kCreamSoft.r, on ? theme::kBrassBright.g : theme::kCreamSoft.g, on ? theme::kBrassBright.b : theme::kCreamSoft.b, on ? 1.0f : 0.80f);
    }
    flagGoldBtn(G.sideFootBtn, u, true);

    // ---- SEÇÃO 0: VISÃO GERAL ----
    if (teamSec == 0) {
        if (teamV.inTeam) {
            static const int kKpiIco[4] = {HiCaptain, HiFlAnchor, HiIdCard, HiChat};
            for (int i = 0; i < 4; ++i) {
                const Rect2D& k = G.kpi[i];
                renderer.uiWoodPanel(k.x + k.w * 0.5f, k.y + k.h * 0.5f, k.w * 0.5f, k.h * 0.5f, 16.0f * u, theme::PanelWood, 0.98f, (float)(i + 10));
                renderer.uiWoodPanel(k.x + 50.0f * u, k.y + k.h * 0.5f, 32.0f * u, 32.0f * u, 10.0f * u, theme::PanelSlotHot, 0.95f, (float)i);
                renderer.uiIconTint(kKpiIco[i], k.x + 50.0f * u, k.y + k.h * 0.5f, 22.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.98f);
            }
            // barrinha de lotação no KPI 0
            {
                const Rect2D& k = G.kpi[0];
                const float bx = k.x + 100.0f * u, bw = k.w - 124.0f * u, by = k.y + k.h - 24.0f * u, bh = 8.0f * u;
                const float fill = std::min(1.0f, (float)teamV.members.size() / (float)teamV.capacity());
                renderer.uiRoundRect(bx + bw * 0.5f, by, bw * 0.5f, bh * 0.5f, 4.0f * u, 0.08f, 0.06f, 0.04f, 0.90f);
                if (fill > 0.01f) renderer.uiRoundRect(bx + bw * fill * 0.5f, by, bw * fill * 0.5f, bh * 0.5f, 4.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
            }
            // Painel esquerdo (Diário recente) e direito (Quadro de bordo)
            renderer.uiWoodPanel(G.ovLeft.x + G.ovLeft.w * 0.5f, G.ovLeft.y + G.ovLeft.h * 0.5f, G.ovLeft.w * 0.5f, G.ovLeft.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 14.0f);
            renderer.uiWoodPanel(G.ovRight.x + G.ovRight.w * 0.5f, G.ovRight.y + G.ovRight.h * 0.5f, G.ovRight.w * 0.5f, G.ovRight.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 15.0f);
            // cartões das últimas 4 mensagens
            const int nShow = std::min(4, (int)teamMsgs.size());
            const float cardY0 = G.ovLeft.y + 102.0f * u, cardH = 112.0f * u, cardGap = 14.0f * u;
            for (int i = 0; i < nShow; ++i) {
                const TeamMsgL& m = teamMsgs[teamMsgs.size() - (size_t)nShow + (size_t)i];
                const float cy = cardY0 + (float)i * (cardH + cardGap);
                renderer.uiWoodPanel(G.ovLeft.x + G.ovLeft.w * 0.5f, cy + cardH * 0.5f, (G.ovLeft.w - 52.0f * u) * 0.5f, cardH * 0.5f, 10.0f * u, m.m.mine ? theme::PanelSlotHot : theme::PanelSlot, 0.94f, (float)i);
            }
            // cartões dos 4 tripulantes em destaque
            std::vector<int> top = teamFilteredMembers(0);
            for (int i = 0; i < (int)top.size() && i < 4; ++i) {
                const Rect2D r = teamOvMemberRect(G, i);
                const e0net::TeamMember& m = teamV.members[(size_t)top[(size_t)i]];
                renderer.uiWoodPanel(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 10.0f * u, m.me ? theme::PanelSlotHot : theme::PanelSlot, 0.95f, (float)i);
                renderer.uiShape(Renderer::UiFill, r.x + 26.0f * u, r.y + r.h * 0.5f, 8.0f * u, m.online ? 0.34f : 0.42f, m.online ? 0.80f : 0.38f, m.online ? 0.42f : 0.34f, 1.0f);
                if (m.owner) renderer.uiIconTint(HiCrown, r.x + r.w - 34.0f * u, r.y + r.h * 0.5f, 20.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
            }
            renderer.uiButton(G.ovChatBtn.x + G.ovChatBtn.w * 0.5f, G.ovChatBtn.y + G.ovChatBtn.h * 0.5f, G.ovChatBtn.w * 0.5f, G.ovChatBtn.h * 0.5f, 12.0f * u, theme::BtnWood, 1.0f);
            renderer.uiButton(G.ovMemBtn.x + G.ovMemBtn.w * 0.5f, G.ovMemBtn.y + G.ovMemBtn.h * 0.5f, G.ovMemBtn.w * 0.5f, G.ovMemBtn.h * 0.5f, 12.0f * u, theme::BtnWood, 1.0f);
            renderer.uiButton(G.ovEstBtn.x + G.ovEstBtn.w * 0.5f, G.ovEstBtn.y + G.ovEstBtn.h * 0.5f, G.ovEstBtn.w * 0.5f, G.ovEstBtn.h * 0.5f, 12.0f * u, theme::BtnWood, 1.0f);
        } else {
            renderer.uiWoodPanel(G.ovJoin.x + G.ovJoin.w * 0.5f, G.ovJoin.y + G.ovJoin.h * 0.5f, G.ovJoin.w * 0.5f, G.ovJoin.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 20.0f);
            renderer.uiWoodPanel(G.ovFound.x + G.ovFound.w * 0.5f, G.ovFound.y + G.ovFound.h * 0.5f, G.ovFound.w * 0.5f, G.ovFound.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 21.0f);
            renderer.uiIconTint(HiIdCard, G.ovJoin.x + G.ovJoin.w * 0.5f, G.ovJoin.y + 185.0f * u, 74.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
            flagGoldBtn(G.ovJoinBtn, u, teamWaitOp < 0);
            teamDrawPoleFlag(G.ovFound.x + G.ovFound.w * 0.5f, G.ovFound.y + 196.0f * u, 134.0f * u, 89.0f * u, u, locked ? 0.45f : 1.0f, curFlag);
            if (locked) renderer.uiIconTint(HiLockedChest, G.ovFound.x + G.ovFound.w * 0.5f, G.ovFound.y + 196.0f * u, 40.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.96f);
            flagGoldBtn(G.ovFoundBtn, u, teamWaitOp < 0);
            static const int kPkIco[3] = {HiPirateFlag, HiCompass, HiChat};
            for (int i = 0; i < 3; ++i) {
                const Rect2D& p = G.ovPerk[i];
                renderer.uiWoodPanel(p.x + p.w * 0.5f, p.y + p.h * 0.5f, p.w * 0.5f, p.h * 0.5f, 16.0f * u, theme::PanelCard, 0.96f, (float)(i + 25));
                renderer.uiIconTint(kPkIco[i], p.x + 54.0f * u, p.y + 58.0f * u, 28.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.96f);
            }
        }
    }
    // ---- SEÇÃO 1: MEMBROS ----
    else if (teamSec == 1) {
        if (teamV.inTeam) {
            renderer.uiWoodPanel(G.memPane.x + G.memPane.w * 0.5f, G.memPane.y + G.memPane.h * 0.5f, G.memPane.w * 0.5f, G.memPane.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 30.0f);
            renderer.uiWoodPanel(G.memInspect.x + G.memInspect.w * 0.5f, G.memInspect.y + G.memInspect.h * 0.5f, G.memInspect.w * 0.5f, G.memInspect.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 31.0f);
            for (int f = 0; f < 2; ++f) {
                const Rect2D& r = G.memFilt[f];
                renderer.uiButton(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 10.0f * u, teamMemFilter == f ? theme::BtnBrass : theme::BtnWood, 1.0f);
            }
            // Inspetor lateral
            if (teamSel >= 0 && teamSel < (int)teamV.members.size()) {
                const e0net::TeamMember& m = teamV.members[(size_t)teamSel];
                const float icx = G.memInspect.x + G.memInspect.w * 0.5f;
                renderer.uiWoodPanel(icx, G.memInspect.y + 136.0f * u, 54.0f * u, 54.0f * u, 16.0f * u, theme::PanelSlotHot, 0.96f, 32.0f);
                renderer.uiIconTint(m.owner ? HiCrown : HiCaptain, icx, G.memInspect.y + 136.0f * u, 36.0f * u, m.owner ? theme::kBrassBright.r : theme::kCream.r, m.owner ? theme::kBrassBright.g : theme::kCream.g, m.owner ? theme::kBrassBright.b : theme::kCream.b, 1.0f);
                // cartão de status/posição
                renderer.uiWoodPanel(icx, G.memInspect.y + 388.0f * u, (G.memInspect.w - 60.0f * u) * 0.5f, 92.0f * u, 12.0f * u, theme::PanelSlot, 0.95f, 33.0f);
                int acts[3]; const int na = teamMemberActs(acts);
                for (int k = 0; k < na; ++k) {
                    const Rect2D b = teamInspectActRect(G, k, na);
                    const bool arm = teamActArm == acts[k];
                    const int bst = acts[k] == 2 ? theme::BtnBrass : (acts[k] == 0 ? (arm ? theme::BtnCrimsonHi : theme::BtnCrimson) : (arm ? theme::BtnBrass : theme::BtnWood));
                    renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12.0f * u, bst, 1.0f);
                }
            } else {
                const float icx = G.memInspect.x + G.memInspect.w * 0.5f;
                renderer.uiIconTint(HiCrown, icx, G.memInspect.y + 128.0f * u, 42.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
                renderer.uiWoodPanel(icx, G.memInspect.y + 370.0f * u, (G.memInspect.w - 60.0f * u) * 0.5f, 124.0f * u, 12.0f * u, theme::PanelSlot, 0.94f, 34.0f);
                flagGoldBtn(G.memInviteBtn, u, true);
            }
        } else {
            renderer.uiWoodPanel(G.main.x + G.main.w * 0.5f, G.main.y + G.main.h * 0.5f, G.main.w * 0.5f, G.main.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 35.0f);
            renderer.uiIconTint(HiCaptain, G.main.x + G.main.w * 0.5f, G.main.y + 300.0f * u, 76.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.9f);
            const Rect2D cb{G.main.x + G.main.w * 0.5f - 260.0f * u, G.main.y + 560.0f * u, 520.0f * u, 82.0f * u};
            flagGoldBtn(cb, u, teamWaitOp < 0);
        }
    }
    // ---- SEÇÃO 2: DIÁRIO & CHAT ----
    else if (teamSec == 2) {
        renderer.uiWoodPanel(G.main.x + G.main.w * 0.5f, G.main.y + G.main.h * 0.5f, G.main.w * 0.5f, G.main.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 40.0f);
        if (teamV.inTeam) {
            renderer.uiWoodPanel(G.chatList.x + G.chatList.w * 0.5f, G.chatList.y + G.chatList.h * 0.5f, G.chatList.w * 0.5f, G.chatList.h * 0.5f, 12.0f * u, theme::PanelSlot, 0.72f, 41.0f);
            for (int k = 0; k < 4; ++k) {
                const Rect2D& q = G.chatQuick[k];
                renderer.uiButton(q.x + q.w * 0.5f, q.y + q.h * 0.5f, q.w * 0.5f, q.h * 0.5f, 10.0f * u, theme::BtnWood, 0.95f);
            }
            teamField(G.chatBar, u, true);
            flagGoldBtn(G.chatSendBtn, u, true);
        } else {
            renderer.uiIconTint(HiLockedChest, G.main.x + G.main.w * 0.5f, G.main.y + 330.0f * u, 76.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
        }
    }
    // ---- SEÇÃO 3: ESTANDARTE & GESTÃO ----
    else if (teamSec == 3) {
        renderer.uiWoodPanel(G.estLeft.x + G.estLeft.w * 0.5f, G.estLeft.y + G.estLeft.h * 0.5f, G.estLeft.w * 0.5f, G.estLeft.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 50.0f);
        renderer.uiWoodPanel(G.estRight.x + G.estRight.w * 0.5f, G.estRight.y + G.estRight.h * 0.5f, G.estRight.w * 0.5f, G.estRight.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 51.0f);
        const float dim = locked ? 0.38f : 1.0f;
        teamDrawPoleFlag(G.efX, G.efY, G.efHW, G.efHH, u, dim, curFlag);
        renderer.uiButton(G.flagBtn.x + G.flagBtn.w * 0.5f, G.flagBtn.y + G.flagBtn.h * 0.5f, G.flagBtn.w * 0.5f, G.flagBtn.h * 0.5f, 12.0f * u, (!locked && canEdit) ? theme::BtnWood : theme::BtnDisabled, dim);
        teamField(G.nameBox, u, !locked && canEdit);
        if (locked) {
            renderer.uiIconTint(HiLockedChest, G.efX, G.efY, 48.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
            renderer.uiWoodPanel(G.estRight.x + G.estRight.w * 0.5f, G.nameBox.y + G.nameBox.h + 150.0f * u, (G.estRight.w - 72.0f * u) * 0.5f, 108.0f * u, 14.0f * u, theme::PanelCardSel, 0.96f, 52.0f);
            flagGoldBtn(G.buyBtn, u, teamWaitOp < 0);
        } else if (!teamV.inTeam) {
            flagGoldBtn(G.mainBtn, u, (int)teamDraftName.size() >= e0net::kTeamNameMin);
        } else {
            // Cartão código de recrutamento + cartão regras + botão sair/desfazer
            const float rx = G.estRight.x + 36.0f * u, rw = G.estRight.w - 72.0f * u;
            renderer.uiWoodPanel(rx + rw * 0.5f, G.inviteBtn.y + G.inviteBtn.h * 0.5f, rw * 0.5f, 56.0f * u, 12.0f * u, theme::PanelSlotHot, 0.95f, 53.0f);
            flagGoldBtn(G.inviteBtn, u, true);
            renderer.uiWoodPanel(rx + rw * 0.5f, G.inviteBtn.y + G.inviteBtn.h + 156.0f * u, rw * 0.5f, 114.0f * u, 12.0f * u, theme::PanelSlot, 0.92f, 54.0f);
            renderer.uiButton(G.leaveBtn.x + G.leaveBtn.w * 0.5f, G.leaveBtn.y + G.leaveBtn.h * 0.5f, G.leaveBtn.w * 0.5f, G.leaveBtn.h * 0.5f, 12.0f * u, teamLeaveArm ? theme::BtnCrimsonHi : theme::BtnCrimson, 1.0f);
        }
    }

    // ---- SEÇÃO 4: COFRE & LOJA ----
    else if (teamSec == 4) {
        renderer.uiWoodPanel(G.vaultL.x + G.vaultL.w * 0.5f, G.vaultL.y + G.vaultL.h * 0.5f, G.vaultL.w * 0.5f, G.vaultL.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 60.0f);
        renderer.uiWoodPanel(G.vaultR.x + G.vaultR.w * 0.5f, G.vaultR.y + G.vaultR.h * 0.5f, G.vaultR.w * 0.5f, G.vaultR.h * 0.5f, 18.0f * u, theme::PanelWood, 0.98f, 61.0f);
        if (teamV.inTeam) {
            renderer.uiGlow(G.vaultL.x + G.vaultL.w * 0.5f, G.vaultL.y + 250.0f * u, 200.0f * u, 120.0f * u, 1.0f, 0.78f, 0.30f, 0.16f);
            renderer.uiIconTint(HiLockedChest, G.vaultL.x + G.vaultL.w * 0.5f, G.vaultL.y + 150.0f * u, 54.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
            for (int k = 0; k < 4; ++k) {
                renderer.uiButton(G.depBtn[k].x + G.depBtn[k].w * 0.5f, G.depBtn[k].y + G.depBtn[k].h * 0.5f, G.depBtn[k].w * 0.5f, G.depBtn[k].h * 0.5f, 12.0f * u, theme::BtnWood, 1.0f);
                renderer.uiButton(G.wdBtn[k].x + G.wdBtn[k].w * 0.5f, G.wdBtn[k].y + G.wdBtn[k].h * 0.5f, G.wdBtn[k].w * 0.5f, G.wdBtn[k].h * 0.5f, 12.0f * u, teamV.owner ? theme::BtnWood : theme::BtnDisabled, teamV.owner ? 1.0f : 0.8f);
            }
            static const int kShopIco[5] = {HiCaptain, HiGalleon, HiGalleon, HiGalleon, HiPirateFlag};
            for (int k = 0; k < 5; ++k) {
                const Rect2D& r = G.shopRow[k]; int price; const char* st = teamShopState(k, price);
                const bool buyable = std::strcmp(st, "COMPRAR") == 0;
                renderer.uiWoodPanel(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 12.0f * u, buyable ? theme::PanelSlotHot : theme::PanelSlot, 0.94f, (float)(62 + k));
                renderer.uiIconTint(kShopIco[k], r.x + 54.0f * u, r.y + r.h * 0.5f, 26.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, buyable ? 1.0f : 0.6f);
                if (buyable) flagGoldBtn(G.shopBuyBtn[k], u, teamV.treasury >= price);
                else renderer.uiButton(G.shopBuyBtn[k].x + G.shopBuyBtn[k].w * 0.5f, G.shopBuyBtn[k].y + G.shopBuyBtn[k].h * 0.5f, G.shopBuyBtn[k].w * 0.5f, G.shopBuyBtn[k].h * 0.5f, 12.0f * u, theme::BtnDisabled, 0.9f);
            }
        }
    }

    // Toast de aviso
    if (teamMsgT > 0.0f) {
        const float a = std::min(1.0f, teamMsgT * 3.0f);
        const Rect2D r = teamToastRect(G);
        renderer.uiWoodPanel(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, r.h * 0.5f, theme::PanelCardSel, 0.98f * a, 6.0f);
    }
}

void drawTeamText(const TeamGeo& G) {
    const float u = G.u;
    const bool locked = teamLocked(), canEdit = !teamV.inTeam || teamV.owner;
    const int t6 = teamTs(5.6f), t5 = teamTs(4.6f), t4 = teamTs(3.6f), t3 = teamTs(2.8f), t2 = teamTs(2.3f);
    auto center = [&](const Rect2D& r, int ts, const char* s, float cr, float cg, float cb, float a = 1.0f, bool bold = false) {
        int t = ts; while (t > 2 && Renderer::textWidth(s, t) > r.w - 24.0f * u) --t;
        const float x = r.x + (r.w - Renderer::textWidth(s, t)) * 0.5f, y = r.y + (r.h - 7.0f * (float)t) * 0.5f;
        if (bold) shopBold(x, y, t, s, cr, cg, cb, a, false); else renderer.text(x, y, t, s, cr, cg, cb, a, false);
    };
    auto label = [&](float x, float y, const char* s) { renderer.text(x, y, t3, s, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 1.0f, false); };

    // Cabeçalho
    const char* hdTitle = teamV.inTeam ? teamV.name.c_str() : "TRIPULA\xC7\xC3O";
    shopBold(126.0f * u, 24.0f * u, t6, hdTitle, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    char subBuf[96];
    if (teamV.inTeam) std::snprintf(subBuf, sizeof subBuf, "%s  -  C\xD3" "DIGO %s  -  %d/%d MEMBROS", teamV.owner ? "CAPIT\xC3O DA TRIPULA\xC7\xC3O" : "TRIPULANTE", teamV.code.c_str(), (int)teamV.members.size(), teamV.capacity());
    else std::snprintf(subBuf, sizeof subBuf, "QUARTEL-GENERAL: CRIE SUA TRIPULA\xC7\xC3O OU ENTRE COM UM C\xD3" "DIGO");
    renderer.text(126.0f * u, 24.0f * u + 7.0f * (float)t6 + 12.0f * u, t3, subBuf, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);
    {
        char ct[32]; coinText(ct, sizeof ct, teamV.coins);
        const char* s = teamHave ? ct : "--";
        const Rect2D& p = G.coinPill;
        shopBold(p.x + 70.0f * u, p.y + (p.h - 7.0f * (float)t5) * 0.5f, t5, s, 1.0f, 0.90f, 0.55f, 1.0f, false);
    }

    // ---- SIDEBAR TEXTOS ----
    {
        const Rect2D nmR{G.side.x + 18.0f * u, G.side.y + 158.0f * u, G.side.w - 36.0f * u, 30.0f * u};
        center(nmR, t4, teamV.inTeam ? teamV.name.c_str() : "SEM TRIPULA\xC7\xC3O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
        char stBuf[48];
        if (teamV.inTeam) std::snprintf(stBuf, sizeof stBuf, "%d/%d MEMBROS  -  %d ONLINE", (int)teamV.members.size(), teamV.capacity(), teamOnlineCount());
        else std::snprintf(stBuf, sizeof stBuf, "%s", locked ? "LICEN\xC7" "A PENDENTE" : "LICEN\xC7" "A ATIVA");
        const Rect2D stR{G.side.x + 18.0f * u, G.side.y + 194.0f * u, G.side.w - 36.0f * u, 24.0f * u};
        center(stR, t2, stBuf, teamV.inTeam ? 0.58f : theme::kCreamMuted.r, teamV.inTeam ? 0.86f : theme::kCreamMuted.g, teamV.inTeam ? 0.58f : theme::kCreamMuted.b);

        static const char* kNavTitle[5] = {"VIS\xC3O GERAL", "MEMBROS", "DI\xC1RIO & CHAT", "ESTANDARTE", "COFRE & LOJA"};
        char badge[5][20];
        badge[0][0] = '\0';
        if (teamV.inTeam) {
            std::snprintf(badge[1], sizeof badge[1], "%d/%d", (int)teamV.members.size(), teamV.capacity());
            std::snprintf(badge[2], sizeof badge[2], "%d", (int)teamMsgs.size());
            std::snprintf(badge[3], sizeof badge[3], "%s", teamV.owner ? "CAPIT\xC3O" : "VER");
            coinText(badge[4], sizeof badge[4], teamV.treasury);
        } else {
            std::snprintf(badge[1], sizeof badge[1], "--");
            std::snprintf(badge[2], sizeof badge[2], "--");
            std::snprintf(badge[3], sizeof badge[3], "%s", locked ? "TRANCADO" : "CRIAR");
            std::snprintf(badge[4], sizeof badge[4], "--");
        }
        for (int i = 0; i < 5; ++i) {
            const Rect2D& b = G.navBtn[i];
            const bool on = (teamSec == i);
            const float bw = badge[i][0] ? Renderer::textWidth(badge[i], t2) + 28.0f * u : 16.0f * u;
            const float avail = b.w - 76.0f * u - bw;
            int nt = t3; while (nt > 2 && Renderer::textWidth(kNavTitle[i], nt) > avail) --nt;
            renderer.text(b.x + 74.0f * u, b.y + (b.h - 7.0f * (float)nt) * 0.5f, nt, kNavTitle[i],
                          on ? theme::kBrassBright.r : theme::kCream.r, on ? theme::kBrassBright.g : theme::kCream.g, on ? theme::kBrassBright.b : theme::kCream.b, on ? 1.0f : 0.85f, false);
            if (badge[i][0]) {
                const float tw = Renderer::textWidth(badge[i], t2);
                renderer.text(b.x + b.w - 18.0f * u - tw, b.y + (b.h - 7.0f * (float)t2) * 0.5f, t2, badge[i],
                              on ? theme::kBrassBright.r : theme::kCreamMuted.r, on ? theme::kBrassBright.g : theme::kCreamMuted.g, on ? theme::kBrassBright.b : theme::kCreamMuted.b, 0.95f, false);
            }
        }
        if (teamV.inTeam) {
            char cf[32]; std::snprintf(cf, sizeof cf, "C\xD3" "DIGO: %s", teamV.code.c_str());
            const Rect2D cr{G.side.x + 20.0f * u, G.sideFootBtn.y - 34.0f * u, G.side.w - 40.0f * u, 26.0f * u};
            center(cr, t3, cf, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.96f, true);
            center(G.sideFootBtn, t4, "CONVIDAR AMIGOS", 0.16f, 0.10f, 0.02f, 1.0f, true);
        } else {
            center(G.sideFootBtn, t4, "ENTRAR COM C\xD3" "DIGO", 0.16f, 0.10f, 0.02f, 1.0f, true);
        }
    }

    // ---- SEÇÃO 0: VISÃO GERAL ----
    if (teamSec == 0) {
        if (teamV.inTeam) {
            // 4 KPIs
            char v0[24], v1[24], v3[24], s1[48], s3[48];
            std::snprintf(v0, sizeof v0, "%d / %d", (int)teamV.members.size(), teamV.capacity());
            std::snprintf(v1, sizeof v1, "%d ONLINE", teamOnlineCount());
            std::snprintf(s1, sizeof s1, "CAPIT\xC3O: %s", teamCaptainName().c_str());
            std::snprintf(v3, sizeof v3, "%d REGISTROS", (int)teamMsgs.size());
            if (netGame && net.connected()) {
                const e0net::PlayerState me = net.visual();
                char qc[12]; mapGridCoord(me.x, me.z, qc, sizeof qc);
                std::snprintf(s3, sizeof s3, "SEU QUADRANTE: %s", qc);
            } else std::snprintf(s3, sizeof s3, "HIST\xD3RICO DE 24 HORAS");
            const char* kt[4] = {"TRIPULANTES", "NO MAR AGORA", "C\xD3" "DIGO DE CONVITE", "DI\xC1RIO DE BORDO"};
            const char* kv[4] = {v0, v1, teamV.code.c_str(), v3};
            const char* ks[4] = {"", s1, "TOQUE PARA CONVIDAR", s3};
            for (int i = 0; i < 4; ++i) {
                const Rect2D& k = G.kpi[i];
                const float lx = k.x + 98.0f * u, maxW = k.w - 116.0f * u;
                renderer.text(lx, k.y + 22.0f * u, t2, kt[i], theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
                int tv = t5; while (tv > 2 && Renderer::textWidth(kv[i], tv) > maxW) --tv;
                const bool green = (i == 1 && teamOnlineCount() > 0);
                shopBold(lx, k.y + 52.0f * u, tv, kv[i], green ? 0.45f : theme::kBrassBright.r, green ? 0.88f : theme::kBrassBright.g, green ? 0.50f : theme::kBrassBright.b, 1.0f, false);
                if (ks[i][0]) {
                    int ts = t2; while (ts > 2 && Renderer::textWidth(ks[i], ts) > maxW) --ts;
                    renderer.text(lx, k.y + k.h - 32.0f * u, ts, ks[i], theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.82f, false);
                }
            }
            // Painel esquerdo: Diário recente
            shopBold(G.ovLeft.x + 28.0f * u, G.ovLeft.y + 22.0f * u, t5, "DI\xC1RIO RECENTE", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
            renderer.text(G.ovLeft.x + 28.0f * u, G.ovLeft.y + 22.0f * u + 7.0f * (float)t5 + 10.0f * u, t2, "\xDALTIMAS CONVERSAS E EVENTOS DA TRIPULA\xC7\xC3O", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
            if (teamMsgs.empty()) {
                Rect2D er{G.ovLeft.x + 26.0f * u, G.ovLeft.y + 260.0f * u, G.ovLeft.w - 52.0f * u, 40.0f * u};
                center(er, t4, "NENHUMA MENSAGEM NAS \xDALTIMAS 24H", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
            } else {
                const double now = teamNow();
                const int nShow = std::min(4, (int)teamMsgs.size());
                const float cardY0 = G.ovLeft.y + 102.0f * u, cardH = 112.0f * u, cardGap = 14.0f * u;
                for (int i = 0; i < nShow; ++i) {
                    const TeamMsgL& m = teamMsgs[teamMsgs.size() - (size_t)nShow + (size_t)i];
                    const float cy = cardY0 + (float)i * (cardH + cardGap);
                    const float tx = G.ovLeft.x + 44.0f * u, twMax = G.ovLeft.w - 88.0f * u;
                    char ag[16]; teamAge(ag, sizeof ag, (double)m.m.age + (now - m.at));
                    const std::string who = m.m.author.empty() ? std::string("EVENTO") : (m.m.mine ? std::string("VOC\xCA") : m.m.author);
                    renderer.text(tx, cy + 14.0f * u, t2, who.c_str(), theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.96f, false);
                    renderer.text(G.ovLeft.x + G.ovLeft.w - 44.0f * u - Renderer::textWidth(ag, t2), cy + 14.0f * u, t2, ag, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.85f, false);
                    char lns[2][72]; int nl = wrapLinesPx(m.m.text.c_str(), t3, twMax, lns, 2);
                    for (int k = 0; k < nl; ++k)
                        renderer.text(tx, cy + 44.0f * u + (float)k * (7.0f * (float)t3 + 6.0f * u), t3, lns[k], theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f, false);
                }
            }
            center(G.ovChatBtn, t4, "ABRIR DI\xC1RIO & CHAT COMPLETO", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b);

            // Painel direito: Quadro de bordo (4 tripulantes em destaque)
            shopBold(G.ovRight.x + 28.0f * u, G.ovRight.y + 22.0f * u, t5, "TRIPULANTES ATIVOS", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
            renderer.text(G.ovRight.x + 28.0f * u, G.ovRight.y + 22.0f * u + 7.0f * (float)t5 + 10.0f * u, t2, "TOQUE NUM TRIPULANTE PARA INSPECIONAR", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
            std::vector<int> top = teamFilteredMembers(0);
            for (int i = 0; i < (int)top.size() && i < 4; ++i) {
                const Rect2D r = teamOvMemberRect(G, i);
                const e0net::TeamMember& m = teamV.members[(size_t)top[(size_t)i]];
                std::string nm = m.name + (m.me ? "  (VOC\xCA)" : "");
                char st[24] = "";
                float px, pz;
                if (!m.me && netGame && m.online && teamCrewPos(m.slot, px, pz)) {
                    const e0net::PlayerState me = net.predicted();
                    const float d = std::hypot(px - me.x, pz - me.z);
                    if (d >= 1000.0f) std::snprintf(st, sizeof st, "%.1f KM", d / 1000.0f);
                    else std::snprintf(st, sizeof st, "%d M", (int)d);
                } else std::snprintf(st, sizeof st, "%s", m.online ? "ONLINE" : "OFFLINE");
                int tn = t4; while (tn > 2 && Renderer::textWidth(nm.c_str(), tn) > r.w - 190.0f * u) --tn;
                renderer.text(r.x + 48.0f * u, r.y + (r.h - 7.0f * (float)tn) * 0.5f, tn, nm.c_str(), m.owner ? theme::kBrassBright.r : theme::kCream.r, m.owner ? theme::kBrassBright.g : theme::kCream.g, m.owner ? theme::kBrassBright.b : theme::kCream.b, 1.0f, false);
                const float sx = r.x + r.w - (m.owner ? 68.0f : 24.0f) * u - Renderer::textWidth(st, t2);
                renderer.text(sx, r.y + (r.h - 7.0f * (float)t2) * 0.5f, t2, st, m.online ? 0.52f : theme::kCreamMuted.r, m.online ? 0.88f : theme::kCreamMuted.g, m.online ? 0.52f : theme::kCreamMuted.b, 0.95f, false);
            }
            if ((int)top.size() > 4) {
                char more[48]; std::snprintf(more, sizeof more, "+ %d OUTROS TRIPULANTES NA LISTA COMPLETA", (int)top.size() - 4);
                Rect2D mr{G.ovRight.x + 26.0f * u, G.ovRight.y + 102.0f * u + 4.0f * 78.0f * u + 12.0f * u, G.ovRight.w - 52.0f * u, 28.0f * u};
                center(mr, t2, more, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
            }
            center(G.ovMemBtn, t3, "TODOS OS MEMBROS", theme::kCream.r, theme::kCream.g, theme::kCream.b);
            center(G.ovEstBtn, t3, "ESTANDARTE & GEST\xC3O", theme::kCream.r, theme::kCream.g, theme::kCream.b);
        } else {
            // Sem tripulação: 2 cartões principais + 3 benefícios
            center({G.ovJoin.x, G.ovJoin.y + 34.0f * u, G.ovJoin.w, 42.0f * u}, t5, "ENTRAR COM C\xD3" "DIGO", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
            center({G.ovJoin.x + 30.0f * u, G.ovJoin.y + 296.0f * u, G.ovJoin.w - 60.0f * u, 30.0f * u}, t4, "UM CAPIT\xC3O TE PASSOU UM C\xD3" "DIGO DE 6 LETRAS?", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
            center({G.ovJoin.x + 30.0f * u, G.ovJoin.y + 344.0f * u, G.ovJoin.w - 60.0f * u, 30.0f * u}, t4, "ENTRAR NUMA TRIPULA\xC7\xC3O \xC9 TOTALMENTE GR\xC1TIS", 0.52f, 0.88f, 0.54f);
            center(G.ovJoinBtn, t5, teamWaitOp == e0net::TeamJoin ? "ENTRANDO..." : "DIGITAR C\xD3" "DIGO", 0.16f, 0.10f, 0.02f, 1.0f, true);

            center({G.ovFound.x, G.ovFound.y + 34.0f * u, G.ovFound.w, 42.0f * u}, t5, "FUNDAR SUA TRIPULA\xC7\xC3O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
            center({G.ovFound.x + 30.0f * u, G.ovFound.y + 308.0f * u, G.ovFound.w - 60.0f * u, 30.0f * u}, t4, locked ? "LIBERE BANDEIRA PR\xD3PRIA, NOME E CHAT" : "LICEN\xC7" "A ATIVA! PERSONALIZE SUA BANDEIRA E NOME", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
            char pr[64];
            if (locked) std::snprintf(pr, sizeof pr, teamWaitOp == e0net::TeamBuyLicense ? "COMPRANDO..." : "COMPRAR LICEN\xC7" "A  -  %d MOEDAS", e0net::kTeamLicenseCost);
            else std::snprintf(pr, sizeof pr, "CONFIGURAR E FUNDAR");
            center(G.ovFoundBtn, t5, pr, 0.16f, 0.10f, 0.02f, 1.0f, true);

            static const char* kPkT[3] = {"BANDEIRA PR\xD3PRIA", "NAVEGA\xC7\xC3O ALIADA", "DI\xC1RIO & CONVITES"};
            static const char* kPkD[3] = {"SEU ESTANDARTE APARECE NA TAG DOS TRIPULANTES", "VEJA ALIADOS NO MAPA E TRACE ROTA AT\xC9 ELES", "CHAT PRIVADO DE 24H E CONVITE DIRETO NO JOGO"};
            for (int i = 0; i < 3; ++i) {
                const Rect2D& p = G.ovPerk[i];
                shopBold(p.x + 96.0f * u, p.y + 42.0f * u, t4, kPkT[i], theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
                char lns[3][72]; int nl = wrapLinesPx(kPkD[i], t3, p.w - 48.0f * u, lns, 3);
                for (int k = 0; k < nl; ++k)
                    renderer.text(p.x + 26.0f * u, p.y + 106.0f * u + (float)k * (7.0f * (float)t3 + 10.0f * u), t3, lns[k], theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);
            }
        }
    }
    // ---- SEÇÃO 1: MEMBROS ----
    else if (teamSec == 1) {
        if (teamV.inTeam) {
            char hd[48]; std::snprintf(hd, sizeof hd, "TRIPULANTES  %d/%d", (int)teamV.members.size(), teamV.capacity());
            shopBold(G.memPane.x + 28.0f * u, G.memPane.y + 28.0f * u, t5, hd, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
            char f0[24], f1[24];
            std::snprintf(f0, sizeof f0, "TODOS (%d)", (int)teamV.members.size());
            std::snprintf(f1, sizeof f1, "NO MAR (%d)", teamOnlineCount());
            center(G.memFilt[0], t3, f0, teamMemFilter == 0 ? 0.16f : theme::kCream.r, teamMemFilter == 0 ? 0.10f : theme::kCream.g, teamMemFilter == 0 ? 0.02f : theme::kCream.b, 1.0f, teamMemFilter == 0);
            center(G.memFilt[1], t3, f1, teamMemFilter == 1 ? 0.16f : theme::kCream.r, teamMemFilter == 1 ? 0.10f : theme::kCream.g, teamMemFilter == 1 ? 0.02f : theme::kCream.b, 1.0f, teamMemFilter == 1);

            // Inspetor lateral direito
            if (teamSel >= 0 && teamSel < (int)teamV.members.size()) {
                const e0net::TeamMember& m = teamV.members[(size_t)teamSel];
                center({G.memInspect.x + 24.0f * u, G.memInspect.y + 214.0f * u, G.memInspect.w - 48.0f * u, 38.0f * u}, t5, m.name.c_str(), theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
                center({G.memInspect.x + 24.0f * u, G.memInspect.y + 258.0f * u, G.memInspect.w - 48.0f * u, 28.0f * u}, t3, m.owner ? "CAPIT\xC3O DA TRIPULA\xC7\xC3O" : (m.me ? "TRIPULANTE  (VOC\xCA)" : "TRIPULANTE"), theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
                // Telemetria / status dentro do cartão
                const float iy = G.memInspect.y + 324.0f * u;
                center({G.memInspect.x + 36.0f * u, iy, G.memInspect.w - 72.0f * u, 30.0f * u}, t4, m.online ? "STATUS: ONLINE NO MAR" : "STATUS: OFFLINE", m.online ? 0.48f : theme::kCreamMuted.r, m.online ? 0.88f : theme::kCreamMuted.g, m.online ? 0.50f : theme::kCreamMuted.b);
                char posInfo[72] = "FORA DO MAR NO MOMENTO";
                float px, pz;
                if (m.online && netGame && teamCrewPos(m.slot, px, pz)) {
                    char qc[12]; mapGridCoord(px, pz, qc, sizeof qc);
                    if (m.me) std::snprintf(posInfo, sizeof posInfo, "SEU QUADRANTE ATUAL: %s", qc);
                    else {
                        const e0net::PlayerState me = net.predicted();
                        const float d = std::hypot(px - me.x, pz - me.z);
                        char ds[24]; fmtDist(ds, sizeof ds, d);
                        std::snprintf(posInfo, sizeof posInfo, "DIST\xC2NCIA: %s  -  QUADRANTE %s", ds, qc);
                    }
                } else if (m.online) std::snprintf(posInfo, sizeof posInfo, "ATIVO NO SERVIDOR AGORA");
                center({G.memInspect.x + 36.0f * u, iy + 52.0f * u, G.memInspect.w - 72.0f * u, 30.0f * u}, t3, posInfo, theme::kCream.r, theme::kCream.g, theme::kCream.b);

                int acts[3]; const int na = teamMemberActs(acts);
                for (int k = 0; k < na; ++k) {
                    const Rect2D b = teamInspectActRect(G, k, na);
                    const bool arm = teamActArm == acts[k];
                    const char* lb2 = acts[k] == 2 ? "TRA\xC7" "AR ROTA AT\xC9 ELE" : (arm ? "TOQUE DE NOVO P/ CONFIRMAR" : (acts[k] == 0 ? "EXPULSAR DA TRIPULA\xC7\xC3O" : "PROMOVER A CAPIT\xC3O"));
                    center(b, t4, lb2, acts[k] == 2 ? 0.16f : 1.0f, acts[k] == 2 ? 0.10f : (acts[k] == 0 ? 0.78f : 0.90f), acts[k] == 2 ? 0.02f : (acts[k] == 0 ? 0.68f : 0.50f), 1.0f, acts[k] == 2);
                }
                if (na == 0 && !m.me) {
                    center({G.memInspect.x + 36.0f * u, G.memInspect.y + 540.0f * u, G.memInspect.w - 72.0f * u, 32.0f * u}, t3, "APENAS O CAPIT\xC3O GERENCIA MEMBROS", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
                }
            } else {
                center({G.memInspect.x + 24.0f * u, G.memInspect.y + 190.0f * u, G.memInspect.w - 48.0f * u, 34.0f * u}, t5, "HIERARQUIA", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
                char l1[64], l2[64], l3[64];
                std::snprintf(l1, sizeof l1, "CAPIT\xC3O:  %s", teamCaptainName().c_str());
                std::snprintf(l2, sizeof l2, "TRIPULANTES:  %d DE %d  (%d VAGAS)", (int)teamV.members.size(), teamV.capacity(), teamV.capacity() - (int)teamV.members.size());
                std::snprintf(l3, sizeof l3, "ONLINE NO MAR AGORA:  %d", teamOnlineCount());
                const float lx = G.memInspect.x + 54.0f * u;
                renderer.text(lx, G.memInspect.y + 286.0f * u, t3, l1, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
                renderer.text(lx, G.memInspect.y + 344.0f * u, t3, l2, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f, false);
                renderer.text(lx, G.memInspect.y + 402.0f * u, t3, l3, 0.50f, 0.88f, 0.52f, 0.98f, false);
                center({G.memInspect.x + 34.0f * u, G.memInspect.y + 530.0f * u, G.memInspect.w - 68.0f * u, 32.0f * u}, t3, "TOQUE NUM MEMBRO \xC0 ESQUERDA PARA OP\xC7\xD5" "ES", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
                center(G.memInviteBtn, t4, "CONVIDAR NOVO TRIPULANTE", 0.16f, 0.10f, 0.02f, 1.0f, true);
            }
        } else {
            center({G.main.x, G.main.y + 420.0f * u, G.main.w, 40.0f * u}, t5, "SEM TRIPULA\xC7\xC3O ATIVA", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
            center({G.main.x, G.main.y + 476.0f * u, G.main.w, 30.0f * u}, t4, "ENTRE COM UM C\xD3" "DIGO OU FUNDE UMA TRIPULA\xC7\xC3O PARA VER MEMBROS", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
            const Rect2D cb{G.main.x + G.main.w * 0.5f - 260.0f * u, G.main.y + 560.0f * u, 520.0f * u, 82.0f * u};
            center(cb, t4, "ENTRAR COM C\xD3" "DIGO", 0.16f, 0.10f, 0.02f, 1.0f, true);
        }
    }
    // ---- SEÇÃO 2: DIÁRIO & CHAT ----
    else if (teamSec == 2) {
        shopBold(G.main.x + 30.0f * u, G.main.y + 22.0f * u, t5, "DI\xC1RIO DE BORDO & CHAT", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        char cs[80];
        std::snprintf(cs, sizeof cs, "MENSAGENS ATIVAS: %d  -  AS MENSAGENS APAGAM AP\xD3S 24 HORAS", (int)teamMsgs.size());
        renderer.text(G.main.x + 30.0f * u, G.main.y + 22.0f * u + 7.0f * (float)t5 + 10.0f * u, t2, cs, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
        if (teamV.inTeam) {
            for (int k = 0; k < 4; ++k) center(G.chatQuick[k], t2, teamQuickMsg(k), theme::kCream.r, theme::kCream.g, theme::kCream.b);
            renderer.text(G.chatBar.x + 26.0f * u, G.chatBar.y + (G.chatBar.h - 7.0f * (float)t4) * 0.5f, t4, teamInput == 2 ? "ESCREVENDO..." : "TOQUE PARA ESCREVER NO DI\xC1RIO...", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 1.0f, false);
            center(G.chatSendBtn, t4, "ESCREVER", 0.16f, 0.10f, 0.02f, 1.0f, true);
            if (teamMsgs.empty()) center({G.chatList.x, G.chatList.y + G.chatList.h * 0.45f, G.chatList.w, 34.0f * u}, t4, "NENHUMA MENSAGEM AINDA  -  USE AS ORDENS R\xC1PIDAS ABAIXO!", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
        } else {
            center({G.main.x, G.main.y + 440.0f * u, G.main.w, 40.0f * u}, t5, "DI\xC1RIO TRANCADO", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
            center({G.main.x, G.main.y + 496.0f * u, G.main.w, 30.0f * u}, t4, "TOQUE PARA VOLTAR \xC0 VIS\xC3O GERAL E ENTRAR OU FUNDAR UMA TRIPULA\xC7\xC3O", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
        }
    }
    // ---- SEÇÃO 3: ESTANDARTE & GESTÃO ----
    else if (teamSec == 3) {
        const float dim = locked ? 0.40f : 1.0f;
        {
            const char* et = "ESTANDARTE DA TRIPULA\xC7\xC3O";
            const char* es = "EXIBIDO NA TAG DOS TRIPULANTES E NOS CONVITES";
            int tet = t5; while (tet > 2 && Renderer::textWidth(et, tet) > G.estLeft.w - 68.0f * u) --tet;
            int tes = t2; while (tes > 1 && Renderer::textWidth(es, tes) > G.estLeft.w - 68.0f * u) --tes;
            shopBold(G.estLeft.x + 34.0f * u, G.estLeft.y + 24.0f * u, tet, et, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
            renderer.text(G.estLeft.x + 34.0f * u, G.estLeft.y + 24.0f * u + 7.0f * (float)tet + 12.0f * u, tes, es, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
        }
        center(G.flagBtn, t4, canEdit ? "EDITAR BANDEIRA" : "S\xD3 O CAPIT\xC3O EDITA", 0.95f * dim, 0.88f * dim, 0.72f * dim);

        const float rx = G.estRight.x + 36.0f * u, rw = G.estRight.w - 72.0f * u;
        shopBold(rx, G.estRight.y + 24.0f * u, t5, "REGISTRO & GEST\xC3O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        label(rx, G.nameBox.y - 34.0f * u, "NOME DA TRIPULA\xC7\xC3O");
        {
            const std::string nm = teamV.inTeam ? teamV.name : teamDraftName;
            const bool empty = nm.empty();
            int t = t5; while (t > 2 && Renderer::textWidth(empty ? "TOQUE PARA ESCREVER" : nm.c_str(), t) > G.nameBox.w - 160.0f * u) --t;
            renderer.text(G.nameBox.x + 26.0f * u, G.nameBox.y + (G.nameBox.h - 7.0f * (float)t) * 0.5f, t, empty ? "TOQUE PARA ESCREVER" : nm.c_str(),
                          (empty ? 0.55f : 1.0f) * dim, (empty ? 0.53f : 0.95f) * dim, (empty ? 0.50f : 0.88f) * dim, 1.0f, false);
            if (canEdit && !locked) renderer.text(G.nameBox.x + G.nameBox.w - 26.0f * u - Renderer::textWidth("ALTERAR", t3), G.nameBox.y + (G.nameBox.h - 7.0f * (float)t3) * 0.5f, t3, "ALTERAR", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f, false);
        }
        if (locked) {
            const float ly = G.nameBox.y + G.nameBox.h + 72.0f * u;
            shopBold(rx + 26.0f * u, ly, t5, "LICEN\xC7" "A DE TRIPULA\xC7\xC3O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
            const char* ll = "PAGAMENTO \xDANICO PARA FUNDAR TRIPULA\xC7\xC3O, EDITAR BANDEIRA E CHAT";
            int tll = t2; while (tll > 1 && Renderer::textWidth(ll, tll) > rw - 52.0f * u) --tll;
            renderer.text(rx + 26.0f * u, ly + 48.0f * u, tll, ll, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.95f, false);
            renderer.text(rx + 26.0f * u, ly + 86.0f * u, tll, "TAMB\xC9M DISPON\xCDVEL NA LOJA DO JOGO", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.90f, false);
            char pr[64]; std::snprintf(pr, sizeof pr, teamWaitOp == e0net::TeamBuyLicense ? "COMPRANDO..." : "COMPRAR LICEN\xC7" "A  -  %d MOEDAS", e0net::kTeamLicenseCost);
            center(G.buyBtn, t5, pr, 0.16f, 0.10f, 0.02f, 1.0f, true);
        } else if (!teamV.inTeam) {
            const bool ready = (int)teamDraftName.size() >= e0net::kTeamNameMin;
            label(rx, G.nameBox.y + G.nameBox.h + 36.0f * u, ready ? "LICEN\xC7" "A ATIVA: TUDO PRONTO PARA FUNDAR SUA TRIPULA\xC7\xC3O" : "LICEN\xC7" "A ATIVA: ESCOLHA UM NOME ACIMA PARA FUNDAR");
            center(G.mainBtn, t5, teamWaitOp == e0net::TeamCreate ? "FUNDANDO..." : "FUNDAR TRIPULA\xC7\xC3O", ready ? 0.16f : 0.80f, ready ? 0.10f : 0.76f, ready ? 0.02f : 0.70f, 1.0f, ready);
        } else {
            renderer.text(rx + 24.0f * u, G.inviteBtn.y - 6.0f * u, t2, "C\xD3" "DIGO DE RECRUTAMENTO", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
            shopBold(rx + 24.0f * u, G.inviteBtn.y + 24.0f * u, t6, teamV.code.c_str(), theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
            center(G.inviteBtn, t4, "CONVIDAR", 0.16f, 0.10f, 0.02f, 1.0f, true);

            const float ry = G.inviteBtn.y + G.inviteBtn.h + 70.0f * u;
            shopBold(rx + 24.0f * u, ry, t4, "REGRAS & PERMISS\xD5" "ES", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f, false);
            static const char* kRules[3] = {
                "- CAPIT\xC3O EDITA NOME, BANDEIRA, PROMOVE E EXPULSA",
                "- CONVITE NO JOGO: SEGURE EM UM JOGADOR A AT\xC9 8 M",
                "- VAGAS: 10 NO COME\xC7O, AT\xC9 30 COMPRANDO NA LOJA"
            };
            int tr = t2;
            for (int k = 0; k < 3; ++k) while (tr > 1 && Renderer::textWidth(kRules[k], tr) > rw - 52.0f * u) --tr;
            for (int k = 0; k < 3; ++k)
                renderer.text(rx + 24.0f * u, ry + 48.0f * u + (float)k * 38.0f * u, tr, kRules[k], theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);

            const char* lb = teamLeaveArm ? "TOQUE NOVAMENTE PARA CONFIRMAR" : (teamV.owner ? "DESFAZER TRIPULA\xC7\xC3O" : "SAIR DA TRIPULA\xC7\xC3O");
            center(G.leaveBtn, t4, lb, 1.0f, 0.78f, 0.68f);
        }
    }

    // ---- SEÇÃO 4: COFRE & LOJA ----
    else if (teamSec == 4) {
        const float lx = G.vaultL.x + 30.0f * u, lw = G.vaultL.w - 60.0f * u;
        shopBold(lx, G.vaultL.y + 24.0f * u, t5, "COFRE DA TRIPULA\xC7\xC3O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        shopBold(G.vaultR.x + 30.0f * u, G.vaultR.y + 24.0f * u, t5, "LOJA DA TRIPULA\xC7\xC3O", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        if (!teamV.inTeam) {
            center({G.main.x, G.main.y + 400.0f * u, G.main.w, 40.0f * u}, t5, "COFRE E LOJA TRANCADOS", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, true);
            center({G.main.x, G.main.y + 456.0f * u, G.main.w, 30.0f * u}, t4, "ENTRE OU FUNDE UMA TRIPULA\xC7\xC3O NA VIS\xC3O GERAL", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
        } else {
            renderer.text(lx, G.vaultL.y + 24.0f * u + 7.0f * (float)t5 + 12.0f * u, t2, "TODOS DEPOSITAM  -  S\xD3 O CAPIT\xC3O SACA E COMPRA", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
            char b[64]; coinText(b, sizeof b, teamV.treasury);
            center({G.vaultL.x, G.vaultL.y + 250.0f * u, G.vaultL.w, 72.0f * u}, t6 + 2, b, 1.0f, 0.90f, 0.55f, 1.0f, true);
            center({G.vaultL.x, G.vaultL.y + 330.0f * u, G.vaultL.w, 28.0f * u}, t3, "MOEDAS NO COFRE", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b);
            { char cv[24]; coinText(cv, sizeof cv, teamV.coins); std::snprintf(b, sizeof b, "SEU SALDO: %s", cv); }
            center({G.vaultL.x, G.vaultL.y + 366.0f * u, G.vaultL.w, 26.0f * u}, t2, b, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
            label(lx, G.depBtn[0].y - 36.0f * u, "DEPOSITAR (DA SUA CONTA PARA O COFRE)");
            label(lx, G.wdBtn[0].y - 36.0f * u, teamV.owner ? "SACAR (DO COFRE PARA A SUA CONTA)" : "SACAR  -  S\xD3 O CAPIT\xC3O");
            for (int k = 0; k < 4; ++k) {
                char a[16]; if (k < 3) coinText(a, sizeof a, kTeamAmt[k]); else std::snprintf(a, sizeof a, "TUDO");
                center(G.depBtn[k], t3, a, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f, true);
                const float d = teamV.owner ? 1.0f : 0.5f;
                center(G.wdBtn[k], t3, a, theme::kCream.r * d, theme::kCream.g * d, theme::kCream.b * d, 1.0f, true);
            }
            renderer.text(G.vaultR.x + 30.0f * u, G.vaultR.y + 24.0f * u + 7.0f * (float)t5 + 12.0f * u, t2, "PAGO COM O COFRE  -  SERVE A TODOS OS TRIPULANTES", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
            for (int k = 0; k < 5; ++k) {
                const Rect2D& r = G.shopRow[k]; int price; const char* st = teamShopState(k, price);
                const bool buyable = std::strcmp(st, "COMPRAR") == 0, armed = teamActArm == 100 + k;
                char nm[64], d1[96];
                if (k == 0) {
                    std::snprintf(nm, sizeof nm, "+%d VAGAS NA TRIPULA\xC7\xC3O", e0net::kTeamSlotStep);
                    if (teamV.slotLevel >= e0net::kTeamSlotLevels) std::snprintf(d1, sizeof d1, "CAPACIDADE M\xC1XIMA: %d TRIPULANTES", teamV.capacity());
                    else std::snprintf(d1, sizeof d1, "N\xCDVEL %d/%d  -  %d PARA %d TRIPULANTES", (int)teamV.slotLevel, e0net::kTeamSlotLevels, teamV.capacity(), e0net::teamCapacity(teamV.slotLevel + 1));
                } else {
                    std::snprintf(nm, sizeof nm, "%s", e0net::shipDef(k - 1).name);
                    std::snprintf(d1, sizeof d1, "TODOS CHAMAM DE GRA\xC7" "A, SEM ESPERA");
                }
                const float tx = r.x + 108.0f * u, tw = G.shopBuyBtn[k].x - tx - 14.0f * u;
                int tn = t4; while (tn > 2 && Renderer::textWidth(nm, tn) > tw) --tn;
                int td = t2; while (td > 1 && Renderer::textWidth(d1, td) > tw) --td;
                shopBold(tx, r.y + r.h * 0.5f - 7.0f * (float)tn - 4.0f * u, tn, nm, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f, false);
                renderer.text(tx, r.y + r.h * 0.5f + 8.0f * u, td, d1, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.9f, false);
                if (buyable) {
                    char pr[24]; if (armed) std::snprintf(pr, sizeof pr, "CONFIRMAR"); else coinText(pr, sizeof pr, price);
                    const bool ok = teamV.treasury >= price;
                    center(G.shopBuyBtn[k], t3, teamWaitOp == e0net::TeamBuy ? "..." : pr, ok ? 0.16f : 0.80f, ok ? 0.10f : 0.76f, ok ? 0.02f : 0.70f, 1.0f, ok);
                } else center(G.shopBuyBtn[k], t3, st, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b);
            }
        }
    }

    // Status conexão e toast
    if (!teamHave) {
        const char* s = teamConn == 3 ? "MODO LOCAL: SEM SERVIDOR" : (teamConn == 2 ? "SEM CONEX\xC3O COM O SERVIDOR" : "CONECTANDO...");
        renderer.text(G.side.x, G.side.y - 26.0f * u, t2, s, 1.0f, 0.55f, 0.45f, 0.9f, false);
    }
    if (teamMsgT > 0.0f) {
        const float a = std::min(1.0f, teamMsgT * 3.0f);
        const Rect2D r = teamToastRect(G);
        int t = t4; while (t > 2 && Renderer::textWidth(teamMsg.c_str(), t) > r.w - 40.0f * u) --t;
        shopBold(r.x + (r.w - Renderer::textWidth(teamMsg.c_str(), t)) * 0.5f, r.y + (r.h - 7.0f * (float)t) * 0.5f, t, teamMsg.c_str(), 1.0f, 0.85f, 0.35f, a, false);
    }
}

// Lista rolável da seção 1 (MEMBROS) com GL_SCISSOR_TEST + barra de rolagem
void drawTeamMemList(const TeamGeo& G) {
    if (teamSec != 1 || !teamV.inTeam) return;
    const float u = G.u; const Rect2D& L = G.memList;
    std::vector<int> list = teamFilteredMembers(teamMemFilter);
    const float totalH = (float)list.size() * G.memRowH;
    const float maxS = std::max(0.0f, totalH - L.h);
    teamMemScroll = std::max(0.0f, std::min(maxS, teamMemScroll));

    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)L.x, (GLint)((float)h - L.y - L.h), (GLsizei)L.w, (GLsizei)L.h);
    renderer.beginUi();
    for (int row = 0; row < (int)list.size(); ++row) {
        const float ry = L.y + (float)row * G.memRowH - teamMemScroll;
        const float rh = G.memRowH - 10.0f * u;
        if (ry + rh < L.y || ry > L.y + L.h) continue;
        const int idx = list[(size_t)row];
        const e0net::TeamMember& m = teamV.members[(size_t)idx];
        const bool sel = (teamSel == idx);
        const float rw = maxS > 1.0f ? L.w - 18.0f * u : L.w;
        renderer.uiWoodPanel(L.x + rw * 0.5f, ry + rh * 0.5f, rw * 0.5f, rh * 0.5f, 10.0f * u, sel ? theme::PanelCardSel : (m.me ? theme::PanelSlotHot : theme::PanelSlot), 0.95f, (float)row);
        renderer.uiShape(Renderer::UiFill, L.x + 28.0f * u, ry + rh * 0.5f, 8.5f * u, m.online ? 0.34f : 0.42f, m.online ? 0.80f : 0.38f, m.online ? 0.42f : 0.34f, 1.0f);
        if (m.owner) renderer.uiIconTint(HiCrown, L.x + rw - 36.0f * u, ry + rh * 0.5f, 21.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    }
    renderer.endUi();

    renderer.beginText();
    const int t4 = teamTs(3.6f), t3 = teamTs(2.8f), t2 = teamTs(2.3f);
    if (list.empty()) {
        const char* em = "NENHUM TRIPULANTE NO MAR AGORA";
        renderer.text(L.x + (L.w - Renderer::textWidth(em, t3)) * 0.5f, L.y + L.h * 0.44f, t3, em, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
    }
    for (int row = 0; row < (int)list.size(); ++row) {
        const float ry = L.y + (float)row * G.memRowH - teamMemScroll;
        const float rh = G.memRowH - 10.0f * u;
        if (ry + rh < L.y || ry > L.y + L.h) continue;
        const int idx = list[(size_t)row];
        const e0net::TeamMember& m = teamV.members[(size_t)idx];
        const float rw = maxS > 1.0f ? L.w - 18.0f * u : L.w;
        std::string nm = m.name + (m.me ? "  (VOC\xCA)" : "");
        char dist[28] = "";
        float px, pz;
        if (!m.me && netGame && m.online && teamCrewPos(m.slot, px, pz)) {
            const e0net::PlayerState me = net.predicted();
            const float d = std::hypot(px - me.x, pz - me.z);
            if (d >= 1000.0f) std::snprintf(dist, sizeof dist, "%.1f KM", d / 1000.0f);
            else std::snprintf(dist, sizeof dist, "%d M", (int)d);
        } else std::snprintf(dist, sizeof dist, "%s", m.online ? "ONLINE" : "OFFLINE");
        int t = t4; while (t > 2 && Renderer::textWidth(nm.c_str(), t) > rw - 200.0f * u) --t;
        renderer.text(L.x + 54.0f * u, ry + (rh - 7.0f * (float)t) * 0.5f, t, nm.c_str(), m.owner ? theme::kBrassBright.r : theme::kCream.r, m.owner ? theme::kBrassBright.g : theme::kCream.g, m.owner ? theme::kBrassBright.b : theme::kCream.b, 1.0f, false);
        const float dx = L.x + rw - (m.owner ? 72.0f : 26.0f) * u - Renderer::textWidth(dist, t2);
        renderer.text(dx, ry + (rh - 7.0f * (float)t2) * 0.5f, t2, dist, m.online ? 0.52f : theme::kCreamMuted.r, m.online ? 0.88f : theme::kCreamMuted.g, m.online ? 0.52f : theme::kCreamMuted.b, 0.95f, false);
    }
    renderer.endText();
    glDisable(GL_SCISSOR_TEST);

    if (maxS > 1.0f) {
        renderer.beginUi();
        const float view = L.h / (L.h + maxS), pos = std::max(0.0f, std::min(1.0f, teamMemScroll / maxS));
        const float bl = std::max(44.0f * u, L.h * view), off = (L.h - bl) * pos, bx = L.x + L.w - 6.0f * u;
        renderer.uiRoundRect(bx, L.y + L.h * 0.5f, 3.0f * u, L.h * 0.5f, 3.0f * u, 1.0f, 1.0f, 1.0f, 0.08f);
        renderer.uiRoundRect(bx, L.y + off + bl * 0.5f, 3.0f * u, bl * 0.5f, 3.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.85f);
        renderer.endUi();
    }
}

// Mensagens do chat na seção 2 (DIÁRIO & CHAT) em largura total
void drawTeamChat(const TeamGeo& G) {
    if (teamSec != 2 || !teamV.inTeam || teamMsgs.empty()) return;
    const float u = G.u; const Rect2D& L = G.chatList;
    const int t4 = teamTs(3.8f), t3 = teamTs(2.8f);
    const float maxS = std::max(0.0f, teamChatContentH(G) - L.h);
    teamChatScroll = std::max(0.0f, std::min(maxS, teamChatScroll));
    const double now = teamNow();
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)L.x, (GLint)((float)h - L.y - L.h), (GLsizei)L.w, (GLsizei)L.h);
    struct Item { float y, hgt; int idx; int nl; };
    std::vector<Item> items;
    float yb = L.y + L.h - 10.0f * u + teamChatScroll;
    char lines[4][72];
    for (int i = (int)teamMsgs.size() - 1; i >= 0; --i) {
        int nl; const float hh = teamMsgHeight(G, teamMsgs[(size_t)i], nl, lines);
        yb -= hh;
        if (yb + hh >= L.y && yb <= L.y + L.h) items.push_back({yb, hh, i, nl});
        yb -= 14.0f * u;
        if (yb < L.y - 600.0f * u) break;
    }
    const float bubW = std::min(L.w * 0.72f, 1100.0f * u);
    renderer.beginUi();
    for (const Item& it : items) {
        const TeamMsgL& m = teamMsgs[(size_t)it.idx];
        if (m.m.author.empty()) continue;
        const float bx = m.m.mine ? L.x + L.w - 22.0f * u - bubW : L.x + 16.0f * u;
        renderer.uiWoodPanel(bx + bubW * 0.5f, it.y + it.hgt * 0.5f, bubW * 0.5f, it.hgt * 0.5f, 12.0f * u, m.m.mine ? theme::PanelSlotHot : theme::PanelCard, 0.95f, (float)it.idx);
    }
    renderer.endUi();
    renderer.beginText();
    for (const Item& it : items) {
        const TeamMsgL& m = teamMsgs[(size_t)it.idx];
        int nl; teamMsgHeight(G, m, nl, lines);
        if (m.m.author.empty()) {
            float y = it.y + 11.0f * u;
            for (int k = 0; k < nl; ++k) { const float tw = Renderer::textWidth(lines[k], t3); renderer.text(L.x + (L.w - tw) * 0.5f, y, t3, lines[k], 0.85f, 0.70f, 0.38f, 0.95f, false); y += 7.0f * (float)t3 + 8.0f * u; }
            continue;
        }
        const float bx = m.m.mine ? L.x + L.w - 22.0f * u - bubW : L.x + 16.0f * u;
        char ag[16]; teamAge(ag, sizeof ag, (double)m.m.age + (now - m.at));
        const std::string who = m.m.mine ? std::string("VOC\xCA") : m.m.author;
        renderer.text(bx + 24.0f * u, it.y + 14.0f * u, t3, who.c_str(), theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        renderer.text(bx + bubW - 24.0f * u - Renderer::textWidth(ag, t3), it.y + 14.0f * u, t3, ag, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.9f, false);
        float y = it.y + 14.0f * u + 7.0f * (float)t3 + 12.0f * u;
        for (int k = 0; k < nl; ++k) { renderer.text(bx + 24.0f * u, y, t4, lines[k], theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f, false); y += 7.0f * (float)t4 + 10.0f * u; }
    }
    renderer.endText();
    glDisable(GL_SCISSOR_TEST);
    if (maxS > 1.0f) {
        renderer.beginUi();
        const float view = L.h / (L.h + maxS), pos = 1.0f - std::max(0.0f, std::min(1.0f, teamChatScroll / maxS));
        const float bl = std::max(40.0f * u, L.h * view), off = (L.h - bl) * pos, bx = L.x + L.w - 8.0f * u;
        renderer.uiRoundRect(bx, L.y + L.h * 0.5f, 3.0f * u, L.h * 0.5f, 3.0f * u, 1.0f, 1.0f, 1.0f, 0.08f);
        renderer.uiRoundRect(bx, L.y + off + bl * 0.5f, 3.0f * u, bl * 0.5f, 3.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.85f);
        renderer.endUi();
    }
}
void drawTeamScreen() {
    glClearColor(0.02f, 0.02f, 0.025f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (flagOpen) { drawFlagScreen(); return; }
    const TeamGeo G = teamGeo();
    renderer.beginUi(); drawTeamShapes(G); renderer.endUi();
    renderer.beginText(); drawTeamText(G); renderer.endText();
    drawTeamMemList(G);
    drawTeamChat(G);
}

// ================= TRIPULAÇÃO NO JOGO =================
// - TAG: a bandeira da tripulação de cada jogador fica à ESQUERDA do id (o servidor diz a tripulação de cada id: MsgTeamQuery/MsgTeamInfo,
//   cache de 15 s por id; mudanças chegam na hora pelo MsgTeamInfo que o servidor manda a quem está perto).
// - CONVIDAR: item do menu de SEGURAR o dedo no jogador (rmItems em engine.cpp): só aparece se eu tenho tripulação e o outro não.
//   O convidado vê um cartão no topo com a bandeira e ACEITAR / RECUSAR (some em 60 s). O servidor confere tudo (shared/team.h).
struct TagFlagReq { float x, y, w, h, a; uint8_t flag[e0net::kFlagBytes]; };
std::vector<TagFlagReq> tagFlags;
float teamQueryT = 0.0f;
bool teamOfferOn = false; NetClient::TeamOffer teamOfferCur; float teamOfferT = 0.0f;
double teamInviteAt = -1e9; int teamInviteTo = -1;

bool teamCanInvite(int id) const {   // eu numa tripulação (ou ainda não sei) e ele sem tripulação (ou ainda não sei)
    if (!net.connected() || id == net.myId()) return false;
    const NetClient::TeamInfo& me = net.teamInfo(net.myId());
    if (me.known && me.team == 0) return false;
    if (!me.known) return false;
    const NetClient::TeamInfo& t = net.teamInfo(id);
    return !t.known || t.team == 0;
}
void teamInvite(int id) {
    if (teamNow() - teamInviteAt < e0net::kTeamInviteGap) { notify("ESPERE UM POUCO PARA CONVIDAR DE NOVO"); return; }
    teamInviteAt = teamNow(); teamInviteTo = id;
    LOG("tripulacao: convidando o jogador %d", id);
    net.inviteTeam((uint16_t)id);
}
void updateTeamGame(float dt) {
    if (!net.connected()) { teamOfferOn = false; return; }
    teamQueryT -= dt;
    if (teamQueryT <= 0.0f) {   // pede a tripulação de quem tem TAG na tela (e a minha) se o que sei tem mais de 15 s
        teamQueryT = 0.5f;
        const double now = teamNow();   // mesmo relógio (steady_clock) do NetClient
        uint16_t ids[e0net::kTeamQueryMax]; int n = 0;
        auto want = [&](int id) {
            if (id < 0 || id >= e0net::kMaxPlayers || n >= e0net::kTeamQueryMax) return;
            if (now - net.teamInfo(id).at > 15.0) { for (int i = 0; i < n; ++i) if (ids[i] == id) return; ids[n++] = (uint16_t)id; }
        };
        want(net.myId());
        for (const auto& t : tags) if (!t.self) want(t.id);
        if (n) net.queryTeams(ids, n);
    }
    for (NetClient::TeamOffer o; net.popTeamOffer(o); ) {   // convite recebido
        if (selfDead) { net.answerTeamOffer(o.id, false); continue; }
        teamOfferCur = o; teamOfferOn = true; teamOfferT = (float)e0net::kTeamOfferTtl - 2.0f;
    }
    if (teamOfferOn && (teamOfferT -= dt) <= 0.0f) teamOfferOn = false;
    for (NetClient::TeamNotice tn; net.popTeamNotice(tn); ) {
        char t[112];
        switch (tn.kind) {
            case e0net::TnInviteSent:
                if (tn.result == e0net::TeamOk) std::snprintf(t, sizeof t, "CONVITE ENVIADO PARA O ID %03d", (int)tn.other);
                else std::snprintf(t, sizeof t, "%s", e0net::teamResultText(tn.result));
                if (tn.result == e0net::TeamErrTargetTeam || tn.result == e0net::TeamErrNoTeam) { net.forgetTeam(tn.other); net.forgetTeam(net.myId()); }
                break;
            case e0net::TnAccepted: std::snprintf(t, sizeof t, "O ID %03d ENTROU NA SUA TRIPULA\xC7\xC3" "O", (int)tn.other); net.forgetTeam(tn.other); break;
            case e0net::TnDeclined: std::snprintf(t, sizeof t, "O ID %03d RECUSOU O CONVITE", (int)tn.other); break;
            case e0net::TnJoined:
                if (tn.result == e0net::TeamOk) std::snprintf(t, sizeof t, "BEM-VINDO \xC0 %s", tn.name.c_str());   // curto: a linha da notificação tem largura fixa
                else std::snprintf(t, sizeof t, "%s", e0net::teamResultText(tn.result));
                net.forgetTeam(net.myId());
                break;
            case e0net::TnOfferGone: std::snprintf(t, sizeof t, "ESSE CONVITE N\xC3" "O VALE MAIS"); break;
            default: continue;
        }
        notify(t);
    }
}
// cartão do convite: topo central
Rect2D teamOfferRect() const { const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f); const float cw = 900.0f * u, ch = 250.0f * u; return {((float)w - cw) * 0.5f, 120.0f * u, cw, ch}; }
Rect2D teamOfferBtn(int k) const {   // 0 ACEITAR, 1 RECUSAR
    const Rect2D c = teamOfferRect(); const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
    const float bw = (c.w - 330.0f * u) * 0.5f, bh = 74.0f * u;
    return {c.x + 290.0f * u + (float)k * (bw + 20.0f * u), c.y + c.h - bh - 24.0f * u, bw, bh};
}
bool teamOfferTouch(float x, float y) {
    if (!teamOfferOn || uiFull()) return false;
    if (!teamOfferRect().contains(x, y)) return false;
    for (int k = 0; k < 2; ++k) if (teamOfferBtn(k).contains(x, y)) {
        net.answerTeamOffer(teamOfferCur.id, k == 0);
        teamOfferOn = false;
        if (k == 1) notify("CONVITE RECUSADO");
        return true;
    }
    return true;   // toque no cartão: consumido
}
void drawTeamOffer() {   // passe de texto (formas com renderer.rect); a bandeira vai na fila tagFlags
    if (!teamOfferOn || uiFull()) return;
    const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
    const Rect2D c = teamOfferRect();
    const float a = std::min(1.0f, teamOfferT * 2.0f) * std::min(1.0f, ((float)e0net::kTeamOfferTtl - 2.0f - teamOfferT) * 5.0f + 0.2f);
    renderer.rect(c.x - 3.0f * u, c.y - 3.0f * u, c.w + 6.0f * u, c.h + 6.0f * u, 1.0f, 0.78f, 0.25f, 0.85f * a);
    renderer.rect(c.x, c.y, c.w, c.h, 0.06f, 0.05f, 0.04f, 0.95f * a);
    TagFlagReq fr; fr.h = 70.0f * u; fr.w = fr.h * 1.5f; fr.x = c.x + 30.0f * u; fr.y = c.y + c.h * 0.5f - fr.h * 0.5f; fr.a = a; std::memcpy(fr.flag, teamOfferCur.flag, e0net::kFlagBytes);
    tagFlags.push_back(fr);
    const int t5 = std::max(2, (int)std::lround(5.0f * u)), t4 = std::max(2, (int)std::lround(4.0f * u)), t3 = std::max(2, (int)std::lround(3.0f * u));
    char l1[64]; std::snprintf(l1, sizeof l1, "O ID %03d TE CONVIDOU PARA A TRIPULA\xC7\xC3" "O", (int)teamOfferCur.from);
    renderer.text(c.x + 290.0f * u, c.y + 26.0f * u, t3, l1, 0.82f, 0.78f, 0.70f, a, false);
    int tn = t5; while (tn > 2 && Renderer::textWidth(teamOfferCur.name.c_str(), tn) > c.w - 320.0f * u) --tn;
    renderer.text(c.x + 290.0f * u, c.y + 26.0f * u + 7.0f * (float)t3 + 14.0f * u, tn, teamOfferCur.name.c_str(), 1.0f, 0.86f, 0.40f, a);
    char l2[48]; std::snprintf(l2, sizeof l2, "%d MEMBROS  -  %d S", (int)teamOfferCur.members, (int)std::ceil(teamOfferT));
    renderer.text(c.x + 290.0f * u, c.y + 26.0f * u + 7.0f * (float)t3 + 14.0f * u + 7.0f * (float)tn + 12.0f * u, t3, l2, 0.62f, 0.60f, 0.56f, a, false);
    for (int k = 0; k < 2; ++k) {
        const Rect2D b = teamOfferBtn(k);
        if (k == 0) renderer.rect(b.x, b.y, b.w, b.h, 0.95f, 0.72f, 0.20f, 0.97f * a);
        else { renderer.rect(b.x, b.y, b.w, b.h, 0.55f, 0.20f, 0.18f, 0.9f * a); renderer.rect(b.x + 2.0f, b.y + 2.0f, b.w - 4.0f, b.h - 4.0f, 0.18f, 0.06f, 0.05f, 0.95f * a); }
        const char* lb = k == 0 ? "ACEITAR" : "RECUSAR";
        renderer.text(b.x + (b.w - Renderer::textWidth(lb, t4)) * 0.5f, b.y + (b.h - 7.0f * (float)t4) * 0.5f, t4, lb, k == 0 ? 0.16f : 1.0f, k == 0 ? 0.10f : 0.75f, k == 0 ? 0.02f : 0.70f, a, false);
    }
}
void drawTagFlags() {   // bandeiras das TAGs e do convite (depois do passe de texto)
    if (tagFlags.empty()) return;
    renderer.beginUi();
    for (const TagFlagReq& q : tagFlags) {
        FlagDesign fd; if (!flagFromBytes(q.flag, fd)) continue;
        FlagLook L = flagLook(fd);
        renderer.uiRoundRect(q.x + q.w * 0.5f + 1.5f, q.y + q.h * 0.5f + 1.5f, q.w * 0.5f, q.h * 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.45f * q.a);   // sombra
        renderer.uiFlag(L, q.x + q.w * 0.5f, q.y + q.h * 0.5f, q.w * 0.5f, q.h * 0.5f, (float)std::fmod(teamNow(), 1000.0), 0.35f, q.a);
    }
    renderer.endUi();
    tagFlags.clear();
}

// ================= COOP no jogo: chat chegando, botão com não lidas, rota até um tripulante, token do PUSH =================
int teamUnread = 0;                    // mensagens da tripulação que chegaram com o painel fechado (bolinha no botão)
std::string teamPushSent; float teamPushT = 2.0f;
// pedido "de fundo" (não trava os botões do painel): só o token do push por enquanto
void teamSendBg(e0net::TeamRequest r) {
    if (!auth.signedIn()) return;
    const Auth::Account& A = auth.account();
    if (A.cloud) { if (auth.idToken().empty()) return; r.authKind = e0net::LoginFirebase; r.token = auth.idToken(); }
    else { r.authKind = e0net::LoginLocal; r.uid = A.uid; }
    r.playerName = teamPlayerName(); r.since = teamSince;
    std::string ip; int port;
    if (!teamServer(ip, port)) return;
    teamNet.setServer(ip, port);
    teamNet.send(r);
}
// a cada 5 s: se o Firebase deu um token novo para este aparelho, manda ao servidor (notificações do chat com o jogo fechado)
void teamPushTick(float dt) {
    if (!teamOpen && !netGame) for (TeamClient::Reply R; teamNet.poll(R); ) teamApply(R);   // menu com o painel fechado: respostas de fundo
    if ((teamPushT -= dt) > 0.0f) return;
    teamPushT = 5.0f;
    if (!auth.signedIn()) return;
    const std::string tok = e0::pushToken(app->activity);
    if (tok.empty() || tok == teamPushSent) return;
    if (auth.account().cloud && auth.idToken().empty()) return;
    e0net::TeamRequest r; r.op = e0net::TeamPushToken; r.text = tok;
    teamSendBg(r);
    teamPushSent = tok;
    LOG("tripulacao: token FCM enviado ao servidor (...%s)", tok.size() > 8 ? tok.substr(tok.size() - 8).c_str() : tok.c_str());
}
void updateTeamCoop(float dt) {
    if (!teamOpen) for (TeamClient::Reply R; teamNet.poll(R); ) if (R.op != e0net::TeamPushToken) teamApply(R);   // respostas de fundo
    for (NetClient::TeamChatPush m; net.popTeamChat(m); ) {
        if (teamOpen) { teamPollT = 0.0f; continue; }   // painel aberto: atualiza o chat na hora
        if (m.system) { notify(("TRIPULA\xC7\xC3" "O: " + m.text).c_str()); continue; }
        ++teamUnread;
        if (pushQ.size() < 4) { PushBanner b; b.title = "TRIPULA\xC7\xC3" "O - " + m.author; b.body = m.text; b.action = 5; pushQ.push_back(b); }
    }
    if (wpCrew >= 0) {   // ROTA seguindo um tripulante
        float x, z;
        if (!wpSet) wpCrew = -1;
        else if (teamCrewPos(wpCrew, x, z)) { wpX = x; wpZ = z; }
    }
}
Rect2D teamBtnRect() const { return menuBtnRect(3); }
void drawTeamBtn() {
    const Rect2D r = teamBtnRect();
    hudIcon(HiSabres, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.h * 0.44f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    if (teamUnread > 0) {   // não lidas: selo carmesim com o número
        char n[8]; std::snprintf(n, sizeof n, "%d", std::min(teamUnread, 99));
        const int ts = std::max(1, uiScale() - 1);
        const float bw = std::max(9.0f * (float)ts, Renderer::textWidth(n, ts) + 4.0f * (float)ts), bh = 9.0f * (float)ts;
        const float bx = r.x + r.w - bw * 0.6f, by = r.y - bh * 0.4f;
        renderer.rect(bx - 1.5f, by - 1.5f, bw + 3.0f, bh + 3.0f, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.95f);
        renderer.rect(bx, by, bw, bh, theme::kWaxRed.r, theme::kWaxRed.g, theme::kWaxRed.b, 0.98f);
        renderer.text(bx + (bw - Renderer::textWidth(n, ts)) * 0.5f, by + ts, ts, n, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f, false);
    }
}
// minimapa/mapa: tripulantes online (verde). Fora do alcance do minimapa ficam encostados na borda, na direção deles.
bool crewMember(int id) const {
    if (!net.connected()) return false;
    const NetClient::TeamInfo& me = net.teamInfo(net.myId()), &t = net.teamInfo(id);
    return me.known && me.team && t.known && t.team == me.team;
}
