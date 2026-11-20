// Isolated - cliente de rede: conexão UDP, predição local, reconciliação e interpolação de remotos.
#pragma once
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../shared/protocol.h"

// E0_DEBUG_UI = 1 (padrão, build de desenvolvimento): painel de debug, aba GOD e cheats/teleportes (mensagens MsgDebug*).
// E0_RELEASE=1 ./build.sh compila com E0_DEBUG_UI=0: nada disso entra no binário.
#ifndef E0_DEBUG_UI
#define E0_DEBUG_UI 1
#endif

namespace e0 {

class NetClient {
public:
    enum class State { Offline, Connecting, Connected };

    ~NetClient() { stop(); if (islandThread_.joinable()) islandThread_.join(); }
    void setCacheDir(const std::string& dir) { cacheDir_ = dir; }   // onde guardar o mundo baixado (antes de start)
    bool startOffline();                                            // sem servidor: gera o mundo local (semente padrão)

    // Mundo (terreno): baixado do servidor por TCP (com cache) ou gerado localmente (offline). nullptr até ficar pronto
    // (e em servidores --flat). worldGeneration() muda a cada mundo novo => quem desenha reconstrói a malha.
    e0net::World* worldMutable() { return (worldReady_ && !flat_) ? world_.get() : nullptr; }   // p/ remover colisão de objetos destruídos
    const e0net::World* world() const { return (worldReady_ && !flat_) ? world_.get() : nullptr; }
    bool worldReady() const { return worldReady_; }
    int worldGeneration() const { return worldGen_; }
    int worldPhase() const { return worldPhase_.load(); }   // 0 nada, 1 baixando, 2 do servidor, 3 do cache, 4 local, 5 servidor sem resposta
    const char* worldStatus() const;

    bool start(const char* ipv4, uint16_t port);  // false se endereço inválido (modo offline)
    void stop();                                  // envia Leave e fecha o socket

    // Chamar a cada frame: aplica o input localmente (predição) e o enfileira para envio.
    void step(e0net::InputCmd cmd);
    // Chamar a cada frame: recebe pacotes, envia inputs, controla timeouts.
    void tick();

