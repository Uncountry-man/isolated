#include "net_client.h"
#include "../../shared/team.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <android/log.h>

#include "../../shared/world_sync.h"

#include <time.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated.net", __VA_ARGS__)

using namespace e0net;

namespace e0 {

static uint32_t nowMs32();
static double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t nowMs32() { return (uint32_t)((uint64_t)(nowSec() * 1000.0)); }

bool NetClient::start(const char* ipv4, uint16_t port) {
    stop();
    in_addr ia{};
    if (!ipv4 || !*ipv4 || inet_pton(AF_INET, ipv4, &ia) != 1) { LOG("sem servidor: modo offline"); return false; }
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    int tos = 0xB8;  // DSCP EF: o Wi-Fi (WMM) põe o pacote na fila de voz = menor espera/jitter
    setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    int rcvBuf = 512 * 1024;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof rcvBuf);
    int on = 1;      // timestamp de chegada do kernel: tira o ruído de "só leio no próximo frame"
    setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on);
    serverIp_ = ia.s_addr;
    serverPort_ = htons(port);
    state_ = State::Connecting; kicked_ = false; invHave_ = false;
    lastJoin_ = -1;
    lastRecv_ = nowSec();
    std::snprintf(label_, sizeof label_, "%s:%d", ipv4, port);
    host_ = ipv4; port_ = port;
    startWorldThread(false);
    LOG("conectando em %s:%d", ipv4, port);
    return true;
}

void NetClient::stop() {
    stopWorld_ = true;
    if (worldThread_.joinable()) worldThread_.join();
    stopWorld_ = false;
    if (fd_ >= 0) {
        if (state_ == State::Connected) {
            uint8_t b[8]; Writer w(b, sizeof b); w.header(MsgLeave); send(b, w.size());
        }
        close(fd_);
    }
    fd_ = -1;
    state_ = State::Offline; invHave_ = false;
    myId_ = -1;
    pending_.clear();
    snaps_.clear();
}

void NetClient::send(const uint8_t* d, size_t n) {
    if (fd_ < 0) return;
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = serverIp_; a.sin_port = serverPort_;
    sendto(fd_, d, n, 0, (const sockaddr*)&a, sizeof a);
    txB_ += n;
}

// ---------------------------------------------------------------------------------------------
// Mundo: download por TCP (com cache em disco) numa thread; o resultado é publicado no tick() (thread principal)
// ---------------------------------------------------------------------------------------------
const char* NetClient::worldStatus() const {
    static const char* n[6] = {"SEM MUNDO", "BAIXANDO...", "BAIXADO DO SERVIDOR", "CACHE LOCAL", "GERADO LOCAL (OFFLINE)", "SERVIDOR SEM RESPOSTA"};
    if (flat_) return "SERVIDOR PLANO (--FLAT)";
    int p = worldPhase_.load();
    return n[p < 0 || p > 5 ? 0 : p];
}

void NetClient::publishWorld(std::shared_ptr<World> w, int phase) {
    std::lock_guard<std::mutex> l(wm_);
    pendingWorld_ = std::move(w);
    worldPhase_ = phase;
    pendingReady_ = true;
}

void NetClient::startWorldThread(bool local) {
    if (worldThread_.joinable()) { stopWorld_ = true; worldThread_.join(); stopWorld_ = false; }
    fetching_ = true;
    worldThread_ = std::thread([this, local] { worldThreadMain(local); fetching_ = false; });
}

