// Incluído DENTRO de struct Engine (engine.cpp). CONTA + INVENTÁRIO/MOEDAS DO SERVIDOR (protocolo v42; ver CLAUDE.md "PERSISTÊNCIA NO SERVIDOR").
// Online, o servidor é a fonte da verdade: o cliente PREVÊ as mudanças (o item some/aparece na hora) e manda a ação (MsgAct); quando não
// há mais ação em voo ele ADOTA o inventário + moedas do último MsgInvSync (net.invAdoptable). Coisas que só o servidor decide (coletar,
// pegar do chão/baú, presentes, moedas) não são previstas: chegam pela sincronização. Offline (sem servidor) tudo continua local como antes.

bool netGame = false;               // o jogo foi iniciado com servidor (beginGame): moedas/recibos/inventário vão para ele
bool flagUnsynced = false;          // bandeira salva enquanto o servidor não sabia (menu.cfg flagsync=0): sobe no próximo login
bool walletNoticeDone = false;      // aviso único "moedas agora ficam no servidor" (menu.cfg walletmig=1)
float authPollT = 0.0f, tokenAskT = 0.0f, vitalsT = 0.0f;
int vitalsSentHp = -1, vitalsSentHu = -1;
std::string loginTokenSent;
bool invEverAdopted = false;