    State state() const { return state_; }
    bool connected() const { return state_ == State::Connected; }
    int myId() const { return myId_; }
    const e0net::PlayerState& predicted() const { return me_; }
    // Estado para DESENHAR: predição + deslocamento visual que decai (esconde correções pequenas do servidor).
    e0net::PlayerState visual() const { e0net::PlayerState v = me_; v.x += offX_; v.y += offY_; v.z += offZ_; return v; }
    // Hora do ciclo dia/noite em segundos [0, kCycleSec): vem do servidor; offline segue o relógio local.
    float timeOfDay() const { return (float)std::fmod(clock_, (double)e0net::kCycleSec); }
    // Métricas para o painel de debug (atualizadas a cada 1 s, exceto rtt e contagens)
    struct Stats {
        float rttMs = -1;       // RTT médio da última janela de 1 s (-1 = sem medida)
        float interpMs = 100;   // atraso atual de interpolação dos remotos (adaptativo)
        float rttMin = 0, rttMax = 0, rttJit = 0;  // janela de 1 s: mín, máx, jitter (média |Δ| entre amostras)
        int snapPerSec = 0;     // snapshots aceitos por segundo
        float rxKBs = 0, txKBs = 0;
        float corrMax = 0;      // maior correção do servidor (m) na última janela de 1 s
        int players = 1;        // jogadores no último snapshot (incluindo eu): só os da ÁREA DE INTERESSE
    };
    const Stats& stats() const { return stats_; }
    size_t pendingCount() const { return pending_.size(); }
    const char* serverLabel() const { return label_; }
    // Debug: pede ao servidor para mudar a hora do mundo (só vale se ele foi iniciado com --debug). Offline: muda o relógio local.
#if E0_DEBUG_UI
    void requestTime(float cycleSec);
#endif
    // Debug: pede ao servidor (--debug) para teleportar o jogador; offline: move direto.
#if E0_DEBUG_UI
    void requestTeleport(float x, float z);
#endif
    void requestRespawn();   // renascer no ponto de nascimento (MsgRespawn)
    // Debug (GOD): faz nascer o navio na água perto de (x, z). Conectado: MsgDebugShip (só vale se o servidor está em --debug); offline: local.
#if E0_DEBUG_UI
    void requestSpawnShip(float x, float z, int kind);
#endif
    // CHAMAR NAVIO (tela NAVEGAÇÃO): pede o navio `kind` no mar à frente de mim. O resultado volta em popSummon (offline: resolvido na hora aqui mesmo).
    struct SummonRes { uint8_t kind; uint8_t result; uint16_t dist; };   // result = e0net::SummonResult; dist = m à frente (SumOk) ou s de espera (SumCooldown)
    void requestSummon(int kind);
    // s até a LIMPEZA dos navios sem ninguém (servidor ou local offline); < 0 = não há limpeza
    float shipCleanIn() const { return state_ == State::Connected ? shipCleanIn_ : (world() ? offClean_ : -1.0f); }
    bool popSummon(SummonRes& out) { if (summons_.empty()) return false; out = summons_.front(); summons_.pop_front(); return true; }
    // Debug (GOD): soma delta à vida do navio (-50 = dano; <= -1000 = afundar). Conectado: MsgDebugShipHp (só com --debug); offline: local.
#if E0_DEBUG_UI
    void requestShipHp(int ship, int delta);
#endif
    // Construção: coloca ou remove uma peça no mundo (envia ao servidor se conectado e aplica localmente).
    // MELHORIAS DA ILHA (shared/isle_build.h): op 1 = erguer (kind, rot, x, z), 2 = demolir (id). Devolve o reqId (0 = sem conexão).
    uint16_t requestStruct(uint8_t op, int slot, uint8_t kind, uint8_t rot, float x, float z, uint16_t id);
    struct StructRes { uint16_t req = 0; uint8_t op = 0, result = 0, kind = 0; };
    bool popStructRes(StructRes& e) { if (structRes_.empty()) return false; e = structRes_.front(); structRes_.pop_front(); return true; }
    // Coleta (gravetos/pedrinhas): pede ao servidor para pegar world.objects[idx]. Conectado: o servidor valida e responde
    // MsgCollectOk (vira um evento em popCollected) + bitset MsgTakenSync p/ todos. Offline: pega na hora.
    void requestCollect(uint32_t idx);
    // gid = (ilha << 20) | índice (ver terrain.h); ilha 0 = a inicial, então o gid dela é o próprio índice.
    bool isTaken(uint32_t gid) const { int sl = (int)(gid >> 20); uint32_t idx = gid & 0xFFFFFu; return sl >= 0 && sl < e0net::kIslandSlots && idx / 8 < taken_[sl].size() && (taken_[sl][idx / 8] >> (idx & 7)) & 1u; }
    // OCEANO ABERTO: ilhas geradas sob demanda perto do jogador (thread própria; mesma semente => idênticas às do servidor). island(0) = a inicial.
    // Cópia das ilhas (shared_ptr) para threads de leitura (mapa): chamar na thread principal, que é a única que troca/descarrega ilhas.
    std::shared_ptr<const e0net::TerrainSnapshot> terrainSnapshot() const {
        auto s = std::make_shared<e0net::TerrainSnapshot>();
        if (worldReady_ && !flat_) s->isl[0] = world_;
        for (int i = 1; i < e0net::kIslandSlots; ++i) s->isl[i] = islands_[i];
        return s;
    }
    const e0net::World* island(int slot) const { return slot == 0 ? world() : (slot > 0 && slot < e0net::kIslandSlots ? islands_[slot].get() : nullptr); }
    e0net::World* islandMutable(int slot) { return slot == 0 ? worldMutable() : (slot > 0 && slot < e0net::kIslandSlots ? islands_[slot].get() : nullptr); }
    bool popNewIsland(int& slot) { if (newIslands_.empty()) return false; slot = newIslands_.front(); newIslands_.pop_front(); return true; }
    bool popUnloadedIsland(int& slot) { if (unloadedIslands_.empty()) return false; slot = unloadedIslands_.front(); unloadedIslands_.pop_front(); return true; }
    const e0net::WorldObject* object(uint32_t gid) const { const e0net::World* w = island((int)(gid >> 20)); uint32_t i = gid & 0xFFFFFu; return w && i < w->objects.size() ? &w->objects[i] : nullptr; }
    uint32_t respawnGeneration() const { return respawnGen_; }       // muda quando recursos já pegos voltam (renovação da meia-noite)
    uint32_t takenGeneration() const { return takenGen_; }          // muda quando o conjunto de itens pegos muda
    bool popCollected(uint32_t& idx, uint8_t& type);                  // coletas confirmadas p/ ESTE jogador (-> inventário)
    // Inventário em rede: soltar no chão, pegar item solto e entregar ao jogador mais próximo (ações confiáveis: reenviadas até o Ack).
    // Resultados chegam em popActResult. Offline: o cliente é a autoridade (soltar/pegar funcionam; entregar não tem ninguém).
    struct DropInfo { uint16_t id; uint8_t item; uint16_t n; float x, y, z; };
    struct ActResult { uint8_t kind, result, item; uint16_t n; uint16_t other; uint16_t a, b; uint32_t rev = 0; };   // a,b = o que foi pedido; rev = revisão do inventário (servidor)
    void requestAct(uint8_t kind, uint16_t a, uint16_t b);
    bool popActResult(ActResult& out);
    size_t actBusy() const { return acts_.size(); }
    const std::vector<DropInfo>& drops() const { return drops_; }
    const e0net::Chest& chest(int ship) const { return chests_[ship < 0 || ship >= e0net::kMaxShips ? 0 : ship]; }   // baú do navio (o servidor manda; offline é local)
    struct Gift { uint16_t from; uint8_t item; uint16_t n; };
    bool popGift(Gift& out);
    // Dano de outros jogadores (servidor autoritativo): cada MsgDamage é confirmado (Ack) e entregue 1 vez; HitConfirm = o MEU golpe acertou `target`.
    struct Damage { uint8_t amount; uint16_t from; };
    struct HitConfirm { uint16_t target; uint8_t amount; };
    bool popDamage(Damage& out);
    bool popHitConfirm(HitConfirm& out);
    // Canhões do navio: requestCannon manda o disparo (alvo no mar/terra); fire = uma bala saiu (de qualquer jogador), hit = a bala chegou.
    struct CannonFire { uint16_t shooter; float sx, sy, sz, tx, ty, tz, flight; };
    struct CannonHit { float x, y, z; uint8_t kind; uint8_t ship = 255; int16_t hp = 0; uint16_t from = e0net::kNoPlayer; };   // ship != 255: a bala acertou o casco desse navio (vida hp)
    void requestCannon(float tx, float tz);
    bool popCannonFire(CannonFire& out) { if (cannonFires_.empty()) return false; out = cannonFires_.front(); cannonFires_.pop_front(); return true; }
    bool popCannonHit(CannonHit& out);
    // BACAMARTE: requestShoot manda o tiro (origem = boca do cano, dir = para o ponto da mira); o servidor decide os chumbos e devolve a todos
    // um ShotFx (onde cada chumbo parou e o que acertou). Offline: o próprio cliente traça os chumbos contra o relevo. Devolve o shotId.
    struct ShotFx { uint16_t shooter = 0; uint16_t shotId = 0; float ox = 0, oy = 0, oz = 0; int n = 0; struct P { float x, y, z; uint8_t kind; uint16_t target; } p[e0net::kGunPellets]; };
    uint16_t requestShoot(const float o[3], const float d[3], bool aimed);
    bool popShotFx(ShotFx& out) { if (shotFx_.empty()) return false; out = shotFx_.front(); shotFx_.pop_front(); return true; }
    // CHAT: sendChat manda o texto (o servidor limpa, limita 1/s e devolve a quem está perto, inclusive a mim); popChat entrega as mensagens recebidas.
    // Offline: o texto volta na hora, como se eu mesmo tivesse falado.
    struct Chat { uint16_t from = 0; char text[e0net::kChatMaxLen + 1] = {}; };
    void sendChat(const char* text);
    // CONTA / RECIBO para o servidor (MsgAccount; 2 cópias, sem confirmação): ponto de extensão p/ a validação no servidor.
    void sendAccount(uint8_t kind, const char* data);
    bool popChat(Chat& out) { if (chats_.empty()) return false; out = chats_.front(); chats_.pop_front(); return true; }
    // ---- TRIPULAÇÃO no jogo (shared/team.h): bandeira de cada jogador (para a TAG) e convites ----
    struct TeamInfo { bool known = false; uint32_t team = 0; uint8_t flag[e0net::kFlagBytes] = {}; double at = -1e9; };
    const TeamInfo& teamInfo(int id) const { static TeamInfo none; return id >= 0 && id < e0net::kMaxPlayers ? teamInfo_[id] : none; }
    void queryTeams(const uint16_t* ids, int n);   // pede ao servidor (MsgTeamQuery); a resposta atualiza teamInfo
    void forgetTeam(int id) { if (id >= 0 && id < e0net::kMaxPlayers) teamInfo_[id].at = -1e9; }
    void inviteTeam(uint16_t target);              // MsgTeamInvite (3 cópias)
    struct TeamOffer { uint32_t id = 0; uint16_t from = 0; uint8_t members = 0; uint8_t flag[e0net::kFlagBytes] = {}; std::string name; };
    bool popTeamOffer(TeamOffer& o) { if (teamOffers_.empty()) return false; o = teamOffers_.front(); teamOffers_.pop_front(); return true; }
    void answerTeamOffer(uint32_t offerId, bool accept);   // MsgTeamOfferReply (3 cópias)
    struct TeamPos { uint16_t id; float x, z; };
    const std::vector<TeamPos>& teamPositions() const { return teamPos_; }   // outros tripulantes online (qualquer distância), 1x/s
    double teamPosAge() const;                                                // s desde a última (vazio/velho = ninguém online)
    struct TeamChatPush { bool system = false; std::string author, text; };
    bool popTeamChat(TeamChatPush& m) { if (teamChat_.empty()) return false; m = teamChat_.front(); teamChat_.pop_front(); return true; }
    // ---- DOMINAÇÃO DE ILHAS (shared/conquest.h) ----
    struct IsleOwner { uint32_t team = 0; uint8_t flag[e0net::kFlagBytes] = {}; };
    const IsleOwner& isleOwner(int slot) const { static IsleOwner none; return slot > 0 && slot < e0net::kIslandSlots ? isleOwner_[slot] : none; }
    struct IsleCap { uint32_t owner = 0, pole = 0; float prog = 0; uint8_t state = 0, nAtt = 0, nDef = 0; int shield = 0; uint8_t ownerFlag[e0net::kFlagBytes] = {}, poleFlag[e0net::kFlagBytes] = {}; std::string ownerName, poleName; double at = -1e9; };
    const IsleCap& isleCap(int slot) const { static IsleCap none; return slot > 0 && slot < e0net::kIslandSlots ? isleCap_[slot] : none; }
    double isleCapAge(int slot) const;   // s desde o último MsgIslandCap dessa ilha
    struct IsleEvent { uint8_t kind = 0, slot = 0; int32_t value = 0; std::string name; };
    bool popIsleEvent(IsleEvent& e) { if (isleEvents_.empty()) return false; e = isleEvents_.front(); isleEvents_.pop_front(); return true; }
    struct TeamNotice { uint8_t kind = 0, result = 0; uint16_t other = 0; std::string name; };
    bool popTeamNotice(TeamNotice& n) { if (teamNotices_.empty()) return false; n = teamNotices_.front(); teamNotices_.pop_front(); return true; }
    // ---- CONTA E INVENTÁRIO DO SERVIDOR (v42; ver CLAUDE.md "PERSISTÊNCIA NO SERVIDOR") ----
    // setLogin: quem sou (LoginFirebase + idToken, ou LoginLocal + uid "local-..."). Mandado depois de cada Welcome e reenviado até a resposta.
    void setLogin(uint8_t kind, const std::string& uid, const std::string& token, const std::string& prevLocal);
    struct LoginInfo { uint8_t status = 0, flags = 0, health = 100, hunger = 100, skin = 0; uint8_t flag[e0net::kFlagBytes] = {}; };
    bool popLogin(LoginInfo& out) { if (logins_.empty()) return false; out = logins_.front(); logins_.pop_front(); return true; }
    bool loginSettled() const { return loginSettled_; }   // o servidor já respondeu (ou não há conta configurada)
    bool loginWaitingToken() const { return loginNeedToken_; }   // token expirado: o motor pede outro e chama setLogin
    bool kicked() const { return kicked_; }                // a mesma conta entrou em outro aparelho (não reconecta sozinho)
    // Inventário + moedas AUTORITATIVOS: o servidor manda MsgInvSync a cada mudança. O cliente prevê localmente e ADOTA o do servidor quando
    // não há ação em voo (assim a previsão nunca acumula erro).
    bool serverInv() const { return state_ == State::Connected && invHave_; }
    uint8_t shipsOwned() const { return serverInv() ? (uint8_t)((ships_ | (ships_ >> 4)) & 15u) : 15; }   // meus + da tripulação
    uint8_t shipsCrewOnly() const { return serverInv() ? (uint8_t)(((ships_ >> 4) & ~ships_) & 15u) : 0; }   // da tripulação e não comprados por mim: chamar sem espera   // bit k = a conta é dona do navio k (a conta nova começa com 0; offline/sem servidor: todos)
    bool invAdoptable() const { return serverInv() && loginSettled_ && (invNeed_ || invRev_ != invAdopted_) && acts_.empty() && invRev_ >= ackRev_; }
    void invAdopt(e0net::InvSlot* out, int32_t& coins) { std::memcpy(out, inv_, sizeof inv_); coins = coins_; invAdopted_ = invRev_; invNeed_ = false; }
    int32_t serverCoins() const { return coins_; }
    void sendVitals(uint8_t health, uint8_t hunger);
    void sendProfile(const uint8_t* flag);
    struct AccountRes { uint8_t kind = 0, status = 0; int32_t coins = 0; uint32_t hash = 0; };
    bool popAccountRes(AccountRes& out) { if (accRes_.empty()) return false; out = accRes_.front(); accRes_.pop_front(); return true; }
#if E0_DEBUG_UI
    void requestDebugCoins(int delta);
    void requestDebugItems(int item, int n);
#endif
    void setSkin(int s) { skin_ = (uint8_t)(s >= 0 && s < e0net::kSkinCount ? s : 0); me_.skin = skin_; }   // antes de conectar (vai no Join)
    int skin() const { return skin_; }
    float renderTick() const;   // tick do servidor em que os outros jogadores estão sendo desenhados agora (interpolação)
    enum class TimeReq { None, Pending, Ok, Refused };
    TimeReq timeReq() const { return timeReq_; }
    // Jogadores remotos interpolados (atrasados kInterpDelay) para desenho.
    void remotes(std::vector<e0net::PlayerState>& out) const;
    void remotesRaw(std::vector<e0net::PlayerState>& out) const;   // interpolados, sem posicionar quem está a bordo

private:
    struct Snap { double t; uint32_t tick; std::vector<e0net::PlayerState> players; };
    void send(const uint8_t* d, size_t n);
    void onPacket(const uint8_t* d, size_t n, double arrival);  // arrival = instante real de chegada (timestamp do kernel)