void NetClient::worldThreadMain(bool local) {
    if (local) {
        auto w = std::make_shared<World>();
        if (World::generate(kDefaultWorldSeed, *w)) { LOG("mundo local gerado: semente %u, hash %08x", w->seed, w->hash); publishWorld(w, 4); }
        return;
    }
    while (!stopWorld_) {
        std::shared_ptr<World> cached;
        uint32_t cachedHash = 0;
        if (!cacheDir_.empty()) {
            auto cw = std::make_shared<World>();
            if (World::load(cacheDir_ + "/world.e0w", *cw)) { cached = cw; cachedHash = cw->hash; }
        }
        worldPhase_ = 1;
        std::vector<uint8_t> payload; uint32_t sh = 0;
        if (fetchWorld(host_.c_str(), port_, cachedHash, payload, sh, 2500)) {
            if (payload.empty()) {  // servidor: "seu cache está atual"
                if (cached && cached->hash == sh) { LOG("mundo do cache (hash %08x)", sh); publishWorld(cached, 3); return; }
            } else {
                auto w = std::make_shared<World>();
                if (World::fromBytes(payload.data(), payload.size(), *w) && w->hash == sh) {
                    if (!cacheDir_.empty()) w->save(cacheDir_ + "/world.e0w");
                    LOG("mundo baixado: semente %u, hash %08x, %.0f KB", w->seed, w->hash, payload.size() / 1024.0);
                    publishWorld(w, 2);
                    return;
                }
                LOG("mundo baixado inválido (CRC/versão); tentando de novo");
            }
        }
        worldPhase_ = 5;
        for (int i = 0; i < 15 && !stopWorld_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool NetClient::startOffline() {
    stop();
    state_ = State::Offline;
    std::snprintf(label_, sizeof label_, "%s", "(offline)");
    startWorldThread(true);
    return true;
}

#if E0_DEBUG_UI   // só no build de desenvolvimento (release: nem o código nem a mensagem MsgDebug* existem)
void NetClient::requestTeleport(float x, float z) {
    if (state_ != State::Connected) {
        shipDetachPlayer(me_);
        me_.x = x; me_.z = z; me_.y = world_ && !flat_ ? std::max(terrain().heightAt(x, z), World::kSwimFloorH) : 0.0f; me_.vy = 0;
        return;
    }
    uint8_t b[16]; Writer w(b, sizeof b);
    w.header(MsgDebugTeleport); w.put<float>(x); w.put<float>(z);
    send(b, w.size());
}
#endif

void NetClient::requestRespawn() {
    if (state_ != State::Connected) {   // offline: volta ao ponto de nascimento da ilha local
        float x = world_ && !flat_ ? world_->spawnX() : 0.0f, z = world_ && !flat_ ? world_->spawnZ() : 0.0f;
        shipDetachPlayer(me_);
        me_.x = x; me_.z = z; me_.y = world_ && !flat_ ? std::max(terrain().heightAt(x, z), World::kSwimFloorH) : 0.0f; me_.vy = 0;
        return;
    }
    uint8_t b[16]; Writer w(b, sizeof b);
    w.header(MsgRespawn);
    send(b, w.size());
}

#if E0_DEBUG_UI   // só no build de desenvolvimento (release: nem o código nem a mensagem MsgDebug* existem)
void NetClient::requestSpawnShip(float x, float z, int kind) {
    if (state_ == State::Connected) {
        uint8_t b[20]; Writer w(b, sizeof b);
        w.header(MsgDebugShip); w.put<float>(x); w.put<float>(z); w.put<uint8_t>((uint8_t)kind);
        send(b, w.size());
        return;
    }
    const World* W = world();
    float sx, sz, sy;
    if (!W || !shipFindSpawn(kind, *W, x, z, 14.0f, sx, sz, sy)) return;
    ShipState& S = shipState(kind);
    S.exists = true; S.x = sx; S.z = sz; S.yaw = sy; S.speed = 0; S.steer = S.cmdSteer = 0; S.cmdThrottle = 0; S.helmer = -1; ++S.gen;
    S.hp = (int16_t)shipDef(kind).maxHp; S.sinkT = 0.0f; offRespawn_[kind] = 0.0f;
}
#endif

void NetClient::requestSummon(int kind) {
    if (kind < 0 || kind >= kMaxShips) return;
    if (state_ == State::Connected) {
        ChatResend s; Writer w(s.pkt, sizeof s.pkt);
        w.header(MsgSummonShip); w.put<uint8_t>(++summonId_); w.put<uint8_t>((uint8_t)kind);
        s.len = w.size();
        send(s.pkt, s.len);
        s.at = nowSec() + 0.08; chatResend_.push_back(s);   // cópias contra perda de pacote (o servidor responde igual ao mesmo id)
        s.at = nowSec() + 0.25; chatResend_.push_back(s);
        return;
    }
    // offline: o cliente é a autoridade (mesmas regras do servidor)
    const World* W = world();
    ShipState& S = shipState(kind);
    auto res = [&](uint8_t r, uint16_t d) { if (summons_.size() < 16) summons_.push_back({(uint8_t)kind, r, d}); };
    if (!W) { res(SumNoShip, 0); return; }
    if (me_.ship) { res(SumAboard, 0); return; }
    if (S.exists && !shipAfloat(S)) { res(SumSinking, 0); return; }
    // não existe: o chamado cria o navio à frente (como no servidor)
    const double t = nowSec();
    if (t < offSummonAt_ + kSummonCooldown) { res(SumCooldown, (uint16_t)std::ceil(offSummonAt_ + kSummonCooldown - t)); return; }
    float sx, sz, sy;
    if (!shipFindAhead(kind, *W, me_.x, me_.z, me_.yaw, sx, sz, sy)) { res(SumNoWater, 0); return; }
    S.exists = true; S.x = sx; S.z = sz; S.yaw = sy; S.speed = 0; S.steer = S.cmdSteer = 0; S.cmdThrottle = 0; S.helmer = -1; ++S.gen;
    S.hp = (int16_t)shipDef(kind).maxHp; S.sinkT = 0.0f; offRespawn_[kind] = 0.0f;
    offSummonAt_ = t;
    res(SumOk, (uint16_t)std::hypot(sx - me_.x, sz - me_.z));
}

#if E0_DEBUG_UI   // só no build de desenvolvimento (release: nem o código nem a mensagem MsgDebug* existem)
void NetClient::requestShipHp(int ship, int delta) {
    if (ship < 0 || ship >= kMaxShips) return;
    if (state_ == State::Connected) {
        uint8_t b[16]; Writer w(b, sizeof b);
        w.header(MsgDebugShipHp); w.put<uint8_t>((uint8_t)ship); w.put<int16_t>((int16_t)std::max(-32000, std::min(32000, delta)));
        send(b, w.size());
        return;
    }
    ShipState& S = shipState(ship);   // offline: o cliente é a autoridade
    if (!shipAfloat(S)) return;
    S.hp = (int16_t)std::max(0, std::min(shipDef(ship).maxHp, delta <= -1000 ? 0 : (int)S.hp + delta));
    if (S.hp <= 0) { S.sinkT = 1e-3f; S.helmer = -1; S.cmdSteer = 0; S.cmdThrottle = 0; chests_[ship] = Chest{}; }   // quem está a bordo cai na água em stepAboard
}
#endif

// MELHORIAS DA ILHA: pedido ao servidor (3 cópias; o servidor responde 1x por reqId)
uint16_t NetClient::requestStruct(uint8_t op, int slot, uint8_t kind, uint8_t rot, float x, float z, uint16_t id) {
    if (state_ != State::Connected) return 0;
    const uint16_t req = ++structReq_ ? structReq_ : ++structReq_;
    ChatResend s; Writer w(s.pkt, sizeof s.pkt);
    w.header(MsgStructReq); w.put<uint16_t>(req); w.put<uint8_t>(op); w.put<uint8_t>((uint8_t)slot); w.put<uint8_t>(kind); w.put<uint8_t>(rot);
    w.put<float>(x); w.put<float>(z); w.put<uint16_t>(id);
    s.len = w.size(); send(s.pkt, s.len);
    const double now = nowSec();
    s.at = now + 0.15; chatResend_.push_back(s);
    s.at = now + 0.45; chatResend_.push_back(s);
    return req;
}

void NetClient::requestCollect(uint32_t idx) {
    if (state_ == State::Connected) {
        uint8_t b[16]; Writer w(b, sizeof b);
        w.header(MsgCollect); w.put<uint32_t>(idx);
        send(b, w.size());
        return;
    }
    // offline: o próprio cliente é a autoridade
    const World* W = island((int)(idx >> 20));
    const uint32_t li = idx & 0xFFFFFu, sl = idx >> 20;
    if (!W || sl >= (uint32_t)kIslandSlots || li >= W->objects.size() || !World::isCollectible(W->objects[li].type) || isTaken(idx)) return;
    if (taken_[sl].size() <= li / 8) taken_[sl].resize(li / 8 + 1, 0);
    taken_[sl][li / 8] |= (uint8_t)(1u << (li & 7));
    ++takenGen_;
    collected_.push_back({idx, W->objects[li].type});
}

bool NetClient::popCollected(uint32_t& idx, uint8_t& type) {
    if (collected_.empty()) return false;
    idx = collected_.front().first; type = collected_.front().second;
    collected_.pop_front();
    return true;
}

void NetClient::requestAct(uint8_t kind, uint16_t a, uint16_t b) {
    if (state_ != State::Connected) {   // offline: autoridade local
        ActResult r{kind, 0, (uint8_t)a, b, kNoPlayer, a, b};
        if (kind == ActDrop && a && b) {
            if (drops_.size() >= (size_t)kMaxDrops) drops_.erase(drops_.begin());
            float x = me_.x + std::sin(me_.yaw) * kDropForward, z = me_.z - std::cos(me_.yaw) * kDropForward;
            float y = me_.y;
            if (world()) { terrain().pushOut(x, z, 0.15f); y = terrain().heightAt(x, z); if (std::fabs(y - me_.y) > 1.0f) y = me_.y; }
            drops_.push_back({localDropId_++, (uint8_t)a, b, x, y, z});
            r.result = 1;
        } else if (kind == ActChestPut || kind == ActChestTake) {
            uint8_t item = 0; uint16_t n = 0;
            r.result = chestAct(chests_, me_, kind, a, b, item, n); r.item = item; r.n = n;
        } else if (kind == ActPickupDrop) {
            for (size_t i = 0; i < drops_.size(); ++i) if (drops_[i].id == a) {
                r.result = 1; r.item = drops_[i].item; r.n = drops_[i].n; drops_.erase(drops_.begin() + (long)i); break;
            }
        }
        actResults_.push_back(r);
        return;
    }
    double t = nowSec();
    acts_.push_back({++actSeq_, kind, a, b, 0, t});
}

bool NetClient::popActResult(ActResult& out) {
    if (actResults_.empty()) return false;
    out = actResults_.front(); actResults_.pop_front();
    return true;
}

void NetClient::requestCannon(float tx, float tz) {
    if (state_ == State::Connected) {
        uint8_t b[24]; Writer w(b, sizeof b);
        w.header(MsgCannon); w.put<uint8_t>(++cannonShot_); w.put<float>(tx); w.put<float>(tz);
        for (int i = 0; i < 3; ++i) send(b, w.size());   // redundância contra perda (o servidor ignora cópias do mesmo shotId)
        return;
    }
    const ShipState& S = shipOf(me_);   // offline: o cliente é a autoridade (só visual)
    const ShipDef& D = shipDef(S.kind);
    if (!S.exists || me_.ship != 1 || !D.hasCannons) return;
    float dx = tx - S.x, dz = tz - S.z, d = std::sqrt(dx * dx + dz * dz);
    if (d < 0.01f) return;
    float dd = std::max(D.rangeMin, std::min(D.rangeMax, d)); tx = S.x + dx / d * dd; tz = S.z + dz / d * dd; d = dd;
    float a, b; shipWorldToLocal(S, tx, tz, a, b);
    float sx, sz; shipLocalToWorld(S, std::max(-D.muzzleA, std::min(D.muzzleA, a * 0.1f)), (b >= 0 ? 1.0f : -1.0f) * D.muzzleB, sx, sz);
    const Terrain* W = terrainPtr();
    float ty = W ? std::max(0.0f, W->heightAt(tx, tz)) : 0.0f;
    float fl = cannonFlightTime(d);
    cannonFires_.push_back({(uint16_t)std::max(0, myId_), sx, D.deckY + 1.1f, sz, tx, ty, tz, fl});
    localShells_.push_back({nowSec() + fl, {tx, ty, tz, (uint8_t)(ty > 0.3f ? 1 : 0)}});
}

bool NetClient::popCannonHit(CannonHit& out) {
    if (!localShells_.empty()) {
        double t = nowSec();
        for (size_t i = 0; i < localShells_.size();) { if (t >= localShells_[i].at) { cannonHits_.push_back(localShells_[i].hit); localShells_.erase(localShells_.begin() + (long)i); } else ++i; }
    }
    if (cannonHits_.empty()) return false;
    out = cannonHits_.front(); cannonHits_.pop_front();
    return true;
}

bool NetClient::popDamage(Damage& out) { if (damages_.empty()) return false; out = damages_.front(); damages_.pop_front(); return true; }
bool NetClient::popHitConfirm(HitConfirm& out) { if (hitConfirms_.empty()) return false; out = hitConfirms_.front(); hitConfirms_.pop_front(); return true; }
bool NetClient::popGift(Gift& out) {
    if (gifts_.empty()) return false;
    out = gifts_.front(); gifts_.pop_front();
    return true;
}

#if E0_DEBUG_UI   // só no build de desenvolvimento (release: nem o código nem a mensagem MsgDebug* existem)
void NetClient::requestTime(float cycleSec) {
    cycleSec = std::fmod(std::fmod(cycleSec, kCycleSec) + kCycleSec, kCycleSec);
    if (state_ != State::Connected) { clock_ = cycleSec; timeReq_ = TimeReq::Ok; return; }  // offline: relógio local
    uint8_t b[16]; Writer w(b, sizeof b);
    w.header(MsgDebugTime); w.put<float>(cycleSec);
    send(b, w.size());
    timeReq_ = TimeReq::Pending; timeReqTarget_ = cycleSec; timeReqAt_ = nowSec();
}
#endif

void NetClient::step(InputCmd cmd) {
    cmd.seq = nextSeq_++;
    for (int i = 0; i < kMaxShips; ++i) stepShip(shipState(i), cmd.dt, terrainPtr());   // os navios andam todo frame (predição; os snapshots o corrigem); quem está a bordo vai junto
    if (state_ != State::Connected) for (int i = 0; i < kMaxShips; ++i) {   // offline: o naufrágio termina e o navio renasce aqui (online quem decide é o servidor)
        ShipState& S = shipState(i);
        if (S.exists && S.sinkT >= kShipSinkTime) { if (me_.ship && me_.shipIdx == i) shipEject(me_); S.exists = false; S.sinkT = 0.0f; }   // afundou: só volta por chamado
    }
    if (state_ != State::Connected && world()) {   // offline: a mesma LIMPEZA do servidor (só eu posso estar usando)
        offClean_ -= cmd.dt;
        if (offClean_ <= 0.0f) {
            offClean_ += kShipCleanupSec;
            for (int i = 0; i < kMaxShips; ++i) { ShipState& S = shipState(i); if (shipAfloat(S) && !shipInUseBy(i, me_)) { S.exists = false; S.speed = 0.0f; S.helmer = -1; ++S.gen; chests_[i] = Chest{}; } }
        }
    }
    if (me_.ship) shipCarry(me_);
    stepPlayer(me_, cmd);  // predição: mesmo código do servidor
    if (state_ == State::Connected) {
        pending_.push_back(cmd);
        if (pending_.size() > 64) pending_.pop_front();
    }
}

void NetClient::onPacket(const uint8_t* d, size_t n, double now) {  // `now` = instante de chegada
    Reader r(d, n);
    uint8_t type = r.header();
    if (!type) return;
    lastRecv_ = now;

    if (type == MsgWelcome) {
        int id = r.get<uint16_t>();
        float x = r.get<float>(), y = r.get<float>(), z = r.get<float>();
        uint32_t wh = r.get<uint32_t>();
        if (!r.ok() || state_ == State::Connected) return;
        for (auto& p : acts_) p.sentAt = 0;   // reconectou: reenvia já (seq continua crescendo; o servidor aceita qualquer seq maior)
        for (int sl = 1; sl < kIslandSlots; ++sl) if (!isleStructs(sl).empty()) { isleStructs(sl).clear(); ++isleStructGen(sl); }   // o servidor manda as estruturas em seguida
        if (wh == 0) {                                   // servidor sem terreno (--flat): chão plano
            flat_ = true; worldReady_ = true; setTerrain(nullptr);
        } else if (!worldReady_ || flat_ || !world_ || world_->hash != wh) {
            // Nosso mundo não é o do servidor (ainda baixando, ou o servidor trocou de mundo): baixa e tenta entrar de novo.
            if (!fetching_) { LOG("mundo do servidor (%08x) difere do local: baixando", wh); worldReady_ = false; flat_ = false; startWorldThread(false); }
            return;
        }
        myId_ = id;
        invHave_ = false; ackRev_ = 0; invRev_ = 0; invAdopted_ = 0;   // sessão nova no servidor: o inventário dele chega no MsgInvSync
        if (loginKind_) { loginSettled_ = false; loginSentAt_ = 0; }   // conta: entra de novo a cada sessão (MsgLogin reenviado até a resposta)
        me_ = PlayerState{}; me_.id = (uint16_t)id; me_.x = x; me_.y = y; me_.z = z;
        for (int i = 0; i < kMaxShips; ++i) { shipState(i) = ShipState{}; chests_[i] = Chest{}; }   // os navios e os baús vêm do servidor
        state_ = State::Connected;
        pending_.clear(); snaps_.clear(); lastTick_ = 0;
        for (TeamInfo& t : teamInfo_) t = TeamInfo{}; teamOffers_.clear(); teamNotices_.clear(); teamPos_.clear(); teamChat_.clear();
        LOG("conectado: jogador %d", id);
    } else if (type == MsgChallenge) {   // handshake: repete o Join com o cookie (prova que este endereço é nosso)
        const uint32_t ck = r.get<uint32_t>();
        if (r.ok() && state_ == State::Connecting) { joinCookie_ = ck; uint8_t b[kJoinPacketSize]; send(b, writeJoin(b, skin_, ck)); lastJoin_ = now; }
    } else if (type == MsgReject) {
        const uint8_t why = r.get<uint8_t>();
        if (r.ok() && why == 2 && state_ == State::Connected) {   // a MESMA conta entrou em outro aparelho: para (não fica se derrubando com o outro)
            LOG("conta aberta em outro aparelho: desconectado");
            kicked_ = true; state_ = State::Offline; myId_ = -1; pending_.clear(); snaps_.clear(); invHave_ = false;
            return;
        }
        LOG("servidor recusou a conexão");
    } else if (type == MsgInvSync) {
        uint32_t rv; int32_t co; InvSlot tmp[kInvSlots]; uint8_t shp = 0;
        r.inv(rv, co, tmp, &shp);
        if (!r.ok() || state_ != State::Connected) return;
        uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgInvAck); w.put<uint32_t>(rv); send(b, w.size());
        if (invHave_ && (int32_t)(rv - invRev_) < 0) return;   // atrasado (mais antigo que o que já temos)
        ships_ = shp; invRev_ = rv; coins_ = co; std::memcpy(inv_, tmp, sizeof inv_); invHave_ = true; invNeed_ = true;
    } else if (type == MsgLoginResult) {
        LoginInfo L; const uint8_t rq = r.get<uint8_t>(); L.status = r.get<uint8_t>(); L.flags = r.get<uint8_t>(); L.health = r.get<uint8_t>(); L.hunger = r.get<uint8_t>(); L.skin = r.get<uint8_t>();
        r.bytes(L.flag, sizeof L.flag);
        if (!r.ok() || rq != loginReq_ || loginSettled_) return;
        LOG("login: resposta %d (flags %d)", L.status, L.flags);
        if (L.status == LoginExpired) { loginNeedToken_ = true; return; }   // o motor pede outro token e chama setLogin
        loginSettled_ = true;
        if (L.status == LoginNoKeys || L.status == LoginInvalid) loginRetryAt_ = nowSec() + 30.0;   // fica sem conta (efêmera) e tenta de novo depois
        if (logins_.size() < 4) logins_.push_back(L);
    } else if (type == MsgAccountResult) {
        AccountRes a; a.kind = r.get<uint8_t>(); a.status = r.get<uint8_t>(); a.coins = r.get<int32_t>(); a.hash = r.get<uint32_t>();
        if (r.ok() && accRes_.size() < 16) accRes_.push_back(a);
    } else if (type == MsgSnapshot && state_ == State::Connected) {
        uint32_t tick = r.get<uint32_t>();
        float cyc = r.get<float>();
        uint32_t echo = r.get<uint32_t>();
        uint16_t hold = r.get<uint16_t>();
        int count = r.get<uint16_t>();   // outros jogadores (área de interesse); EU vem antes deles, completo
        if (!r.ok() || count > kMaxPlayers || tick <= lastTick_) return;
        if (std::isfinite(cyc) && cyc >= 0 && cyc < kCycleSec) {
            clock_ = cyc;
            if (timeReq_ == TimeReq::Pending) {  // o servidor aceitou se o relógio foi para perto do alvo
                float d = std::fabs(cyc - timeReqTarget_);
                if (std::min(d, kCycleSec - d) < 3.0f) timeReq_ = TimeReq::Ok;
            }
        }
        if (echo != 0) {  // RTT = agora - instante em que o cliente enviou - tempo parado no servidor
            int32_t rtt = (int32_t)((uint32_t)((uint64_t)(now * 1000.0)) - echo) - (int32_t)hold;
            if (rtt >= 0 && rtt < 5000) {
                if (stats_.rttMs < 0) stats_.rttMs = (float)rtt;  // 1ª medida; depois vale a média da janela
                rttSum_ += rtt; ++rttN_;
                rttMinW_ = std::min(rttMinW_, (double)rtt); rttMaxW_ = std::max(rttMaxW_, (double)rtt);
                if (rttLast_ >= 0) { rttJitSum_ += std::fabs(rtt - rttLast_); ++rttJitN_; }
                rttLast_ = rtt;
            }
        }
        stats_.players = count + 1;
        ++snapCnt_;
        // Jitter buffer adaptativo: atraso = 2 ticks + 3.5 x desvio médio do intervalo entre snapshots
        if (lastSnapArr_ > 0) {
            double dev = std::fabs((now - lastSnapArr_) - 1.0 / kTickHz);
            snapJitter_ = snapJitter_ * 0.9 + dev * 0.1;
            double target = std::min(0.30, std::max(0.08, 2.0 / kTickHz + 3.5 * snapJitter_));
            interpDelay_ += (target - interpDelay_) * (target > interpDelay_ ? 0.2 : 0.01);  // sobe rápido, desce devagar
            stats_.interpMs = (float)(interpDelay_ * 1000.0);
        }
        lastSnapArr_ = now;
        Snap s{now, tick, {}};
        PlayerState meSrv = r.state(); uint32_t meAck = r.get<uint32_t>();
        if (!r.ok()) return;
        const bool gotMe = meSrv.id == myId_;
        s.players.reserve((size_t)count);
        for (int i = 0; i < count; ++i) {
            PlayerState ps = r.stateRel(meSrv.x, meSrv.z);   // compacto: posição relativa à minha (cm)
            if (!r.ok()) return;
            if (ps.id != myId_) s.players.push_back(ps);
        }
        for (int si = 0; si < kMaxShips; ++si) {   // navios: pose do servidor avançada por meia latência (dead reckoning) e misturada à local (o navio é lento: sem saltos visíveis)
            ShipState sv = r.ship(); sv.kind = (uint8_t)si;
            if (r.ok()) {
                ShipState& L = shipState(si);
                if (!sv.exists) L.exists = false;
                else {
                    ShipState adv = sv;
                    float lat = std::min(0.3f, stats_.rttMs > 0 ? stats_.rttMs * 0.0005f : 0.03f);
                    for (int k = 0; k < 3; ++k) stepShip(adv, lat / 3.0f, terrainPtr());
                    float dx = adv.x - L.x, dz = adv.z - L.z;
                    const bool mineHelm = gotMe && meSrv.ship == 2 && meSrv.shipIdx == si;
                    if (!L.exists || L.gen != sv.gen || dx * dx + dz * dz > 64.0f) {
                        const float keepS = L.cmdSteer; const int8_t keepT = L.cmdThrottle;
                        L = adv; L.gen = sv.gen; if (mineHelm) { L.cmdSteer = keepS; L.cmdThrottle = keepT; }
                    } else {
                        const float k = 0.35f;
                        L.x += dx * k; L.z += dz * k;
                        L.yaw = shipWrap(L.yaw + shipWrap(adv.yaw - L.yaw) * k);
                        L.speed += (adv.speed - L.speed) * k;
                        L.steer += (adv.steer - L.steer) * k;
                        L.helmer = sv.helmer;
                        L.hp = sv.hp; L.sinkT = sv.sinkT > 0.0f ? adv.sinkT : 0.0f;
                        if (!mineHelm) { L.cmdSteer = sv.cmdSteer; L.cmdThrottle = sv.cmdThrottle; }
                    }
                    L.exists = true;
                }
            }
        }
        { const uint8_t ci = r.get<uint8_t>(); if (r.ok()) shipCleanIn_ = ci == 255 ? -1.0f : (float)ci; }   // s até a LIMPEZA dos navios
        if (gotMe) {
            // Reconciliação: parte do estado autoritativo e reaplica inputs ainda não confirmados.
            e0net::PlayerState before = me_;
            me_ = meSrv;
            while (!pending_.empty() && pending_.front().seq <= meAck) pending_.pop_front();
            if (me_.ship) shipCarry(me_);
            for (const InputCmd& c : pending_) stepPlayer(me_, c);
            float ex = me_.x - before.x, ez = me_.z - before.z;
            if (before.ship && me_.ship && before.shipIdx == me_.shipIdx) {   // a bordo: o erro vale no espaço do navio (o convés andou desde o último frame)
                const ShipState& S = shipOf(me_);
                float ea = me_.sa - before.sa, eb = me_.sb - before.sb, sy = std::sin(S.yaw), cy = std::cos(S.yaw);
                ex = ea * sy + eb * cy; ez = -ea * cy + eb * sy;
            }
            float err = std::sqrt(ex * ex + ez * ez);
            if (err > corrWin_) corrWin_ = err;
            if (err > 0.05f) LOG("correção %.2f m (pendentes=%zu)", err, pending_.size());
            if (err < 2.0f) {  // correção pequena: mantém a posição desenhada contínua e a deixa convergir
                offX_ -= ex; offZ_ -= ez; offY_ -= me_.y - before.y;
            } else {
                offX_ = offY_ = offZ_ = 0;  // erro grande: teleporte real
            }
        }
        lastTick_ = tick;
        snaps_.push_back(std::move(s));
        while (snaps_.size() > 32) snaps_.pop_front();
    } else if (type == MsgCollectOk) {
        uint32_t gid = r.get<uint32_t>(); uint8_t t = r.get<uint8_t>();
        const uint32_t sl = gid >> 20, idx = gid & 0xFFFFFu;
        if (!r.ok() || !World::isCollectible(t) || sl >= (uint32_t)kIslandSlots) return;
        if (taken_[sl].size() <= idx / 8) taken_[sl].resize(idx / 8 + 1, 0);
        if (!((taken_[sl][idx / 8] >> (idx & 7)) & 1u)) { taken_[sl][idx / 8] |= (uint8_t)(1u << (idx & 7)); ++takenGen_; }
        if (mine_[sl].size() <= idx / 8) mine_[sl].resize(idx / 8 + 1, 0);
        if ((mine_[sl][idx / 8] >> (idx & 7)) & 1u) return;   // OK repetido (reenvio do servidor)
        mine_[sl][idx / 8] |= (uint8_t)(1u << (idx & 7));
        collected_.push_back({gid, t});
    } else if (type == MsgActAck) {
        uint32_t seq = r.get<uint32_t>(); uint8_t kind = r.get<uint8_t>(), res = r.get<uint8_t>(), item = r.get<uint8_t>();
        uint16_t n = r.get<uint16_t>(); uint16_t other = r.get<uint16_t>();
        if (!r.ok() || acts_.empty() || acts_.front().seq != seq) return;   // repetido ou de outra ação
        const uint32_t rev = r.get<uint32_t>();
        if (r.ok()) { ackRev_ = rev; invNeed_ = true; }   // só adota um MsgInvSync com revisão >= esta (e readota: a previsão local é descartada)
        actResults_.push_back({kind, res, item, n, other, acts_.front().a, acts_.front().b, rev});
        if (kind == ActPickupDrop)   // some já daqui (não espera o próximo MsgDropSync): evita pegar de novo o que já veio
            for (size_t i = 0; i < drops_.size(); ++i) if (drops_[i].id == acts_.front().a) { drops_.erase(drops_.begin() + (long)i); break; }
        acts_.pop_front();
        if (!acts_.empty()) acts_.front().sentAt = 0;                         // manda a próxima já
    } else if (type == MsgChestSync) {
        Chest c; int ship = r.chest(c);
        if (ship >= 0 && (int16_t)(c.ver - chests_[ship].ver) >= 0) chests_[ship] = c;   // versão mais antiga (pacote atrasado) é ignorada
    } else if (type == MsgDropSync) {
        uint16_t cnt = r.get<uint16_t>();
        if (!r.ok() || cnt > kMaxDrops) return;
        std::vector<DropInfo> next;
        for (uint16_t i = 0; i < cnt; ++i) {
            DropInfo d; d.id = r.get<uint16_t>(); d.item = r.get<uint8_t>(); d.n = r.get<uint16_t>();
            d.x = r.get<float>(); d.y = r.get<float>(); d.z = r.get<float>();
            next.push_back(d);
        }
        if (r.ok()) drops_ = std::move(next);
    } else if (type == MsgDamage) {
        uint32_t did = r.get<uint32_t>(); uint8_t amount = r.get<uint8_t>(); uint16_t from = r.get<uint16_t>();
        if (!r.ok()) return;
        uint8_t b[16]; Writer w(b, sizeof b);
        w.header(MsgDamageAck); w.put<uint32_t>(did);
        send(b, w.size());
        for (int i = 0; i < 16; ++i) if (dmgSeen_[i] == did) return;   // repetido (Ack perdido)
        dmgSeen_[dmgSeenN_++ & 15] = did;
        damages_.push_back({amount, from});
    } else if (type == MsgCannonFire) {
        CannonFire f; f.shooter = r.get<uint16_t>();
        f.sx = r.get<float>(); f.sy = r.get<float>(); f.sz = r.get<float>(); f.tx = r.get<float>(); f.ty = r.get<float>(); f.tz = r.get<float>(); f.flight = r.get<float>();
        if (r.ok() && cannonFires_.size() < 32) cannonFires_.push_back(f);
    } else if (type == MsgCannonHit) {
        CannonHit h; h.x = r.get<float>(); h.y = r.get<float>(); h.z = r.get<float>(); h.kind = r.get<uint8_t>();
        h.ship = r.get<uint8_t>(); h.hp = r.get<int16_t>(); h.from = r.get<uint16_t>();
        if (r.ok() && cannonHits_.size() < 32) cannonHits_.push_back(h);
    } else if (type == MsgShotFx) {
        ShotFx f; f.shooter = r.get<uint16_t>(); f.shotId = r.get<uint16_t>(); f.ox = r.get<float>(); f.oy = r.get<float>(); f.oz = r.get<float>();
        int n = r.get<uint8_t>();
        if (!r.ok() || n > kGunPellets) return;
        f.n = n;
        for (int i = 0; i < n; ++i) { f.p[i].x = r.get<float>(); f.p[i].y = r.get<float>(); f.p[i].z = r.get<float>(); f.p[i].kind = r.get<uint8_t>(); f.p[i].target = r.get<uint16_t>(); }
        if (r.ok() && shotFx_.size() < 32) shotFx_.push_back(f);
    } else if (type == MsgSummonResult) {
        const uint8_t id = r.get<uint8_t>(), kind = r.get<uint8_t>(), res = r.get<uint8_t>(); const uint16_t dist = r.get<uint16_t>();
        if (r.ok() && kind < kMaxShips && summons_.size() < 16 && (int)id != summonSeen_) { summonSeen_ = (int)id; summons_.push_back({kind, res, dist}); }   // o servidor responde a cada cópia: só a 1ª vale
    } else if (type == MsgChatMsg) {
        Chat c; c.from = r.get<uint16_t>(); int n = r.get<uint8_t>();
        if (!r.ok() || n > kChatMaxLen) return;
        char raw[kChatMaxLen + 1];
        for (int i = 0; i < n; ++i) raw[i] = (char)r.get<uint8_t>();
        if (!r.ok()) return;
        if (chatSanitize(raw, n, c.text) > 0 && chats_.size() < 32) chats_.push_back(c);
    } else if (type == MsgTeamInfo) {   // tripulação (bandeira) de cada jogador
        const int n = r.get<uint8_t>();
        if (!r.ok() || n > kTeamQueryMax) return;
        const double now = nowSec();
        for (int i = 0; i < n; ++i) {
            const uint16_t id = r.get<uint16_t>(); const uint32_t tm = r.get<uint32_t>(); uint8_t fl[kFlagBytes]; r.bytes(fl, kFlagBytes);
            if (!r.ok() || id >= kMaxPlayers) return;
            TeamInfo& t = teamInfo_[id]; t.known = true; t.team = tm; std::memcpy(t.flag, fl, kFlagBytes); t.at = now;
        }
    } else if (type == MsgTeamOffer) {   // fui convidado (o servidor repete 1x/s até a resposta: só o 1º de cada id vale)
        TeamOffer o; o.id = r.get<uint32_t>(); o.from = r.get<uint16_t>(); o.members = r.get<uint8_t>(); r.bytes(o.flag, kFlagBytes);
        const int ln = r.get<uint8_t>();
        if (!r.ok() || ln > 40) return;
        for (int i = 0; i < ln; ++i) o.name += (char)r.get<uint8_t>();
        if (r.ok() && o.id != lastOffer_ && teamOffers_.size() < 8) { lastOffer_ = o.id; teamOffers_.push_back(o); }
    } else if (type == MsgTeamPos) {   // onde estão os outros tripulantes online
        const int n = r.get<uint8_t>();
        if (!r.ok() || n > kTeamMaxMembers) return;
        std::vector<TeamPos> v;
        for (int i = 0; i < n; ++i) { TeamPos p; p.id = r.get<uint16_t>(); p.x = r.get<float>(); p.z = r.get<float>(); if (std::isfinite(p.x) && std::isfinite(p.z)) v.push_back(p); }
        if (r.ok()) { teamPos_.swap(v); teamPosAt_ = nowSec(); }
    } else if (type == MsgTeamChatPush) {   // mensagem nova no chat da tripulação
        TeamChatPush m; m.system = r.get<uint8_t>() != 0;
        int n = r.get<uint8_t>(); if (!r.ok() || n > 40) return;
        for (int i = 0; i < n; ++i) m.author += (char)r.get<uint8_t>();
        n = r.get<uint8_t>(); if (!r.ok() || n > 120) return;
        for (int i = 0; i < n; ++i) m.text += (char)r.get<uint8_t>();
        if (r.ok() && teamChat_.size() < 16) teamChat_.push_back(m);
    } else if (type == MsgIslandOwners) {   // ilhas dominadas (lista completa)
        const int n = r.get<uint8_t>();
        if (!r.ok() || n >= kIslandSlots) return;
        IsleOwner tmp[kIslandSlots];
        for (int i = 0; i < n; ++i) {
            const int sl = r.get<uint8_t>(); const uint32_t tm = r.get<uint32_t>(); uint8_t fl[kFlagBytes]; r.bytes(fl, kFlagBytes);
            if (!r.ok() || sl <= 0 || sl >= kIslandSlots) return;
            tmp[sl].team = tm; std::memcpy(tmp[sl].flag, fl, kFlagBytes);
        }
        std::copy(tmp, tmp + kIslandSlots, isleOwner_);
    } else if (type == MsgIslandCap) {   // mastro de uma ilha perto de mim
        IsleCap c; const int sl = r.get<uint8_t>();
        c.owner = r.get<uint32_t>(); c.pole = r.get<uint32_t>(); c.prog = r.get<uint8_t>() / 255.0f; c.state = r.get<uint8_t>();
        c.nAtt = r.get<uint8_t>(); c.nDef = r.get<uint8_t>(); c.shield = r.get<uint16_t>();
        r.bytes(c.ownerFlag, kFlagBytes); r.bytes(c.poleFlag, kFlagBytes);
        int ln = r.get<uint8_t>(); if (!r.ok() || ln > 40) return;
        for (int i = 0; i < ln; ++i) c.ownerName += (char)r.get<uint8_t>();
        ln = r.get<uint8_t>(); if (!r.ok() || ln > 40) return;
        for (int i = 0; i < ln; ++i) c.poleName += (char)r.get<uint8_t>();
        if (!r.ok() || sl <= 0 || sl >= kIslandSlots) return;
        c.at = nowSec(); isleCap_[sl] = c;
        isleOwner_[sl].team = c.owner; std::memcpy(isleOwner_[sl].flag, c.ownerFlag, kFlagBytes);   // mais novo que a lista
    } else if (type == MsgIslandEvent) {
        IsleEvent e; e.kind = r.get<uint8_t>(); e.slot = r.get<uint8_t>(); e.value = r.get<int32_t>();
        const int ln = r.get<uint8_t>(); if (!r.ok() || ln > 40) return;
        for (int i = 0; i < ln; ++i) e.name += (char)r.get<uint8_t>();
        if (r.ok() && isleEvents_.size() < 16) isleEvents_.push_back(e);
    } else if (type == MsgTeamNotice) {
        TeamNotice t; t.kind = r.get<uint8_t>(); t.result = r.get<uint8_t>(); t.other = r.get<uint16_t>();
        const int ln = r.get<uint8_t>();
        if (!r.ok() || ln > 40) return;
        for (int i = 0; i < ln; ++i) t.name += (char)r.get<uint8_t>();
        if (!r.ok()) return;
        const uint8_t key[4] = {t.kind, t.result, (uint8_t)(t.other & 255), (uint8_t)(t.other >> 8)};
        const double now = nowSec();
        if (std::memcmp(key, lastNoticeKey_, 4) == 0 && now - lastNoticeAt_ < 1.5) return;   // as 3 cópias do convite recebem a mesma resposta
        std::memcpy(lastNoticeKey_, key, 4); lastNoticeAt_ = now;
        if (teamNotices_.size() < 16) teamNotices_.push_back(t);
    } else if (type == MsgHitConfirm) {
        uint16_t target = r.get<uint16_t>(); uint8_t amount = r.get<uint8_t>();
        if (r.ok()) hitConfirms_.push_back({target, amount});
    } else if (type == MsgGift) {
        uint32_t gid = r.get<uint32_t>(); uint16_t from = r.get<uint16_t>(); uint8_t item = r.get<uint8_t>(); uint16_t n = r.get<uint16_t>();
        if (!r.ok()) return;
        uint8_t b[16]; Writer w(b, sizeof b);
        w.header(MsgGiftAck); w.put<uint32_t>(gid);
        send(b, w.size());
        for (int i = 0; i < 16; ++i) if (giftSeen_[i] == gid) return;   // repetido
        giftSeen_[giftSeenN_++ & 15] = gid;
        gifts_.push_back({from, item, n});
    } else if (type == MsgTakenSync) {
        uint8_t sl = r.get<uint8_t>(); uint32_t n = r.get<uint32_t>();
        if (!r.ok() || sl >= kIslandSlots || n > kMaxTakenBits) return;
        std::vector<uint8_t> next((n + 7) / 8);
        for (auto& b : next) b = r.get<uint8_t>();
        if (!r.ok()) return;
        for (size_t i = 0; i < mine_[sl].size(); ++i) mine_[sl][i] &= i < next.size() ? next[i] : 0;   // servidor reiniciado: esquece
        if (next != taken_[sl]) {
            for (size_t i = 0; i < taken_[sl].size(); ++i) if (taken_[sl][i] & ~(i < next.size() ? next[i] : 0)) { ++respawnGen_; break; }   // algum bit voltou a 0: recursos renasceram
            taken_[sl] = std::move(next); ++takenGen_;
        }
    } else if (type == MsgStructSync) {   // estruturas de UMA ilha (lista completa)
        const int sl = r.get<uint8_t>(), n = r.get<uint8_t>();
        if (!r.ok() || sl <= 0 || sl >= kIslandSlots || n > kIsleMaxStructs) return;
        std::vector<IsleStruct> v;
        for (int i = 0; i < n; ++i) {
            IsleStruct t; t.id = r.get<uint16_t>(); t.kind = r.get<uint8_t>(); t.rot = r.get<uint8_t>() & 7; t.x = r.get<float>(); t.y = r.get<float>(); t.z = r.get<float>();
            if (!r.ok() || t.kind >= SkKindCount || !std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z)) return;
            v.push_back(t);
        }
        auto& cur = isleStructs(sl);
        bool same = cur.size() == v.size();
        for (size_t i = 0; same && i < v.size(); ++i) same = cur[i].id == v[i].id && cur[i].kind == v[i].kind && cur[i].rot == v[i].rot && cur[i].x == v[i].x && cur[i].z == v[i].z;
        if (!same) { cur.swap(v); ++isleStructGen(sl); }
    } else if (type == MsgStructRes) {
        StructRes e; e.req = r.get<uint16_t>(); e.op = r.get<uint8_t>(); e.result = r.get<uint8_t>(); e.kind = r.get<uint8_t>();
        if (!r.ok() || e.req == lastStructRes_) return;   // as cópias do pedido recebem a mesma resposta: só a 1ª vale
        lastStructRes_ = e.req;
        if (structRes_.size() < 16) structRes_.push_back(e);
    }
}

// OCEANO ABERTO: gera, numa thread, a ilha mais perto do jogador que ainda não existe aqui (raio 1300 m). A geração é só inteiros e leva ~10-40 ms;
// o resultado é igual ao do servidor (mesma semente do mundo + setor), então nada é baixado.
void NetClient::updateIslands() {
    {
        std::lock_guard<std::mutex> l(im_);
        for (auto& r : islandReady_) { islands_[r.first] = r.second; terrain().setIsland(r.first, r.second.get()); newIslands_.push_back(r.first); }
        islandReady_.clear();
    }
    if (!worldReady_ || flat_ || !world_) return;
    // Descarrega ilhas distantes (> 2400 m) para economizar RAM/VRAM em aparelhos de 4 GB
    for (int sl = 1; sl < kIslandSlots; ++sl) {
        if (!islands_[sl]) continue;
        float dx = islands_[sl]->oxcm * 0.01f - me_.x, dz = islands_[sl]->ozcm * 0.01f - me_.z;
        if (dx * dx + dz * dz > 2400.0f * 2400.0f) {
            terrain().setIsland(sl, nullptr);
            islands_[sl].reset();
            islandWanted_[sl] = false;
            unloadedIslands_.push_back(sl);
        }
    }
    if (islandBusy_) return;
    int best = -1; float bestD = 1600.0f * 1600.0f; IslandSpec bs;
    for (int sl = 1; sl < kIslandSlots; ++sl) {
        if (islands_[sl] || islandWanted_[sl]) continue;
        int sx, sz; sectorOfSlot(sl, sx, sz);
        IslandSpec sp = islandSpec(world_->seed, sx, sz);
        if (!sp.exists) { islandWanted_[sl] = true; continue; }
        float dx = sp.oxcm * 0.01f - me_.x, dz = sp.ozcm * 0.01f - me_.z, d = dx * dx + dz * dz;
        if (d < bestD) { bestD = d; best = sl; bs = sp; }
    }
    if (best < 0) return;
    islandWanted_[best] = true; islandBusy_ = true;
    if (islandThread_.joinable()) islandThread_.join();
    islandThread_ = std::thread([this, best, bs] {
        auto w = std::make_shared<World>();
        if (World::generateIsland(bs.seed, bs.kind, bs.oxcm, bs.ozcm, *w)) {
            std::lock_guard<std::mutex> l(im_);
            islandReady_.push_back({best, w});
            LOG("ilha %d gerada: %s, %zu objetos", best, islandKindName(bs.kind), w->objects.size());
        }
        islandBusy_ = false;
    });
}

void NetClient::tick() {
    if (pendingReady_.exchange(false)) {  // novo mundo pronto (thread terminou): troca e reposiciona se ainda não está em jogo
        { std::lock_guard<std::mutex> l(wm_); world_ = pendingWorld_; pendingWorld_.reset(); }
        flat_ = false; worldReady_ = true; ++worldGen_;
        for (int i = 1; i < kIslandSlots; ++i) { islands_[i].reset(); islandWanted_[i] = false; }   // outro mundo => outro oceano
        newIslands_.clear();
        unloadedIslands_.clear();
        setTerrain(world_.get());
        if (state_ != State::Connected) {
            me_.x = world_->spawnX(); me_.z = world_->spawnZ(); me_.y = world_->heightAt(me_.x, me_.z); me_.vy = 0;
            offClean_ = kShipCleanupSec;   // offline: nenhum navio nasce sozinho (só por chamado, como online)
        }
    }
    updateIslands();
    double now = nowSec();
    if (!acts_.empty()) {   // ação confiável: (re)envia a mais antiga a cada 0.25 s; desiste (e devolve o item) após 4 s sem Ack
        PendingAct& p = acts_.front();
        if (state_ != State::Connected || now - p.firstAt > 4.0) {
            actResults_.push_back({p.kind, 0, (uint8_t)p.a, p.b, kNoPlayer, p.a, p.b});
            acts_.pop_front();
        } else if (now - p.sentAt >= 0.25 || p.sentAt == 0) {
            p.sentAt = now;
            uint8_t b[32]; Writer w(b, sizeof b);
            w.header(MsgAct); w.put<uint32_t>(p.seq); w.put<uint8_t>(p.kind); w.put<uint16_t>(p.a); w.put<uint16_t>(p.b);
            send(b, w.size());
        }
    }
    if (lastTickT_ > 0) {
        clock_ += now - lastTickT_;
        float k = std::exp(-10.0f * (float)(now - lastTickT_));  // meia-vida ~70 ms
        offX_ *= k; offY_ *= k; offZ_ *= k;
    }
    lastTickT_ = now;
    if (now - statT_ >= 1.0) {  // janela de 1 s das métricas
        double w = statT_ > 0 ? now - statT_ : 1.0;
        stats_.snapPerSec = (int)(snapCnt_ / w + 0.5);
        stats_.rxKBs = (float)(rxB_ / w / 1024.0);
        stats_.txKBs = (float)(txB_ / w / 1024.0);
        stats_.corrMax = corrWin_;
        if (rttN_ > 0) {
            stats_.rttMs = (float)(rttSum_ / rttN_); stats_.rttMin = (float)rttMinW_; stats_.rttMax = (float)rttMaxW_;
            stats_.rttJit = rttJitN_ ? (float)(rttJitSum_ / rttJitN_) : 0;
        }
        rttSum_ = rttJitSum_ = 0; rttN_ = rttJitN_ = 0; rttMinW_ = 1e9; rttMaxW_ = 0;
        snapCnt_ = 0; rxB_ = txB_ = 0; corrWin_ = 0; statT_ = now;
    }
    if (timeReq_ == TimeReq::Pending && now - timeReqAt_ > 1.2) timeReq_ = TimeReq::Refused;  // sem resposta: servidor sem --debug
    if (fd_ < 0) return;

    uint8_t buf[kMaxPacket];
    for (;;) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        char ctrl[CMSG_SPACE(sizeof(timespec))];
        iovec iov{buf, sizeof buf};
        msghdr mh{};
        mh.msg_name = &from; mh.msg_namelen = fl; mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = ctrl; mh.msg_controllen = sizeof ctrl;
        ssize_t n = recvmsg(fd_, &mh, 0);
        if (n < 0) break;
        double arrival = now;
        for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS) {
                timespec ts, rt;
                std::memcpy(&ts, CMSG_DATA(cm), sizeof ts);
                clock_gettime(CLOCK_REALTIME, &rt);
                double age = (double)(rt.tv_sec - ts.tv_sec) + (double)(rt.tv_nsec - ts.tv_nsec) * 1e-9;
                if (age >= 0 && age < 1.0) arrival = now - age;  // quanto o pacote esperou no kernel até eu ler
            }
        }
        if (from.sin_addr.s_addr != serverIp_ || from.sin_port != serverPort_) continue;  // só aceita do servidor
        rxB_ += (unsigned long)n;
        onPacket(buf, (size_t)n, arrival);
    }

    for (size_t i = 0; i < chatResend_.size();) {   // cópias das mensagens de chat
        if (now >= chatResend_[i].at) { if (state_ == State::Connected) send(chatResend_[i].pkt, chatResend_[i].len); chatResend_.erase(chatResend_.begin() + (long)i); }
        else ++i;
    }
    for (size_t i = 0; i < shotResend_.size();) {   // 2ª cópia dos tiros
        if (now >= shotResend_[i].at) { if (state_ == State::Connected) send(shotResend_[i].pkt, shotResend_[i].len); shotResend_.erase(shotResend_.begin() + (long)i); }
        else ++i;
    }
    if (state_ == State::Connecting) {
        if (now - lastJoin_ > 0.5) {
            lastJoin_ = now;
            uint8_t b[kJoinPacketSize]; send(b, writeJoin(b, skin_, joinCookie_));
        }
    } else if (state_ == State::Connected) {
        if (loginKind_ && !loginNeedToken_ && (!loginSettled_ ? now - loginSentAt_ > 1.0 : (loginRetryAt_ > 0 && now >= loginRetryAt_))) {
            if (loginSettled_) { loginSettled_ = false; loginRetryAt_ = 0; }
            sendLogin(); loginSentAt_ = now;
        }
        if (now - lastRecv_ > kTimeoutSec) {
            LOG("timeout: reconectando");
            state_ = State::Connecting; myId_ = -1; stats_.rttMs = -1; pending_.clear(); snaps_.clear(); lastJoin_ = -1;
            lastRecv_ = now;
            return;
        }
        if (now - lastSend_ >= 1.0 / kTickHz) {
            lastSend_ = now;
            uint8_t b[kMaxPacket];
            Writer w(b, sizeof b);
            w.header(MsgInput);
            w.put<uint32_t>(nowMs32());
            size_t cnt = std::min<size_t>(pending_.size(), kMaxCmdsPerPacket);
            w.put<uint8_t>((uint8_t)cnt);
            for (size_t i = pending_.size() - cnt; i < pending_.size(); ++i) w.cmd(pending_[i]);  // redundância: reenvia não confirmados
            send(b, w.size());
        }
    }
}