bool invServer() const { return net.serverInv(); }
// configura quem sou para o servidor (Firebase com idToken, ou visitante local)
void configureLogin() {
    loginTokenSent = auth.idToken();
    if (!auth.signedIn()) { net.setLogin(0, "", "", ""); return; }
    const Auth::Account& A = auth.account();
    if (A.cloud) net.setLogin(e0net::LoginFirebase, A.uid, auth.idToken(), auth.prevLocal());
    else net.setLogin(e0net::LoginLocal, A.uid, "", "");
}
// bandeira <-> 11 bytes (validados pelas mesmas regras do menu.cfg)
static void flagToBytes(const FlagDesign& d, uint8_t* out) { std::memcpy(out, &d, e0net::kFlagBytes); }
bool flagFromBytes(const uint8_t* b, FlagDesign& out) {
    std::string s; for (int i = 0; i < e0net::kFlagBytes; ++i) { if (i) s += ','; s += std::to_string((int)b[i]); }
    return flagFromCfg(s, out);
}
void flagSaved_changed() {   // chamado ao SALVAR no editor de bandeira
    if (net.connected() && net.loginSettled()) { uint8_t b[e0net::kFlagBytes]; flagToBytes(flagSaved, b); net.sendProfile(b); flagUnsynced = false; }
    else flagUnsynced = true;
    saveCfg();
}
void adoptServerInv() {
    int32_t c = 0;
    const int heldBefore = hotbarCur >= 0 ? inv[kInvHotFirst + hotbarCur].id : 0;
    net.invAdopt(inv, c);
    if (!invEverAdopted && walletLoaded && !walletNoticeDone && coins > 0 && c != coins) {
        walletNoticeDone = true; saveCfg();
        notify("SUAS MOEDAS AGORA FICAM NO SERVIDOR");
        notify("COMPRAS REAIS SAO RESTAURADAS PELO RECIBO");
    }
    invEverAdopted = true;
    coins = c;
    if (invSel >= 0 && (invSel >= kInvSlots || !inv[invSel].id)) invSel = -1;
    if (chestSelSide == 2 && (chestSelIdx < 0 || !inv[chestSelIdx].id)) { chestSelSide = 0; chestSelIdx = -1; }
    if (heldBefore == ItGun && (hotbarCur < 0 || inv[kInvHotFirst + hotbarCur].id != ItGun)) { armaReloadT = -1.0f; armaShotQueued = false; }
}
void updateAccountNet(float dt) {
    if (!netGame) return;
    // token do Firebase: a fila do Auth também traz os tokens renovados durante o jogo
    authPollT -= dt;
    if (authPollT <= 0.0f) { authPollT = 0.5f; Auth::Fail f; auth.poll(f); }
    if (auth.signedIn() && auth.account().cloud) {
        if (auth.idToken() != loginTokenSent) configureLogin();
        tokenAskT -= dt;
        if ((auth.idToken().empty() || net.loginWaitingToken()) && tokenAskT <= 0.0f) { tokenAskT = 15.0f; auth.refreshToken(); }
    }
    for (NetClient::LoginInfo L; net.popLogin(L); ) {
        if (L.status == e0net::LoginOk || L.status == e0net::LoginGuest) {
            if (!selfDead) { health = std::max(1.0f, (float)L.health); hunger = (float)L.hunger; hungerWarnLow = hungerWarnZero = false; }
            FlagDesign fd;
            if (flagUnsynced) { uint8_t b[e0net::kFlagBytes]; flagToBytes(flagSaved, b); net.sendProfile(b); flagUnsynced = false; saveCfg(); }
            else if ((L.flags & e0net::LoginHasFlag) && flagFromBytes(L.flag, fd) && fd != flagSaved) { flagSaved = fd; if (!flagOpen) flagEdit = fd; saveCfg(); }
            else if (!(L.flags & e0net::LoginHasFlag)) { uint8_t b[e0net::kFlagBytes]; flagToBytes(flagSaved, b); net.sendProfile(b); }
            if (L.flags & e0net::LoginNewAccount) notify(L.status == e0net::LoginOk ? "CONTA CRIADA: PROGRESSO SALVO NO SERVIDOR" : "VISITANTE: PROGRESSO SALVO NO SERVIDOR");
            if (L.flags & e0net::LoginMigrated) notify("PROGRESSO DO VISITANTE PASSOU PARA A CONTA");
            for (auto& r : receipts) r.sent = false;   // recibos não confirmados: manda de novo (o servidor não credita 2x)
            LOG("conta conectada ao servidor (%s, flags %d)", L.status == e0net::LoginOk ? "verificada" : "visitante", L.flags);
        } else {
            notify("CONTA NAO VERIFICADA: PROGRESSO NAO SERA SALVO");
            LOG("login recusado pelo servidor (status %d)", L.status);
        }
    }
    if (net.kicked() && !kickedShown) { kickedShown = true; notify("SUA CONTA ENTROU EM OUTRO APARELHO"); }
    // inventário do servidor (não troca no meio de um arrasto)
    if (net.invAdoptable() && invDragPtr < 0) adoptServerInv();
    // vida/fome: são do cliente; o servidor guarda para a próxima sessão
    vitalsT -= dt;
    const int hp = (int)std::lround(std::max(0.0f, std::min(100.0f, health))), hu = (int)std::lround(std::max(0.0f, std::min(100.0f, hunger)));
    if (net.connected() && net.loginSettled() && vitalsT <= 0.0f && (std::abs(hp - vitalsSentHp) >= 2 || std::abs(hu - vitalsSentHu) >= 2)) {
        vitalsT = 5.0f; vitalsSentHp = hp; vitalsSentHu = hu; net.sendVitals((uint8_t)hp, (uint8_t)hu);
    }
    // recibos da Play -> servidor (credita 1 vez por token; o resultado volta em MsgAccountResult)
    static float rcptT = 0.0f; rcptT -= dt;
    if (net.connected() && net.loginSettled() && rcptT <= 0.0f) {
        rcptT = 3.0f;
        for (auto& r : receipts) if (!r.sent) { net.sendAccount(e0net::AccPurchase, (r.sku + "|" + r.token).c_str()); r.sent = true; break; }
    }
    for (NetClient::AccountRes a; net.popAccountRes(a); ) {
        for (auto& r : receipts) {
            const std::string v = r.sku + "|" + r.token;
            if (e0net::fnv1a32(v.data(), v.size()) != a.hash) continue;
            if (a.status == e0net::PurchCredited) {
                r.done = true; char m[64]; std::snprintf(m, sizeof m, "COMPRA CREDITADA: +%d MOEDAS", (int)a.coins);
                if (shopOpen) shopSay(m); else notify(m);
            } else if (a.status == e0net::PurchDuplicate) r.done = true;
            else if (a.status == e0net::PurchRejected) { r.done = true; if (shopOpen) shopSay("COMPRA RECUSADA PELO SERVIDOR"); else notify("COMPRA RECUSADA PELO SERVIDOR"); }
            else r.sent = false;   // sem conta ainda: tenta de novo
        }
        receipts.erase(std::remove_if(receipts.begin(), receipts.end(), [](const Receipt& r) { return r.done; }), receipts.end());
        saveWallet();
    }
}
bool kickedShown = false;