    int fd_ = -1;
    State state_ = State::Offline;
    unsigned long serverIp_ = 0;  // sockaddr_in.sin_addr.s_addr
    unsigned short serverPort_ = 0;  // network order
    int myId_ = -1;
    e0net::PlayerState me_;
    uint32_t nextSeq_ = 1;
    std::deque<e0net::InputCmd> pending_;
    std::deque<Snap> snaps_;
    Stats stats_;
    char label_[40] = "-";
    double rttSum_ = 0, rttMinW_ = 1e9, rttMaxW_ = 0, rttJitSum_ = 0, rttLast_ = -1; int rttN_ = 0, rttJitN_ = 0;
    double statT_ = 0; int snapCnt_ = 0; unsigned long rxB_ = 0, txB_ = 0; float corrWin_ = 0;
    TimeReq timeReq_ = TimeReq::None; float timeReqTarget_ = 0; double timeReqAt_ = 0;
    void startWorldThread(bool local);
    void worldThreadMain(bool local);
    void publishWorld(std::shared_ptr<e0net::World> w, int phase);
    std::string cacheDir_, host_;
    uint16_t port_ = 0;
    std::thread worldThread_;
    std::atomic<bool> stopWorld_{false}, pendingReady_{false}, fetching_{false};
    std::atomic<int> worldPhase_{0};  // 0 nada, 1 baixando, 2 do servidor, 3 do cache, 4 local, 5 aguardando servidor
    std::mutex wm_;
    std::shared_ptr<e0net::World> pendingWorld_, world_;
    bool worldReady_ = false, flat_ = false;
    int worldGen_ = 0;
    uint32_t lastTick_ = 0;
    std::vector<uint8_t> taken_[e0net::kIslandSlots];           // por ilha: bitset dos objetos já pegos/derrubados (servidor ou local)
    std::shared_ptr<e0net::World> islands_[e0net::kIslandSlots];  // ilhas do oceano aberto já geradas (slot 0 não usado: a inicial é world_)
    std::deque<int> newIslands_, unloadedIslands_;                // ilhas prontas ou descarregadas por distância (4 GB RAM)
    std::thread islandThread_; std::atomic<bool> islandBusy_{false}; std::mutex im_;
    std::vector<std::pair<int, std::shared_ptr<e0net::World>>> islandReady_;
    bool islandWanted_[e0net::kIslandSlots] = {};
    void updateIslands();
    uint32_t takenGen_ = 0, respawnGen_ = 0;
    std::deque<std::pair<uint32_t, uint8_t>> collected_;        // (índice, tipo) confirmados para mim
    struct PendingAct { uint32_t seq; uint8_t kind; uint16_t a, b; double sentAt, firstAt; };
    std::deque<PendingAct> acts_;                               // em ordem; só a primeira é (re)enviada
    uint32_t actSeq_ = 0;
    std::deque<ActResult> actResults_;
    std::vector<DropInfo> drops_;
    e0net::Chest chests_[e0net::kMaxShips];
    uint16_t localDropId_ = 1;
    std::deque<Gift> gifts_;
    uint32_t giftSeen_[16] = {}; int giftSeenN_ = 0;
    std::deque<CannonFire> cannonFires_; std::deque<CannonHit> cannonHits_;
    struct LocalShell { double at; CannonHit hit; }; std::vector<LocalShell> localShells_;   // offline: a bala pendente
    uint8_t cannonShot_ = 0;
    uint16_t gunShot_ = 1; std::deque<ShotFx> shotFx_;
    uint32_t joinCookie_ = 0;   // cookie do handshake (MsgChallenge)
    // conta / inventário do servidor
    uint8_t loginKind_ = 0, loginReq_ = 0; std::string loginUid_, loginToken_, loginPrev_; bool loginSettled_ = true, loginNeedToken_ = false, kicked_ = false; double loginSentAt_ = 0, loginRetryAt_ = 0;
    std::deque<LoginInfo> logins_;
    uint8_t ships_ = 0;
    bool invHave_ = false, invNeed_ = false; uint32_t invRev_ = 0, invAdopted_ = 0, ackRev_ = 0; int32_t coins_ = 0; e0net::InvSlot inv_[e0net::kInvSlots];
    std::deque<AccountRes> accRes_;
    void sendLogin();
    uint8_t skin_ = 0; uint8_t summonId_ = 0; int summonSeen_ = -1; std::deque<SummonRes> summons_; double offSummonAt_ = -100.0;
    std::vector<TeamPos> teamPos_; double teamPosAt_ = -1e9;
    std::deque<TeamChatPush> teamChat_;
    uint16_t structReq_ = 0, lastStructRes_ = 0; std::deque<StructRes> structRes_;
    IsleOwner isleOwner_[e0net::kIslandSlots]; IsleCap isleCap_[e0net::kIslandSlots]; std::deque<IsleEvent> isleEvents_;
    TeamInfo teamInfo_[e0net::kMaxPlayers];
    std::deque<TeamOffer> teamOffers_; uint32_t lastOffer_ = 0;
    std::deque<TeamNotice> teamNotices_; uint32_t inviteId_ = 0; uint8_t lastNoticeKey_[8] = {}; double lastNoticeAt_ = -1;
    uint8_t chatId_ = 0; std::deque<Chat> chats_; struct ChatResend { double at; uint8_t pkt[128]; size_t len; }; std::vector<ChatResend> chatResend_;
    struct ShotResend { double at; uint8_t pkt[48]; size_t len; }; std::vector<ShotResend> shotResend_;
    float offRespawn_[e0net::kMaxShips] = {};                   // offline: navio afundado renasce em tantos s
    float shipCleanIn_ = -1.0f;                                 // online: s até a LIMPEZA dos navios (do snapshot; -1 = sem limpeza)
    float offClean_ = e0net::kShipCleanupSec;                   // offline: o mesmo contador, local
    std::deque<Damage> damages_; std::deque<HitConfirm> hitConfirms_;
    uint32_t dmgSeen_[16] = {}; int dmgSeenN_ = 0;
    std::vector<uint8_t> mine_[e0net::kIslandSlots];            // por ilha: bitset dos que EU já recebi (ignora OK repetido)
    double clock_ = e0net::kStartTimeSec;  // hora do ciclo; avança todo frame e é corrigida por snapshots
    float offX_ = 0, offY_ = 0, offZ_ = 0;  // erro visual remanescente
    double lastTickT_ = 0;
    double lastJoin_ = -1, lastSend_ = 0, lastRecv_ = 0;
    double interpDelay_ = 0.1, snapJitter_ = 0.01, lastSnapArr_ = 0;  // jitter buffer adaptativo
};

}  // namespace e0