void NetClient::setLogin(uint8_t kind, const std::string& uid, const std::string& token, const std::string& prevLocal) {
    const bool changed = kind != loginKind_ || uid != loginUid_ || prevLocal != loginPrev_;
    loginKind_ = kind; loginUid_ = uid; loginToken_ = token; loginPrev_ = prevLocal;
    if (!kind) { loginSettled_ = true; return; }
    if (changed || loginNeedToken_) { loginSettled_ = false; loginNeedToken_ = false; loginSentAt_ = 0; }
}
void NetClient::sendLogin() {
    if (state_ != State::Connected || !loginKind_) return;
    std::vector<uint8_t> b(64 + loginUid_.size() + loginToken_.size() + loginPrev_.size()); Writer w(b.data(), b.size());
    w.header(MsgLogin); w.put<uint8_t>(++loginReq_); w.put<uint8_t>(loginKind_);
    const std::string uid = loginUid_.substr(0, 255), tok = loginToken_.size() <= (size_t)kAccountMaxLen ? loginToken_ : std::string(), prev = loginPrev_.substr(0, 255);
    w.put<uint8_t>((uint8_t)uid.size()); w.bytes(uid.data(), uid.size());
    w.put<uint16_t>((uint16_t)tok.size()); w.bytes(tok.data(), tok.size());
    w.put<uint8_t>((uint8_t)prev.size()); w.bytes(prev.data(), prev.size());
    if (w.ok()) send(b.data(), w.size());
}
void NetClient::sendVitals(uint8_t health, uint8_t hunger) {
    if (state_ != State::Connected) return;
    uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgVitals); w.put<uint8_t>(health); w.put<uint8_t>(hunger); send(b, w.size());
}
void NetClient::sendProfile(const uint8_t* flag) {
    if (state_ != State::Connected) return;
    uint8_t b[32]; Writer w(b, sizeof b); w.header(MsgProfile); w.bytes(flag, kFlagBytes);
    for (int i = 0; i < 3; ++i) send(b, w.size());
}
#if E0_DEBUG_UI
void NetClient::requestDebugCoins(int delta) {
    if (state_ != State::Connected) return;
    uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgDebugCoins); w.put<int32_t>(delta); send(b, w.size());
}
void NetClient::requestDebugItems(int item, int n) {
    if (state_ != State::Connected) return;
    uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgDebugItems); w.put<uint8_t>((uint8_t)item); w.put<uint16_t>((uint16_t)n); send(b, w.size());
}
#endif

void NetClient::sendAccount(uint8_t kind, const char* data) {
    if (state_ != State::Connected || !data) return;
    const size_t n = std::strlen(data);
    if (n == 0 || n > (size_t)kAccountMaxLen) { LOG("MsgAccount: %zu bytes (fora do limite), nao enviado", n); return; }
    std::vector<uint8_t> b(16 + n); Writer w(b.data(), b.size());
    w.header(MsgAccount); w.put<uint8_t>(kind); w.put<uint16_t>((uint16_t)n);
    for (size_t i = 0; i < n; ++i) w.put<uint8_t>((uint8_t)data[i]);
    send(b.data(), w.size()); send(b.data(), w.size());
}

double NetClient::isleCapAge(int slot) const { return slot > 0 && slot < kIslandSlots ? nowSec() - isleCap_[slot].at : 1e9; }
double NetClient::teamPosAge() const { return nowSec() - teamPosAt_; }
void NetClient::queryTeams(const uint16_t* ids, int n) {
    if (state_ != State::Connected || n <= 0) return;
    n = std::min(n, kTeamQueryMax);
    uint8_t b[16 + kTeamQueryMax * 2]; Writer w(b, sizeof b);
    w.header(MsgTeamQuery); w.put<uint8_t>((uint8_t)n);
    const double now = nowSec();
    for (int i = 0; i < n; ++i) { w.put<uint16_t>(ids[i]); if (ids[i] < kMaxPlayers && teamInfo_[ids[i]].at < now - 5.0) teamInfo_[ids[i]].at = now - 5.0; }   // não pede de novo antes da resposta
    send(b, w.size());
}
void NetClient::inviteTeam(uint16_t target) {
    if (state_ != State::Connected) return;
    ChatResend s; Writer w(s.pkt, sizeof s.pkt);
    w.header(MsgTeamInvite); w.put<uint32_t>(++inviteId_); w.put<uint16_t>(target);
    s.len = w.size(); send(s.pkt, s.len);
    s.at = nowSec() + 0.10; chatResend_.push_back(s);
    s.at = nowSec() + 0.30; chatResend_.push_back(s);
}
void NetClient::answerTeamOffer(uint32_t offerId, bool accept) {
    if (state_ != State::Connected) return;
    ChatResend s; Writer w(s.pkt, sizeof s.pkt);
    w.header(MsgTeamOfferReply); w.put<uint32_t>(offerId); w.put<uint8_t>(accept ? 1 : 0);
    s.len = w.size(); send(s.pkt, s.len);
    s.at = nowSec() + 0.10; chatResend_.push_back(s);
    s.at = nowSec() + 0.30; chatResend_.push_back(s);
}
void NetClient::sendChat(const char* text) {
    Chat c; if (!text || chatSanitize(text, (int)std::strlen(text), c.text) == 0) return;
    if (state_ != State::Connected) { c.from = (uint16_t)std::max(0, myId_); if (chats_.size() < 32) chats_.push_back(c); return; }   // offline: eco local
    ChatResend s; Writer w(s.pkt, sizeof s.pkt);
    const int n = (int)std::strlen(c.text);
    w.header(MsgChat); w.put<uint8_t>(++chatId_); w.put<uint8_t>((uint8_t)n);
    for (int i = 0; i < n; ++i) w.put<uint8_t>((uint8_t)c.text[i]);
    s.len = w.size();
    send(s.pkt, s.len);
    s.at = nowSec() + 0.08; chatResend_.push_back(s);   // 2ª e 3ª cópias (perda de pacote); o servidor ignora o chatId repetido
    s.at = nowSec() + 0.25; chatResend_.push_back(s);
}

uint16_t NetClient::requestShoot(const float o[3], const float d[3], bool aimed) {
    const uint16_t id = gunShot_++;
    if (gunShot_ == 0) gunShot_ = 1;
    if (state_ == State::Connected) {
        ShotResend s; Writer w(s.pkt, sizeof s.pkt);
        w.header(MsgShoot); w.put<uint16_t>(id); w.put(o[0]); w.put(o[1]); w.put(o[2]); w.put(d[0]); w.put(d[1]); w.put(d[2]);
        w.put<float>(renderTick()); w.put<uint8_t>(aimed ? 1 : 0);
        s.len = w.size();
        send(s.pkt, s.len);
        s.at = nowSec() + 0.05; shotResend_.push_back(s);   // 2ª cópia (perda de pacote); o servidor ignora o shotId repetido
        return id;
    }
    // offline: traça os chumbos contra o relevo aqui mesmo (não há outros jogadores)
    ShotFx f; f.shooter = (uint16_t)std::max(0, myId_); f.shotId = id; f.ox = o[0]; f.oy = o[1]; f.oz = o[2]; f.n = kGunPellets;
    static uint32_t rng = 0x9E3779B9u;
    auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)(rng & 0xFFFFFF) / 16777216.0f; };
    float rx = -d[2], rz = d[0], rl = std::sqrt(rx * rx + rz * rz); if (rl < 1e-4f) { rx = 1; rz = 0; rl = 1; } rx /= rl; rz /= rl;
    const float ux = -d[1] * rz, uy = rz * d[0] - rx * d[2], uz = d[1] * rx, spread = aimed ? kGunSpreadAim : kGunSpreadHip;
    for (int k = 0; k < kGunPellets; ++k) {
        float px = d[0], py = d[1], pz = d[2];
        if (k > 0 || !aimed) {
            const float a = rnd() * 6.2831853f, rr = spread * std::sqrt(rnd()), cr = std::cos(a) * rr, cu = std::sin(a) * rr;
            px += rx * cr + ux * cu; py += uy * cu; pz += rz * cr + uz * cu;
            const float l = std::sqrt(px * px + py * py + pz * pz); px /= l; py /= l; pz /= l;
        }
        uint8_t kind; const float t = gunTraceWorld(o[0], o[1], o[2], px, py, pz, kGunRange, kind);
        f.p[k] = {o[0] + px * t, o[1] + py * t, o[2] + pz * t, kind, kNoPlayer};
    }
    if (shotFx_.size() < 32) shotFx_.push_back(f);
    return id;
}

float NetClient::renderTick() const {
    if (snaps_.empty()) return (float)lastTick_;
    double rt = nowSec() - interpDelay_;
    size_t hi = 0;
    while (hi < snaps_.size() && snaps_[hi].t < rt) ++hi;
    if (hi == 0) return (float)snaps_.front().tick;
    if (hi >= snaps_.size()) return (float)snaps_.back().tick;
    const Snap& a = snaps_[hi - 1]; const Snap& b = snaps_[hi];
    float t = (float)((rt - a.t) / std::max(1e-6, b.t - a.t));
    return (float)a.tick + (float)(b.tick - a.tick) * std::max(0.0f, std::min(1.0f, t));
}

static float lerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + 3.14159265f, 6.2831853f);
    if (d < 0) d += 6.2831853f;
    return a + (d - 3.14159265f) * t;
}

void NetClient::remotes(std::vector<PlayerState>& out) const {
    out.clear();
    if (snaps_.empty()) return;
    remotesRaw(out);
    for (PlayerState& p : out) {   // quem está a bordo: posição no mundo = pose ATUAL do navio + posição no convés (o convés e quem está nele andam juntos)
        if (!p.ship) continue;
        const ShipState& S = shipOf(p);
        if (!S.exists) { p.ship = 0; continue; }
        shipLocalToWorld(S, p.sa, p.sb, p.x, p.z);
    }
}

void NetClient::remotesRaw(std::vector<PlayerState>& out) const {
    double rt = nowSec() - interpDelay_;
    size_t hi = 0;
    while (hi < snaps_.size() && snaps_[hi].t < rt) ++hi;
    if (hi == 0) { out = snaps_.front().players; return; }
    if (hi >= snaps_.size()) { out = snaps_.back().players; return; }  // sem extrapolação
    const Snap& a = snaps_[hi - 1];
    const Snap& b = snaps_[hi];
    float t = (float)((rt - a.t) / std::max(1e-6, b.t - a.t));
    const PlayerState* paById[kMaxPlayers] = {};
    for (const PlayerState& pa : a.players) {
        if (pa.id < kMaxPlayers) paById[pa.id] = &pa;
    }
    out.reserve(b.players.size());
    for (const PlayerState& pb : b.players) {
        PlayerState r = pb;
        const PlayerState* ppa = (pb.id < kMaxPlayers) ? paById[pb.id] : nullptr;
        if (ppa) {
            const PlayerState& pa = *ppa;
            r.x = pa.x + (pb.x - pa.x) * t;
            r.y = pa.y + (pb.y - pa.y) * t;
            r.z = pa.z + (pb.z - pa.z) * t;
            r.yaw = lerpAngle(pa.yaw, pb.yaw, t);
            r.speed = pa.speed + (pb.speed - pa.speed) * t;
            r.vy = pb.vy;
            r.aimPitch = (int8_t)std::lround(pa.aimPitch + (pb.aimPitch - pa.aimPitch) * t);
            if (pa.ship && pb.ship && pa.shipIdx == pb.shipIdx) { r.sa = pa.sa + (pb.sa - pa.sa) * t; r.sb = pa.sb + (pb.sb - pa.sb) * t; }
        }
        out.push_back(r);
    }
}

}  // namespace e0
