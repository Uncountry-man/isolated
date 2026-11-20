// Teste de integração do servidor: join, movimento autoritativo, anti speed-hack, 2 jogadores.
// Uso: build/nettest [porta] [debug|nodebug]   (servidor em 127.0.0.1, iniciado com --flat [--debug]: as medidas assumem chão plano)
//      build/nettest [porta] world               (servidor COM mundo e --debug: testa só a coleta de gravetos/pedrinhas)
//      build/nettest [porta] ship                (servidor COM mundo, --debug e --ship-respawn 3: navio — vida/naufrágio, embarcar, andar no convés, leme, acelerar, virar, 2º jogador vê tudo)
//      build/nettest [porta] rescue              (servidor --flat --debug NOVO: testa morrer / salvar (RCP) / carregar / renascer)
//      build/nettest [porta] summon              (servidor COM mundo e --debug: CHAMAR NAVIO: à frente, cooldown, em uso/a bordo)
//      build/nettest [porta] cleanup             (servidor COM mundo, --debug --ship-cleanup 6: LIMPEZA dos navios sem ninguém)
//      (o teste `ship` precisa do servidor com --ships-at-start: navios nascendo ao subir e renascendo sozinhos, como antes)
//      build/nettest [porta] chat                (servidor --flat --debug NOVO: CHAT: eco, alcance, anti-spam 1/s, limpeza do texto)
//      build/nettest [porta] gun                 (servidor --flat --debug NOVO: BACAMARTE: cabeça/corpo, distância, recarga, compensação de latência)
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../shared/protocol.h"
#include "../shared/world_sync.h"
#include "../shared/team.h"
#include "../shared/conquest.h"
using namespace e0net;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

struct Cli {
    int fd; sockaddr_in sv{}; int id = -1; uint32_t seq = 1; bool rejected = false;
    std::vector<uint8_t> taken, takenS[kIslandSlots]; int okCount = 0; uint32_t okIdx = 0xFFFFFFFFu; uint8_t okType = 0;
    size_t lastBytes = 0; ShipState ship, galleon, boat, brig; int fired = 0; std::vector<PlayerState> last; uint32_t ack = 0; int snaps = 0; float cycle = -1; uint32_t tok = 1000, echo = 0; uint16_t hold = 0xFFFF;
    Cli(uint16_t port) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int rcvBuf = 512 * 1024; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof rcvBuf);
        sv.sin_family = AF_INET; sv.sin_port = htons(port); inet_pton(AF_INET, std::getenv("NETTEST_HOST") ? std::getenv("NETTEST_HOST") : "127.0.0.1", &sv.sin_addr);   // NETTEST_HOST=ip: servidor remoto (modo acct)
    }
    ~Cli() { if (fd >= 0) close(fd); }
    void send(const uint8_t* b, size_t n) { sendto(fd, b, n, 0, (sockaddr*)&sv, sizeof sv); }
    uint8_t skin = 0;
    uint32_t cookie = 0; int challenges = 0;   // handshake (MsgChallenge)
    void join() { uint8_t b[kJoinPacketSize]; send(b, writeJoin(b, skin, cookie)); }
    uint8_t held = 0;
    uint32_t actSeq = 0; int lastActRes = -1, lastActKind = -1, lastActItem = -1, lastActN = -1; Chest chest[kMaxShips]; int chestSyncs = 0;
    int dmgGot = 0, dmgLast = 0, hitConf = 0, hitConfAmt = 0;
    uint32_t tickS = 0; int shotFx = 0, fxShooter = -1, fxN = 0; uint8_t fxKind[kGunPellets] = {}; uint16_t fxTarget[kGunPellets] = {};
    void shoot(uint16_t id, float ox, float oy, float oz, float tx, float ty, float tz, bool aimed, float viewTick = -1.0f) {
        float dx = tx - ox, dy = ty - oy, dz = tz - oz, l = std::sqrt(dx * dx + dy * dy + dz * dz);
        uint8_t b[64]; Writer w(b, sizeof b); w.header(MsgShoot); w.put<uint16_t>(id); w.put(ox); w.put(oy); w.put(oz); w.put(dx / l); w.put(dy / l); w.put(dz / l);
        w.put<float>(viewTick < 0 ? (float)tickS : viewTick); w.put<uint8_t>(aimed ? 1 : 0); send(b, w.size());
    }
    int sumGot = 0, sumRes = -1, sumDist = 0; uint8_t sumId = 0;   // MsgSummonResult: quantas, a última e a distância
    int cleanIn = -1;   // s até a LIMPEZA dos navios (255 = servidor sem limpeza)
    void summon(int kind, int copies = 1) { uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgSummonShip); w.put<uint8_t>(++sumId); w.put<uint8_t>((uint8_t)kind); for (int k = 0; k < copies; ++k) send(b, w.size()); }
    int chatGot = 0, chatFrom = -1; char chatText[kChatMaxLen + 2] = {};   // MsgChatMsg: quantas e a última
    void chat(uint8_t id, const char* txt, int copies = 1) { uint8_t b[256]; Writer w(b, sizeof b); const int n = (int)std::strlen(txt); w.header(MsgChat); w.put<uint8_t>(id); w.put<uint8_t>((uint8_t)n); for (int i = 0; i < n; ++i) w.put<uint8_t>((uint8_t)txt[i]); for (int k = 0; k < copies; ++k) send(b, w.size()); }
    int shellHits = 0, shellShip = -1, shellHp = -1;   // MsgCannonHit: quantas e a última (navio atingido 255 = nenhum, vida que ficou)
    void act(uint8_t kind, uint16_t a, uint16_t b) { uint8_t pk[32]; Writer w(pk, sizeof pk); w.header(MsgAct); w.put<uint32_t>(++actSeq); w.put<uint8_t>(kind); w.put<uint16_t>(a); w.put<uint16_t>(b); lastActRes = -1; send(pk, w.size()); }
    // ---- CONTA / INVENTÁRIO DO SERVIDOR (v42) ----
    InvSlot inv[kInvSlots]; uint8_t ships = 0; int32_t coins = -1; uint32_t invRev = 0; int invSyncs = 0;
    int loginRes = -1, loginFlags = 0, loginHealth = -1, loginHunger = -1; uint8_t loginFlag[kFlagBytes] = {}; uint8_t loginReq = 0; int rejectReason = -1;
    int accRes = -1, accCoins = 0, accGot = 0;
    // TRIPULAÇÃO no jogo
    uint32_t tmTeam[kMaxPlayers] = {}; bool tmKnown[kMaxPlayers] = {}; uint8_t tmFlag[kMaxPlayers][kFlagBytes] = {};
    int offers = 0; uint32_t offerId = 0; uint16_t offerFrom = 0; std::string offerName;
    // DOMINAÇÃO DE ILHAS
    uint32_t isleOwner[kIslandSlots] = {}; int ownersMsgs = 0;
    struct Cap { uint32_t owner = 0, pole = 0; float prog = 0; int state = -1, nAtt = 0, nDef = 0, shield = 0; int msgs = 0; } cap[kIslandSlots];
    struct IEv { int kind, slot; int32_t value; std::string name; }; std::vector<IEv> iev;
    // MELHORIAS DA ILHA
    std::vector<IsleStruct> structs[kIslandSlots]; int structSyncs = 0; int structRes = -1, structResOp = -1; uint16_t structReqId = 0;
    void structReq(uint8_t op, int slot, uint8_t kind, uint8_t rot, float x, float z, uint16_t sid) {
        uint8_t b[32]; Writer w(b, sizeof b); w.header(MsgStructReq); w.put<uint16_t>(++structReqId); w.put<uint8_t>(op); w.put<uint8_t>((uint8_t)slot); w.put<uint8_t>(kind); w.put<uint8_t>(rot);
        w.put<float>(x); w.put<float>(z); w.put<uint16_t>(sid); structRes = -1; send(b, w.size()); send(b, w.size());
    }
    const IEv* lastIev(int kind) const { for (size_t k = iev.size(); k-- > 0;) if (iev[k].kind == kind) return &iev[k]; return nullptr; }
    struct Notice { uint8_t kind, result; uint16_t other; }; std::vector<Notice> notices;
    uint32_t invId = 100;
    bool tmPos[kMaxPlayers] = {}; int tmPosMsgs = 0; int crewChats = 0; std::string crewChatLast;
    void teamQuery(std::vector<uint16_t> ids) { uint8_t b[128]; Writer w(b, sizeof b); w.header(MsgTeamQuery); w.put<uint8_t>((uint8_t)ids.size()); for (uint16_t i : ids) w.put<uint16_t>(i); send(b, w.size()); }
    void teamInvite(uint16_t target, int copies = 1) { uint8_t b[32]; Writer w(b, sizeof b); w.header(MsgTeamInvite); w.put<uint32_t>(++invId); w.put<uint16_t>(target); for (int k = 0; k < copies; ++k) send(b, w.size()); }
    void teamReply(uint32_t oid, bool acc) { uint8_t b[32]; Writer w(b, sizeof b); w.header(MsgTeamOfferReply); w.put<uint32_t>(oid); w.put<uint8_t>(acc ? 1 : 0); send(b, w.size()); }
    const Notice* lastNotice(uint8_t kind) const { for (size_t i = notices.size(); i-- > 0;) if (notices[i].kind == kind) return &notices[i]; return nullptr; }
    struct DropSeen { uint16_t id; uint8_t item; uint16_t n; }; std::vector<DropSeen> drops;
    void login(uint8_t kind, const std::string& uid, const std::string& token, const std::string& prev = "") {
        std::vector<uint8_t> b(64 + uid.size() + token.size() + prev.size()); Writer w(b.data(), b.size());
        w.header(MsgLogin); w.put<uint8_t>(++loginReq); w.put<uint8_t>(kind);
        w.put<uint8_t>((uint8_t)uid.size()); w.bytes(uid.data(), uid.size());
        w.put<uint16_t>((uint16_t)token.size()); w.bytes(token.data(), token.size());
        w.put<uint8_t>((uint8_t)prev.size()); w.bytes(prev.data(), prev.size());
        loginRes = -1; send(b.data(), w.size());
    }
    void debugItems(uint8_t item, uint16_t n) { uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgDebugItems); w.put<uint8_t>(item); w.put<uint16_t>(n); send(b, w.size()); }
    void debugCoins(int32_t d) { uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgDebugCoins); w.put<int32_t>(d); send(b, w.size()); }
    void purchase(const std::string& v) { std::vector<uint8_t> b(32 + v.size()); Writer w(b.data(), b.size()); w.header(MsgAccount); w.put<uint8_t>(AccPurchase); w.put<uint16_t>((uint16_t)v.size()); w.bytes(v.data(), v.size()); accRes = -1; send(b.data(), w.size()); }
    void vitals(uint8_t hp, uint8_t hu) { uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgVitals); w.put<uint8_t>(hp); w.put<uint8_t>(hu); send(b, w.size()); }
    void profile(const uint8_t* fl) { uint8_t b[32]; Writer w(b, sizeof b); w.header(MsgProfile); w.bytes(fl, kFlagBytes); send(b, w.size()); }
    void leave() { uint8_t b[8]; Writer w(b, sizeof b); w.header(MsgLeave); send(b, w.size()); }
    int count(int item) const { return invCount(inv, item); }
    void respawn() { uint8_t pk[8]; Writer w(pk, sizeof pk); w.header(MsgRespawn); send(pk, w.size()); }
    const PlayerState* find(int id) const { for (auto& st : last) if (st.id == id) return &st; return nullptr; }
    void input(float mx, float my, float dt, int n = 1, uint8_t flags = 0) {
        uint8_t b[kMaxPacket]; Writer w(b, sizeof b); w.header(MsgInput); w.put<uint32_t>(++tok); w.put<uint8_t>((uint8_t)n);
        for (int i = 0; i < n; ++i) { InputCmd c; c.seq = seq++; c.moveX = mx; c.moveY = my; c.dt = dt; c.flags = flags; c.held = held; w.cmd(c); }
        send(b, w.size());
    }
    void inputShip(float mx, float my, float camYaw, uint8_t shipEv, float dt = 1.0f / 30) {
        uint8_t b[kMaxPacket]; Writer w(b, sizeof b); w.header(MsgInput); w.put<uint32_t>(++tok); w.put<uint8_t>(1);
        InputCmd c; c.seq = seq++; c.moveX = mx; c.moveY = my; c.camYaw = camYaw; c.dt = dt; c.shipEv = shipEv; w.cmd(c);
        send(b, w.size());
    }
    void pump() {
        uint8_t b[kMaxPacket];
        for (;;) {
            ssize_t n = recvfrom(fd, b, sizeof b, 0, nullptr, nullptr);
            if (n < 0) return;
            if (n > 20 && b[4] == MsgSnapshot) lastBytes = (size_t)n;
            Reader r(b, (size_t)n);
            uint8_t t = r.header();
            if (t == MsgWelcome) id = r.get<uint16_t>();
            else if (t == MsgChallenge) { uint32_t ck = r.get<uint32_t>(); if (r.ok()) { cookie = ck; ++challenges; if (id < 0) join(); } }
            else if (t == MsgReject) { rejected = true; rejectReason = r.get<uint8_t>(); }
            else if (t == MsgInvSync) { uint32_t rv; int32_t co; InvSlot tmp[kInvSlots]; uint8_t shp = 0; r.inv(rv, co, tmp, &shp); if (r.ok() && rv >= invRev) { invRev = rv; ships = shp; coins = co; std::memcpy(inv, tmp, sizeof inv); ++invSyncs; uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgInvAck); w.put<uint32_t>(rv); send(pk, w.size()); } }
            else if (t == MsgLoginResult) { uint8_t rq = r.get<uint8_t>(); uint8_t st = r.get<uint8_t>(); uint8_t fl = r.get<uint8_t>(); uint8_t hp = r.get<uint8_t>(), hu = r.get<uint8_t>(); r.get<uint8_t>(); uint8_t f[kFlagBytes]; r.bytes(f, sizeof f); if (r.ok() && rq == loginReq) { loginRes = st; loginFlags = fl; loginHealth = hp; loginHunger = hu; std::memcpy(loginFlag, f, sizeof f); } }
            else if (t == MsgTeamInfo) { int n2 = r.get<uint8_t>(); for (int k = 0; k < n2; ++k) { uint16_t pid = r.get<uint16_t>(); uint32_t tm = r.get<uint32_t>(); uint8_t f[kFlagBytes]; r.bytes(f, sizeof f); if (r.ok() && pid < kMaxPlayers) { tmTeam[pid] = tm; tmKnown[pid] = true; std::memcpy(tmFlag[pid], f, sizeof f); } } }
            else if (t == MsgTeamOffer) { uint32_t oid = r.get<uint32_t>(); uint16_t fr = r.get<uint16_t>(); r.get<uint8_t>(); uint8_t f[kFlagBytes]; r.bytes(f, sizeof f); int ln = r.get<uint8_t>(); std::string nm; for (int k = 0; k < ln; ++k) nm += (char)r.get<uint8_t>(); if (r.ok()) { if (oid != offerId) ++offers; offerId = oid; offerFrom = fr; offerName = nm; } }
            else if (t == MsgIslandOwners) { int n2 = r.get<uint8_t>(); uint32_t tmp[kIslandSlots] = {}; for (int k = 0; k < n2; ++k) { int sl = r.get<uint8_t>(); uint32_t tm = r.get<uint32_t>(); uint8_t f[kFlagBytes]; r.bytes(f, sizeof f); if (sl < kIslandSlots) tmp[sl] = tm; } if (r.ok()) { std::memcpy(isleOwner, tmp, sizeof tmp); ++ownersMsgs; } }
            else if (t == MsgIslandCap) { int sl = r.get<uint8_t>(); Cap c; c.owner = r.get<uint32_t>(); c.pole = r.get<uint32_t>(); c.prog = r.get<uint8_t>() / 255.0f; c.state = r.get<uint8_t>(); c.nAtt = r.get<uint8_t>(); c.nDef = r.get<uint8_t>(); c.shield = r.get<uint16_t>(); if (r.ok() && sl < kIslandSlots) { c.msgs = cap[sl].msgs + 1; cap[sl] = c; } }
            else if (t == MsgStructSync) { int sl = r.get<uint8_t>(), n2 = r.get<uint8_t>(); std::vector<IsleStruct> v; for (int k = 0; k < n2; ++k) { IsleStruct q; q.id = r.get<uint16_t>(); q.kind = r.get<uint8_t>(); q.rot = r.get<uint8_t>(); q.x = r.get<float>(); q.y = r.get<float>(); q.z = r.get<float>(); v.push_back(q); } if (r.ok() && sl < kIslandSlots) { structs[sl] = v; ++structSyncs; } }
            else if (t == MsgStructRes) { uint16_t rq = r.get<uint16_t>(); int op = r.get<uint8_t>(), res = r.get<uint8_t>(); r.get<uint8_t>(); if (r.ok() && rq == structReqId) { structRes = res; structResOp = op; } }
            else if (t == MsgIslandEvent) { IEv e; e.kind = r.get<uint8_t>(); e.slot = r.get<uint8_t>(); e.value = r.get<int32_t>(); int ln = r.get<uint8_t>(); for (int k = 0; k < ln; ++k) e.name += (char)r.get<uint8_t>(); if (r.ok()) iev.push_back(e); }
            else if (t == MsgTeamPos) { int n2 = r.get<uint8_t>(); bool tmp[kMaxPlayers] = {}; for (int k = 0; k < n2; ++k) { uint16_t pid = r.get<uint16_t>(); r.get<float>(); r.get<float>(); if (pid < kMaxPlayers) tmp[pid] = true; } if (r.ok()) { std::memcpy(tmPos, tmp, sizeof tmPos); ++tmPosMsgs; } }
            else if (t == MsgTeamChatPush) { r.get<uint8_t>(); int n2 = r.get<uint8_t>(); for (int k = 0; k < n2; ++k) r.get<uint8_t>(); n2 = r.get<uint8_t>(); std::string tx; for (int k = 0; k < n2; ++k) tx += (char)r.get<uint8_t>(); if (r.ok()) { ++crewChats; crewChatLast = tx; } }
            else if (t == MsgTeamNotice) { Notice nt; nt.kind = r.get<uint8_t>(); nt.result = r.get<uint8_t>(); nt.other = r.get<uint16_t>(); if (r.ok()) notices.push_back(nt); }
            else if (t == MsgAccountResult) { r.get<uint8_t>(); uint8_t st = r.get<uint8_t>(); int32_t co = r.get<int32_t>(); if (r.ok()) { accRes = st; accCoins = co; ++accGot; } }
            else if (t == MsgDropSync) { uint16_t cnt = r.get<uint16_t>(); std::vector<DropSeen> v; for (int k = 0; k < cnt; ++k) { DropSeen d; d.id = r.get<uint16_t>(); d.item = r.get<uint8_t>(); d.n = r.get<uint16_t>(); r.get<float>(); r.get<float>(); r.get<float>(); v.push_back(d); } if (r.ok()) drops = v; }
            else if (t == MsgDamage) { uint32_t did = r.get<uint32_t>(); uint8_t am = r.get<uint8_t>(); r.get<uint16_t>(); if (r.ok()) { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgDamageAck); w.put<uint32_t>(did); send(pk, w.size()); static uint32_t lastId = 0; if (did != lastId) { lastId = did; ++dmgGot; dmgLast = am; } } }
            else if (t == MsgCannonFire) ++fired;
            else if (t == MsgCannonHit) { for (int k = 0; k < 3; ++k) r.get<float>(); r.get<uint8_t>(); uint8_t sh = r.get<uint8_t>(); int16_t hp = r.get<int16_t>(); r.get<uint16_t>(); if (r.ok()) { ++shellHits; shellShip = sh; shellHp = hp; } }
            else if (t == MsgHitConfirm) { r.get<uint16_t>(); uint8_t am = r.get<uint8_t>(); if (r.ok()) { ++hitConf; hitConfAmt = am; } }
            else if (t == MsgActAck) { r.get<uint32_t>(); lastActKind = r.get<uint8_t>(); lastActRes = r.get<uint8_t>(); lastActItem = r.get<uint8_t>(); lastActN = r.get<uint16_t>(); }
            else if (t == MsgChestSync) { Chest cc; int sh = r.chest(cc); if (sh >= 0 && r.ok()) { chest[sh] = cc; ++chestSyncs; } }
            else if (t == MsgCollectOk) { okIdx = r.get<uint32_t>(); okType = r.get<uint8_t>(); if (r.ok()) ++okCount; }
            else if (t == MsgTakenSync) { uint8_t sl = r.get<uint8_t>(); uint32_t nb = r.get<uint32_t>(); std::vector<uint8_t> v((nb + 7) / 8); for (auto& x : v) x = r.get<uint8_t>(); if (r.ok() && sl < kIslandSlots) { takenS[sl] = v; if (sl == 0) taken = v; } }
            else if (t == MsgSummonResult) { r.get<uint8_t>(); r.get<uint8_t>(); uint8_t res = r.get<uint8_t>(); uint16_t d = r.get<uint16_t>(); if (r.ok()) { ++sumGot; sumRes = res; sumDist = d; } }
            else if (t == MsgChatMsg) { uint16_t from = r.get<uint16_t>(); int n = r.get<uint8_t>(); char tx[kChatMaxLen + 2] = {}; for (int k = 0; k < n && k < kChatMaxLen; ++k) tx[k] = (char)r.get<uint8_t>(); if (r.ok()) { ++chatGot; chatFrom = from; std::snprintf(chatText, sizeof chatText, "%s", tx); } }
            else if (t == MsgShotFx) {
                uint16_t sh = r.get<uint16_t>(); r.get<uint16_t>(); for (int k = 0; k < 3; ++k) r.get<float>(); int n = r.get<uint8_t>();
                for (int k = 0; k < n && k < kGunPellets; ++k) { for (int q = 0; q < 3; ++q) r.get<float>(); fxKind[k] = r.get<uint8_t>(); fxTarget[k] = r.get<uint16_t>(); }
                if (r.ok()) { ++shotFx; fxShooter = sh; fxN = n; }
            }
            else if (t == MsgSnapshot) {
                tickS = r.get<uint32_t>(); cycle = r.get<float>(); echo = r.get<uint32_t>(); hold = r.get<uint16_t>(); int c = r.get<uint16_t>(); last.clear(); ++snaps;
                PlayerState self = r.state(); ack = r.get<uint32_t>(); last.push_back(self);   // EU (completo) + os outros por perto (compactos, relativos a mim)
                for (int i = 0; i < c; ++i) last.push_back(r.stateRel(self.x, self.z));
                ShipState sh = r.ship(), sg = r.ship(), sb = r.ship(), sr = r.ship(); if (r.ok()) { ship = sh; galleon = sg; galleon.kind = ShipGalleon; boat = sb; boat.kind = ShipBoat; brig = sr; brig.kind = ShipBrig; }
                const uint8_t ci = r.get<uint8_t>(); if (r.ok()) cleanIn = ci;
            }
        }
    }
    bool isTaken(uint32_t i) const { return i / 8 < taken.size() && ((taken[i / 8] >> (i & 7)) & 1u); }
    bool isTakenG(uint32_t gid) const { uint32_t sl = gid >> 20, i = gid & 0xFFFFFu; return sl < (uint32_t)kIslandSlots && i / 8 < takenS[sl].size() && ((takenS[sl][i / 8] >> (i & 7)) & 1u); }
    void collect(uint32_t i) { uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgCollect); w.put<uint32_t>(i); send(b, w.size()); }
    void teleport(float x, float z) { uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgDebugTeleport); w.put<float>(x); w.put<float>(z); send(b, w.size()); }
    const PlayerState* me() const { for (auto& s : last) if (s.id == id) return &s; return nullptr; }
};

static void wait(Cli* a, Cli* b, int ms) {
    for (int i = 0; i < ms / 5; ++i) { usleep(5000); a->pump(); if (b) b->pump(); }
}

// Coleta: servidor com mundo (+ --debug p/ teleporte). Valida distância, "quem chegou primeiro" e reenvio do OK.
static int collectTests(uint16_t port) {
    std::vector<uint8_t> payload; uint32_t hash = 0; World world;
    bool got = fetchWorld("127.0.0.1", port, 0, payload, hash, 8000) && World::fromBytes(payload.data(), payload.size(), world);
    CHECK(got, "mundo baixado do servidor (%zu objetos)", world.objects.size());
    if (!got) { std::printf("\n%d FALHA(S)\n", fails); return 1; }
    uint32_t idx = 0xFFFFFFFFu, far = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < world.objects.size(); ++i) if (world.objects[i].type == ObjStick) { idx = i; break; }
    for (uint32_t i = 0; i < world.objects.size(); ++i)
        if (World::isCollectible(world.objects[i].type) && i != idx) {
            float dx = (world.objects[i].xcm - world.objects[idx].xcm) * 0.01f, dz = (world.objects[i].zcm - world.objects[idx].zcm) * 0.01f;
            if (dx * dx + dz * dz > 30.0f * 30.0f) { far = i; break; }
        }
    CHECK(idx != 0xFFFFFFFFu && far != 0xFFFFFFFFu, "achou um graveto (%u) e um coletável longe (%u)", idx, far);
    Cli a(port), b(port);
    a.join(); b.join(); wait(&a, &b, 300);
    CHECK(a.id >= 0 && b.id >= 0, "A e B entraram (ids %d %d)", a.id, b.id);
    const WorldObject& o = world.objects[idx];
    a.teleport(o.xcm * 0.01f + 0.8f, o.zcm * 0.01f); b.teleport(o.xcm * 0.01f - 0.8f, o.zcm * 0.01f);
    for (int i = 0; i < 10; ++i) { a.input(0, 0, 1.0f / 30); b.input(0, 0, 1.0f / 30); wait(&a, &b, 33); }
    a.collect(far); wait(&a, &b, 200);
    CHECK(a.okCount == 0 && !a.isTaken(far), "coleta LONGE do item é recusada");
    a.collect(idx); wait(&a, &b, 250);
    CHECK(a.okCount == 1 && a.okIdx == idx && a.okType == ObjStick, "A pegou o graveto (OK idx=%u tipo=%u)", a.okIdx, a.okType);
    CHECK(a.isTaken(idx) && b.isTaken(idx), "todos recebem o bitset com o item pego (A=%d B=%d)", a.isTaken(idx), b.isTaken(idx));
    b.collect(idx); wait(&a, &b, 200);
    CHECK(b.okCount == 0, "B não pega o mesmo graveto (já foi de A)");
    a.collect(idx); wait(&a, &b, 200);
    CHECK(a.okCount == 2, "pedido repetido de A (OK perdido) recebe o OK de novo (%d)", a.okCount);
    a.collect(0xFFFFFFF0u); a.collect((uint32_t)world.objects.size()); wait(&a, &b, 150);
    CHECK(a.me() != nullptr && a.okCount == 2, "índices inválidos ignorados");
    Cli c(port); c.join(); wait(&c, nullptr, 300);
    CHECK(c.isTaken(idx), "quem entra depois recebe os itens já pegos no Welcome");
    {   // RENOVAÇÃO À MEIA-NOITE: coloca o relógio 1 s antes de 1350 s; o graveto pego (idx) volta para todos
        CHECK(a.isTaken(idx) && c.isTaken(idx), "antes da meia-noite o graveto continua pego");
        uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgDebugTime); w.put<float>(1349.0f); a.send(pk, w.size());
        for (int i = 0; i < 90; ++i) { a.input(0, 0, 1.0f / 30); b.input(0, 0, 1.0f / 30); c.input(0, 0, 1.0f / 30); wait(&a, &b, 33); c.pump(); }
        CHECK(!a.isTaken(idx) && !b.isTaken(idx) && !c.isTaken(idx), "meia-noite: o graveto renasce para todos (A=%d B=%d C=%d)", a.isTaken(idx), b.isTaken(idx), c.isTaken(idx));
        b.collect(idx); wait(&a, &b, 250);
        CHECK(b.okCount == 1 && b.isTaken(idx), "e pode ser pego de novo (OK=%d)", b.okCount);
        for (Cli* x : {&c}) { uint8_t lb[8]; Writer lw(lb, 8); lw.header(MsgLeave); x->send(lb, lw.size()); }
    }
    // ---- OCEANO ABERTO: coleta numa ilha pequena (gid com a ilha nos bits altos) ----
    {
        int target = -1; World isle; uint32_t sIdx = 0;
        for (int sl = 1; sl < kIslandSlots && target < 0; ++sl) {
            int sx, sz; sectorOfSlot(sl, sx, sz); IslandSpec sp = islandSpec(world.seed, sx, sz);
            if (!sp.exists || sp.kind == IslandBig) continue;
            World w2; if (!World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, w2)) continue;
            for (uint32_t i = 0; i < w2.objects.size(); ++i) if (World::isCollectible(w2.objects[i].type) && w2.objects[i].ycm > 40) { target = sl; sIdx = i; isle = std::move(w2); break; }
        }
        CHECK(target > 0, "achou uma ilha do oceano com coletavel (slot %d, obj %u, %zu objetos)", target, sIdx, isle.objects.size());
        if (target > 0) {
            const WorldObject& o = isle.objects[sIdx]; const uint32_t gid = makeGid(target, sIdx);
            Cli d(port), e(port); d.join(); e.join(); wait(&d, &e, 300);
            d.teleport(o.xcm * 0.01f + 0.8f, o.zcm * 0.01f); e.teleport(o.xcm * 0.01f - 0.8f, o.zcm * 0.01f);
            for (int i = 0; i < 10; ++i) { d.input(0, 0, 1.0f / 30); e.input(0, 0, 1.0f / 30); wait(&d, &e, 33); }
            CHECK(d.me() && std::fabs(d.me()->x - (o.xcm * 0.01f + 0.8f)) < 0.5f, "o servidor leva o jogador para a ilha (x=%.0f)", d.me() ? d.me()->x : 0.0f);
            d.collect(gid); wait(&d, &e, 250);
            CHECK(d.okCount == 1 && d.okIdx == gid && !d.isTaken(sIdx), "coleta na ilha %d: OK com o gid (%08x) e o bit da ilha inicial nao mexe", target, d.okIdx);
            CHECK(d.isTakenG(gid) && e.isTakenG(gid), "todos recebem o bitset DA ILHA (D=%d E=%d)", d.isTakenG(gid), e.isTakenG(gid));
            e.collect(gid); wait(&d, &e, 200);
            CHECK(e.okCount == 0, "outro jogador nao pega o mesmo item da ilha");
            // item igual de OUTRA ilha com o mesmo indice local nao some junto (cada ilha tem o seu bitset)
            CHECK(!d.isTakenG(makeGid(0, sIdx)), "o mesmo indice na ilha inicial continua la");
            for (Cli* x : {&d, &e}) { uint8_t lb[8]; Writer w(lb, 8); w.header(MsgLeave); x->send(lb, w.size()); }
        }
    }
    {   // cortar uma palmeira/coqueiro de uma ilha pequena: o servidor conta os golpes da ilha certa (ActHit: a = indice, b = ilha)
        int target = -1; World isle; uint32_t tIdx = 0;
        for (int sl = 1; sl < kIslandSlots && target < 0; ++sl) {
            int sx, sz; sectorOfSlot(sl, sx, sz); IslandSpec sp = islandSpec(world.seed, sx, sz);
            if (!sp.exists || sp.kind == IslandBig) continue;
            World w2; if (!World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, w2)) continue;
            for (uint32_t i = 0; i < w2.objects.size(); ++i) if (w2.objects[i].type == ObjCoconut) { target = sl; tIdx = i; isle = std::move(w2); break; }
        }
        CHECK(target > 0, "achou um coqueiro numa ilha do oceano (slot %d, obj %u)", target, tIdx);
        if (target > 0) {
            const WorldObject& o = isle.objects[tIdx]; const uint32_t gid = makeGid(target, tIdx);
            Cli d(port); d.held = HeldHatchet; d.join(); wait(&d, nullptr, 300);
            d.teleport(o.xcm * 0.01f + 1.4f, o.zcm * 0.01f);
            for (int i = 0; i < 10; ++i) { d.input(0, 0, 1.0f / 30); wait(&d, nullptr, 33); }
            int last = -1;
            for (int k = 0; k < 4 && last != 2; ++k) { d.act(ActHit, (uint16_t)tIdx, (uint16_t)target); wait(&d, nullptr, 150); last = d.lastActRes; }
            CHECK(last == 2, "3 golpes derrubam o coqueiro da ilha %d (resultado %d)", target, last);
            wait(&d, nullptr, 200);
            CHECK(d.isTakenG(gid) && !d.isTaken(tIdx), "o bitset da ilha marca o coqueiro derrubado");
            { uint8_t lb[8]; Writer w(lb, 8); w.header(MsgLeave); d.send(lb, w.size()); }
        }
    }
    for (Cli* x : {&a, &b, &c}) { uint8_t lb[8]; Writer w(lb, 8); w.header(MsgLeave); x->send(lb, w.size()); }
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
    return fails ? 1 : 0;
}

// NAVIO (servidor COM mundo e --debug): A embarca, anda até o leme e assume; acelera e vira; B (embarcado depois) fica no convés e acompanha; ocupado recusa B.
static int shipTests(uint16_t port) {
    Cli a(port), b(port);
    a.join(); b.join(); wait(&a, &b, 400);
    CHECK(a.id >= 0 && b.id >= 0, "A e B entraram");
    CHECK(a.ship.exists, "o navio existe no snapshot (%.0f, %.0f)", a.ship.x, a.ship.z);
    if (!a.ship.exists) { std::printf("\n%d FALHA(S)\n", fails); return 1; }
    auto sideOf = [&](float bb, float& x, float& z) { shipLocalToWorld(a.ship, 0.0f, bb, x, z); };
    float tx, tz; sideOf(6.2f, tx, tz);
    a.teleport(tx, tz); wait(&a, &b, 200);
    for (int i = 0; i < 5; ++i) { a.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    CHECK(a.me() && a.me()->ship == 0, "A começa fora do navio");
    a.inputShip(0, 0, 0, ShipEvBoard); wait(&a, &b, 150);
    CHECK(a.me() && a.me()->ship == 1, "A embarcou (ship=%d)", a.me() ? a.me()->ship : -1);
    CHECK(a.me() && std::fabs(a.me()->y - kShipDeckY) < 0.02f, "A está na altura do convés (%.2f)", a.me() ? a.me()->y : -9.0f);
    // anda para a popa (camYaw = yaw do navio: frente = proa; moveY = -1 vai para trás)
    {   // vai ao timão pelas escadas: (-4.5, 2.5) -> (-9.9, 2.5) -> (-10.4, 0.5) -> leme
        const float wp[4][2] = {{-4.5f, 2.5f}, {-9.9f, 2.5f}, {-10.4f, 0.5f}, {kHelmA, 0.0f}};
        for (int w = 0; w < 4; ++w)
            for (int i = 0; i < 300 && a.me() && std::hypot(a.me()->sa - wp[w][0], a.me()->sb - wp[w][1]) > 0.3f; ++i) {
                float da = wp[w][0] - a.me()->sa, db = wp[w][1] - a.me()->sb, n = std::max(0.01f, std::hypot(da, db));
                a.inputShip(db / n, da / n, a.ship.yaw, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33);
            }
    }
    CHECK(a.me() && std::hypot(a.me()->sa - kHelmA, a.me()->sb) < 1.0f && std::fabs(a.me()->y - kTerraceY) < 0.1f, "A subiu a escada e andou ate o leme (a=%.2f b=%.2f)", a.me() ? a.me()->sa : 0.0f, a.me() ? a.me()->sb : 0.0f);
    float wx, wz; if (a.me()) shipLocalToWorld(a.ship, a.me()->sa, a.me()->sb, wx, wz); else wx = wz = 0;
    CHECK(a.me() && std::hypot(a.me()->x - wx, a.me()->z - wz) < 0.6f, "posição no mundo = convés + navio");
    a.inputShip(0, 0, 0, ShipEvHelm); wait(&a, &b, 150);
    CHECK(a.me() && a.me()->ship == 2 && a.ship.helmer == a.id, "A assumiu o leme (ship=%d helmer=%d)", a.me() ? a.me()->ship : -1, a.ship.helmer);
    // B tenta o leme ocupado
    float bx, bz; sideOf(-6.2f, bx, bz); b.teleport(bx, bz); wait(&a, &b, 200);
    for (int i = 0; i < 4; ++i) { a.inputShip(0, 0, 0, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    b.inputShip(0, 0, 0, ShipEvBoard); wait(&a, &b, 150);
    CHECK(b.me() && b.me()->ship == 1, "B embarcou");
    {
        const float wp[4][2] = {{-4.5f, 2.5f}, {-9.9f, 2.5f}, {-10.4f, 0.5f}, {kHelmA + 0.3f, 0.2f}};
        for (int w = 0; w < 4; ++w)
            for (int i = 0; i < 300 && b.me() && std::hypot(b.me()->sa - wp[w][0], b.me()->sb - wp[w][1]) > 0.3f; ++i) {
                float da = wp[w][0] - b.me()->sa, db = wp[w][1] - b.me()->sb, n = std::max(0.01f, std::hypot(da, db));
                a.inputShip(0, 0, 0, 0); b.inputShip(db / n, da / n, b.ship.yaw, 0); wait(&a, &b, 33);
            }
    }
    b.inputShip(0, 0, 0, ShipEvHelm); wait(&a, &b, 150);
    CHECK(b.me() && b.me()->ship == 1, "B NÃO pega o leme ocupado (ship=%d)", b.me() ? b.me()->ship : -1);
    // acelera 4 s e vira a estibordo
    float x0 = a.ship.x, z0 = a.ship.z, yaw0 = a.ship.yaw; uint8_t thr = (uint8_t)(1u << ShipThrShift);
    for (int i = 0; i < 120; ++i) { a.inputShip(0.0f, 0, 0, thr); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    CHECK(a.ship.speed > 4.0f, "o navio acelerou (%.1f m/s)", a.ship.speed);
    CHECK(std::hypot(a.ship.x - x0, a.ship.z - z0) > 10.0f, "o navio andou %.0f m", std::hypot(a.ship.x - x0, a.ship.z - z0));
    for (int i = 0; i < 90; ++i) { a.inputShip(1.0f, 0, 0, thr); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    CHECK(shipWrap(a.ship.yaw - yaw0) > 0.2f, "virou a estibordo (+%.2f rad)", shipWrap(a.ship.yaw - yaw0));
    // todos veem A no leme e B no convés, ambos na pose do navio
    const PlayerState* ab = b.find(a.id); const PlayerState* bb = b.me();
    CHECK(ab && ab->ship == 2 && bb && bb->ship == 1, "B vê A no leme; B continua a bordo");
    if (ab) { float ex, ez; shipLocalToWorld(b.ship, ab->sa, ab->sb, ex, ez); CHECK(std::hypot(ab->x - ex, ab->z - ez) < 1.0f, "A no mundo = no convés do navio (erro %.2f m)", std::hypot(ab->x - ex, ab->z - ez)); }
    CHECK(bb && std::fabs(bb->sa) < 12.0f && std::fabs(bb->sb) < 3.2f, "B ainda dentro do navio (a=%.1f b=%.1f)", bb ? bb->sa : 0, bb ? bb->sb : 0);
    // A solta o leme e desce: o navio para de ser controlado
    a.inputShip(0, 0, 0, ShipEvHelm); wait(&a, &b, 100);
    for (int i = 0; i < 6; ++i) { a.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    CHECK(a.me() && a.me()->ship == 1 && a.ship.helmer == -1 && a.ship.cmdThrottle == 0, "A largou o leme (helmer=%d thr=%d)", a.ship.helmer, (int)a.ship.cmdThrottle);
    a.inputShip(0, 0, 0, ShipEvLeave); wait(&a, &b, 150);
    CHECK(a.me() && a.me()->ship == 0 && a.me()->y < 0.5f, "A desembarcou na água (y=%.2f)", a.me() ? a.me()->y : 9.0f);
    // GALEÃO: A embarca nele enquanto B continua no navio; cada jogador sabe em qual navio está. Leme ocupado só vale por navio.
    CHECK(a.galleon.exists, "o galeão existe no snapshot (%.0f, %.0f)", a.galleon.x, a.galleon.z);
    if (a.galleon.exists) {
        const ShipDef& G = kShipDefs[ShipGalleon];
        float gx, gz; shipLocalToWorld(a.galleon, 0.0f, G.hullHalfB + 2.0f, gx, gz);
        a.teleport(gx, gz); wait(&a, &b, 250);
        for (int i = 0; i < 5; ++i) { a.inputShip(0, 0, 0, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
        a.inputShip(0, 0, 0, ShipEvBoard); wait(&a, &b, 150);
        CHECK(a.me() && a.me()->ship == 1 && a.me()->shipIdx == ShipGalleon, "A embarcou no galeão (ship=%d idx=%d)", a.me() ? a.me()->ship : -1, a.me() ? a.me()->shipIdx : -1);
        CHECK(a.me() && std::fabs(a.me()->y - G.deckY) < 0.02f, "A na altura do convés do galeão (%.2f)", a.me() ? a.me()->y : -9.0f);
        CHECK(b.me() && b.me()->ship == 1 && b.me()->shipIdx == ShipSloop, "B continua no navio");
        const PlayerState* ab = nullptr; for (const auto& s : b.last) if (s.id == a.id) ab = &s;
        CHECK(ab && ab->ship == 1 && ab->shipIdx == ShipGalleon, "B vê A no galeão");
        const float wp[4][2] = {{-12.0f, 3.2f}, {-19.4f, 3.2f}, {-19.5f, 0.5f}, {G.helmA, 0.0f}};
        for (const auto& q : wp) {
            for (int i = 0; i < 300 && a.me() && std::hypot(a.me()->sa - q[0], a.me()->sb - q[1]) > 0.35f; ++i) {
                float da = q[0] - a.me()->sa, db = q[1] - a.me()->sb, n = std::hypot(da, db);
                a.inputShip(db / n, da / n, a.galleon.yaw, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33);
            }
        }
        a.inputShip(0, 0, 0, ShipEvHelm); wait(&a, &b, 150);
        CHECK(a.me() && a.me()->ship == 2 && a.galleon.helmer == a.id && a.ship.helmer == -1, "A assumiu o leme do galeão (ship=%d helmer=%d)", a.me() ? a.me()->ship : -1, a.galleon.helmer);
        float x0 = a.galleon.x, z0 = a.galleon.z, sx0 = a.ship.x, sz0 = a.ship.z;
        uint8_t thr = (uint8_t)(1u << ShipThrShift);
        for (int i = 0; i < 150; ++i) { a.inputShip(0, 0, 0, thr); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
        CHECK(a.galleon.speed > 3.0f && std::hypot(a.galleon.x - x0, a.galleon.z - z0) > 6.0f, "o galeão andou %.0f m (%.1f m/s)", std::hypot(a.galleon.x - x0, a.galleon.z - z0), a.galleon.speed);
        CHECK(std::hypot(a.ship.x - sx0, a.ship.z - sz0) < 8.0f && a.ship.helmer == -1, "o outro navio não obedece ao leme do galeão");
    }
    // CHALUPA: embarca, leme e acelera como os outros, mas o canhão é recusado pelo servidor mesmo que o cliente peça
    CHECK(b.boat.exists, "a chalupa existe no snapshot (%.0f, %.0f)", b.boat.x, b.boat.z);
    if (b.boat.exists) {
        const ShipDef& H = kShipDefs[ShipBoat];
        b.inputShip(0, 0, 0, ShipEvLeave); wait(&a, &b, 120);
        float hx, hz; shipLocalToWorld(b.boat, 0.0f, H.hullHalfB + 2.0f, hx, hz);
        b.teleport(hx, hz); wait(&a, &b, 250);
        for (int i = 0; i < 5; ++i) { b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
        b.inputShip(0, 0, 0, ShipEvBoard); wait(&a, &b, 150);
        CHECK(b.me() && b.me()->ship == 1 && b.me()->shipIdx == ShipBoat && std::fabs(b.me()->y - H.deckY) < 0.02f, "B embarcou na chalupa (ship=%d idx=%d y=%.2f)", b.me() ? b.me()->ship : -1, b.me() ? b.me()->shipIdx : -1, b.me() ? b.me()->y : -9.0f);
        const int fired0 = b.fired;
        for (int i = 0; i < 12; ++i) { b.inputShip(0, 0, 0, ShipGunWant); wait(&a, &b, 33); }
        { uint8_t pk[32]; Writer w(pk, sizeof pk); w.header(MsgCannon); w.put<uint8_t>(7); w.put<float>(b.boat.x + 40.0f); w.put<float>(b.boat.z); b.send(pk, w.size()); }
        wait(&a, &b, 300);
        CHECK(b.fired == fired0 && b.me() && !(b.me()->status & StGunner), "a chalupa não tem canhão (disparos=%d status=%d)", b.fired - fired0, b.me() ? b.me()->status : -1);
        // BAÚ: guardar/pegar pelo servidor; quem está em outro navio vê o conteúdo mas não mexe
        {
            const float cw[2][2] = {{-2.5f, H.chestB}, {H.chestA + H.chestHalfA + 0.45f, H.chestB}};
            for (const auto& q : cw) for (int i = 0; i < 160 && b.me() && std::hypot(b.me()->sa - q[0], b.me()->sb - q[1]) > 0.2f; ++i) {
                float da = q[0] - b.me()->sa, db = q[1] - b.me()->sb, n = std::hypot(da, db);
                b.inputShip(db / n, da / n, b.boat.yaw, 0); a.inputShip(0, 0, 0, 0); wait(&a, &b, 33);   // A manda entrada parada: o servidor derruba quem fica 5 s calado
            }
            auto pump = [&](int ms) { for (int t = 0; t < ms; t += 33) { a.inputShip(0, 0, 0, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); } };
            pump(100);
            CHECK(b.me() && std::fabs(b.me()->y - H.deckY) < 0.6f && shipChestDist(H, b.me()->sa, b.me()->sb) <= H.chestReach + 0.6f, "B esta ao alcance do bau (a=%.2f b=%.2f d=%.2f)", b.me() ? b.me()->sa : 0, b.me() ? b.me()->sb : 0, b.me() ? shipChestDist(H, b.me()->sa, b.me()->sb) : 9.0f);
            const int s0 = a.chestSyncs;
            const int k0 = b.count(5);   // kit inicial já traz cocos (3)
            b.debugItems(5, 10); pump(250);   // v42: o inventário é do servidor; B ganha 10 cocos pelo cheat (servidor --debug)
            const int k = k0 + 10;
            CHECK(b.count(5) == k, "B tem %d cocos no inventario do servidor (kit %d + 10; tem %d)", k, k0, b.count(5));
            b.act(ActChestPut, (uint16_t)(5 | (ShipBoat << 8)), 6); pump(300);
            CHECK(b.lastActRes == 1 && b.lastActItem == 5 && b.lastActN == 6 && b.count(5) == k - 6, "B guardou 6 cocos (res=%d n=%d; sobraram %d no inventario)", b.lastActRes, b.lastActN, b.count(5));
            CHECK(a.chestSyncs > s0 && a.chest[ShipBoat].s[0].id == 5 && a.chest[ShipBoat].s[0].n == 6, "A (em outro navio) ve o bau da chalupa com 6 cocos (id=%d n=%d)", a.chest[ShipBoat].s[0].id, a.chest[ShipBoat].s[0].n);
            CHECK(b.chest[ShipBoat].s[0].n == 6 && b.chest[ShipSloop].s[0].id == 0 && b.chest[ShipGalleon].s[0].id == 0, "os outros baus continuam vazios");
            a.act(ActChestTake, (uint16_t)(0 | (ShipBoat << 8)), 2); pump(250);
            CHECK(a.lastActRes == 0, "A, em outro navio, NAO pega do bau da chalupa (res=%d)", a.lastActRes);
            b.act(ActChestTake, (uint16_t)(0 | (ShipBoat << 8)), 4); pump(300);
            CHECK(b.lastActRes == 1 && b.lastActN == 4 && b.lastActItem == 5 && a.chest[ShipBoat].s[0].n == 2 && b.count(5) == k - 2, "B pegou 4 (sobram %d no bau, A ve; B tem %d)", a.chest[ShipBoat].s[0].n, b.count(5));
            b.act(ActChestTake, (uint16_t)(7 | (ShipBoat << 8)), 1); pump(250);
            CHECK(b.lastActRes == 0, "slot vazio recusado (res=%d)", b.lastActRes);
            b.act(ActChestPut, (uint16_t)(99 | (ShipBoat << 8)), 1); pump(250);
            CHECK(b.lastActRes == 0, "item inexistente recusado (res=%d)", b.lastActRes);
            b.act(ActChestPut, (uint16_t)(5 | (ShipGalleon << 8)), 1); pump(250);
            CHECK(b.lastActRes == 0 && b.chest[ShipGalleon].s[0].id == 0, "B nao guarda no bau do galeao (res=%d)", b.lastActRes);
            b.act(ActChestPut, (uint16_t)(5 | (ShipBoat << 8)), 50); pump(300);
            CHECK(b.lastActRes == 0 && b.count(5) == k - 2 && a.chest[ShipBoat].s[0].n == 2, "ADULTERACAO: guardar 50 cocos tendo k-2 e recusado (res=%d, inventario %d, bau %d)", b.lastActRes, b.count(5), a.chest[ShipBoat].s[0].n);
            b.act(ActChestPut, (uint16_t)(1 | (ShipBoat << 8)), 1); pump(250);
            CHECK(b.lastActRes == 0, "guardar graveto que nao tem e recusado (res=%d)", b.lastActRes);
            b.act(ActChestTake, (uint16_t)(0 | (ShipBoat << 8)), 2); pump(250);
            CHECK(chestCount(b.chest[ShipBoat]) == 0 && chestCount(a.chest[ShipBoat]) == 0 && b.count(5) == k, "baú esvaziado (B ve %d, A ve %d slots; B tem %d cocos)", chestCount(b.chest[ShipBoat]), chestCount(a.chest[ShipBoat]), b.count(5));
            b.act(ActChestPut, (uint16_t)(5 | (ShipBoat << 8)), 3); pump(250);   // fica 1 item para a prova de entrada tardia
        }
        const float wp[3][2] = {{-3.0f, 2.0f - 0.3f}, {-6.3f, 1.7f}, {-6.3f, 0.3f}};
        for (const auto& q : wp) {
            for (int i = 0; i < 200 && b.me() && std::hypot(b.me()->sa - q[0], b.me()->sb - q[1]) > 0.25f; ++i) {
                float da = q[0] - b.me()->sa, db = q[1] - b.me()->sb, n = std::hypot(da, db);
                b.inputShip(db / n, da / n, b.boat.yaw, 0); a.inputShip(0, 0, 0, 0); wait(&a, &b, 33);
            }
        }
        for (int i = 0; i < 120 && b.me() && std::hypot(b.me()->sa - H.helmA, b.me()->sb) > 0.3f; ++i) {
            float da = H.helmA - b.me()->sa, db = -b.me()->sb, n = std::hypot(da, db);
            b.inputShip(db / n, da / n, b.boat.yaw, 0); a.inputShip(0, 0, 0, 0); wait(&a, &b, 33);
        }
        b.inputShip(0, 0, 0, ShipEvHelm); wait(&a, &b, 150);
        CHECK(b.me() && b.me()->ship == 2 && b.boat.helmer == b.id, "B assumiu o leme da chalupa (ship=%d helmer=%d a=%.1f b=%.1f y=%.2f)", b.me() ? b.me()->ship : -1, b.boat.helmer, b.me() ? b.me()->sa : 0, b.me() ? b.me()->sb : 0, b.me() ? b.me()->y : 0);
        float x0 = b.boat.x, z0 = b.boat.z; uint8_t thr = (uint8_t)(1u << ShipThrShift);
        for (int i = 0; i < 75; ++i) { b.inputShip(0, 0, 0, thr); a.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
        CHECK(b.boat.speed > 3.0f && std::hypot(b.boat.x - x0, b.boat.z - z0) > 3.0f, "a chalupa andou %.0f m (%.1f m/s)", std::hypot(b.boat.x - x0, b.boat.z - z0), b.boat.speed);
    }
    // prova inversa: o MESMO pedido dispara no galeão (senão "disparos = 0" na chalupa não provaria nada)
    if (a.galleon.exists) {
        a.inputShip(0, 0, 0, ShipEvHelm); wait(&a, &b, 150);
        for (int i = 0; i < 12; ++i) { a.inputShip(0, 0, 0, ShipGunWant); wait(&a, &b, 33); }
        const int f0 = a.fired;
        { uint8_t pk[32]; Writer w(pk, sizeof pk); w.header(MsgCannon); w.put<uint8_t>(9); w.put<float>(a.galleon.x + 60.0f); w.put<float>(a.galleon.z); a.send(pk, w.size()); }
        wait(&a, &b, 300);
        CHECK(a.fired == f0 + 1 && a.me() && (a.me()->status & StGunner), "o galeão dispara com o mesmo pedido (disparos=%d status=%d)", a.fired - f0, a.me() ? a.me()->status : -1);
    }
    // quem entra DEPOIS recebe o conteúdo do baú (Welcome) sem pedir
    { Cli late(port); late.join(); wait(&late, nullptr, 500);
      CHECK(late.chest[ShipBoat].s[0].id == 5 && late.chest[ShipBoat].s[0].n == 3, "quem entra depois ve o bau (id=%d n=%d)", late.chest[ShipBoat].s[0].id, late.chest[ShipBoat].s[0].n);
      uint8_t lb[8]; Writer w(lb, 8); w.header(MsgLeave); late.send(lb, w.size()); }
    // ---- VIDA DOS NAVIOS: bala no casco tira vida, longe não, o próprio navio não, 0 = afunda (solta quem está a bordo), renasce ----
    // (o servidor precisa de --ship-respawn 3 para o renascimento caber no teste)
    if (a.galleon.exists && a.boat.exists && b.me() && b.me()->ship == 2 && b.me()->shipIdx == ShipBoat) {
        auto pump = [&](int ms) { for (int t = 0; t < ms; t += 33) { a.inputShip(0, 0, 0, ShipGunWant); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); } };
        auto fire = [&](uint8_t shot, float x, float z) { uint8_t pk[32]; Writer w(pk, sizeof pk); w.header(MsgCannon); w.put<uint8_t>(shot); w.put<float>(x); w.put<float>(z); a.send(pk, w.size()); };
        auto shipHp = [&](uint8_t ship, int16_t delta) { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgDebugShipHp); w.put<uint8_t>(ship); w.put<int16_t>(delta); a.send(pk, w.size()); };
        pump(3500);   // a bala do teste anterior cai e a recarga acaba
        CHECK(a.galleon.hp == kShipDefs[ShipGalleon].maxHp && a.ship.hp == kShipDefs[ShipSloop].maxHp, "navios com vida cheia no snapshot (galeao %d, navio %d)", a.galleon.hp, a.ship.hp);
        { uint8_t pk[20]; Writer w(pk, sizeof pk); w.header(MsgDebugShip); w.put<float>(a.galleon.x); w.put<float>(a.galleon.z); w.put<uint8_t>(ShipBoat); a.send(pk, w.size()); }
        pump(400);
        const float dB = std::hypot(a.boat.x - a.galleon.x, a.boat.z - a.galleon.z);
        CHECK(dB > 18.0f && dB < 140.0f && b.me() && b.me()->ship == 2, "chalupa levada ao alcance do galeao (%.0f m), B continua no leme", dB);
        const int hp0 = a.boat.hp, h0 = a.shellHits;
        fire(20, a.boat.x, a.boat.z); pump(4200);
        CHECK(a.shellHits == h0 + 1 && a.shellShip == ShipBoat && a.boat.hp == hp0 - kShipShellDamage && b.boat.hp == a.boat.hp, "bala no casco da chalupa: vida %d -> %d (msg navio %d vida %d)", hp0, a.boat.hp, a.shellShip, a.shellHp);
        {   // longe de qualquer casco: do lado oposto à chalupa
            float ux = (a.galleon.x - a.boat.x) / dB, uz = (a.galleon.z - a.boat.z) / dB, tx = a.galleon.x + ux * 70.0f, tz = a.galleon.z + uz * 70.0f;
            bool clear = shipShellDamage(a.ship, tx, tz) == 0 && shipShellDamage(a.boat, tx, tz) == 0 && shipShellDamage(a.galleon, tx, tz) == 0;
            const int hb = a.boat.hp, hs = a.ship.hp;
            fire(21, tx, tz); pump(4000);
            CHECK(clear && a.shellShip == 255 && a.boat.hp == hb && a.ship.hp == hs, "bala longe nao tira vida (navio %d, chalupa %d)", a.shellShip, a.boat.hp);
        }
        {   // fogo amigo: no casco do PRÓPRIO galeão
            float tx, tz; shipLocalToWorld(a.galleon, 20.0f, 0.0f, tx, tz);
            fire(22, tx, tz); pump(3600);
            CHECK(a.galleon.hp == kShipDefs[ShipGalleon].maxHp, "bala no proprio navio nao tira vida (%d)", a.galleon.hp);
        }
        shipHp(ShipBoat, -50); pump(300);
        CHECK(a.boat.hp == kShipDefs[ShipBoat].maxHp - kShipShellDamage - 50, "debug -50 (vida %d)", a.boat.hp);
        const uint8_t gen0 = a.boat.gen;
        shipHp(ShipBoat, -10000); pump(300);
        CHECK(a.boat.exists && a.boat.sinkT > 0.0f && a.boat.hp == 0, "chalupa AFUNDANDO (sinkT %.2f, vida %d)", a.boat.sinkT, a.boat.hp);
        CHECK(b.me() && b.me()->ship == 0 && b.boat.helmer == -1, "quem estava no leme foi para a agua (ship=%d y=%.2f)", b.me() ? b.me()->ship : -1, b.me() ? b.me()->y : 9.0f);
        b.inputShip(0, 0, 0, ShipEvBoard); pump(300);
        CHECK(b.me() && b.me()->ship == 0, "nao embarca no navio afundando");
        pump((int)(kShipSinkTime * 1000) + 300);
        CHECK(!a.boat.exists, "a chalupa sumiu depois de afundar");
        for (int i = 0; i < 40 && !a.boat.exists; ++i) pump(250);
        CHECK(a.boat.exists && a.boat.hp == kShipDefs[ShipBoat].maxHp && a.boat.sinkT == 0.0f && a.boat.gen != gen0, "chalupa RENASCEU com vida cheia (vida %d, gen %d -> %d)", a.boat.hp, gen0, a.boat.gen);
    } else CHECK(false, "pre-condicao do teste de vida (galeao/chalupa/B no leme)");
    for (Cli* x : {&a, &b}) { uint8_t lb[8]; Writer w(lb, 8); w.header(MsgLeave); x->send(lb, w.size()); }
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DO NAVIO (REDE) PASSARAM\n", fails);
    return fails ? 1 : 0;
}

// MORTE / SALVAR / CARREGAR (servidor --flat --debug NOVO): A morre, B salva (RCP) ou carrega; ações recusadas longe/em quem está vivo.
// BACAMARTE: build/nettest <porta> gun  (servidor --flat --debug NOVO). Tiro na cabeça = morte (255), corpo = dano parcial que cai com a
// distância, intervalo mínimo entre tiros, sem arma não atira, origem longe do corpo é recusada, shotId repetido é ignorado, quem está morto
// não leva tiro e a compensação de latência acerta onde o atirador VIA o alvo.
static int summonTests(uint16_t port) {
    Cli a(port), b(port);
    a.join(); b.join(); wait(&a, &b, 400);
    CHECK(a.id >= 0 && b.id >= 0, "A e B entraram");
    CHECK(!a.boat.exists && !a.galleon.exists && !a.ship.exists && !a.brig.exists, "nenhum navio existe ao entrar (so por chamado)");
    CHECK(a.cleanIn > 0 && a.cleanIn <= (int)kShipCleanupSec, "snapshot traz o contador da limpeza (%d s)", a.cleanIn);
    a.teleport(0, 40); wait(&a, &b, 300);
    for (int i = 0; i < 10; ++i) { a.inputShip(0, 1, 3.14159265f, 0); wait(&a, &b, 33);}   // vira o corpo para o SUL (yaw = pi): o mar fica nessa direção
    const float ay = a.me() ? a.me()->yaw : 0.0f;
    CHECK(a.me() && std::fabs(std::fabs(ay) - 3.14159265f) < 0.3f, "A olha para o sul (yaw %.2f)", ay);
    const uint8_t gen0 = a.boat.gen; const float bx0 = a.boat.x, bz0 = a.boat.z;
    // 1) chamar a chalupa (3 cópias do mesmo id = 1 chamado só; o servidor responde a cada uma, o cliente real só conta a 1ª)
    a.summon(ShipBoat, 3); wait(&a, &b, 500);
    CHECK(a.sumGot >= 1 && a.sumRes == SumOk, "chamado aceito (resultado %d, %d m)", a.sumRes, a.sumDist);
    CHECK(a.boat.gen != gen0 && (std::hypot(a.boat.x - bx0, a.boat.z - bz0) > 5.0f), "a chalupa foi para outro lugar (gen %d -> %d)", gen0, a.boat.gen);
    const PlayerState* me = a.me();
    const float d = me ? std::hypot(a.boat.x - me->x, a.boat.z - me->z) : 0.0f;
    CHECK(me && d >= kSummonMinDist - 1.0f && d <= kSummonMaxDist + 25.0f && a.boat.z > me->z + 20.0f, "a chalupa ficou A FRENTE (sul), a %.0f m", d);
    CHECK(a.boat.hp == shipDef(ShipBoat).maxHp && a.boat.helmer == -1, "vida cheia e sem timoneiro");
    // 2) de novo logo em seguida: cooldown
    a.sumRes = -1; a.summon(ShipGalleon); wait(&a, &b, 300);
    CHECK(a.sumRes == SumCooldown && a.sumDist > 0, "segundo chamado em seguida: aguarde (%d s)", a.sumDist);
    // 3) outro jogador, navio em uso: B chama o galeão, embarca nele (teleporta ao lado do casco) e A não consegue chamar nem B
    b.teleport(-60, 40); wait(&a, &b, 300);
    for (int i = 0; i < 10; ++i) { b.inputShip(0, 1, 3.14159265f, 0); a.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    b.sumRes = -1; b.summon(ShipGalleon); wait(&a, &b, 500);
    CHECK(b.sumRes == SumOk && a.galleon.exists, "B chamou o galeão, que antes nao existia (resultado %d)", b.sumRes);
    float gx, gz; shipLocalToWorld(a.galleon, 0.0f, shipDef(ShipGalleon).hullHalfB + 1.5f, gx, gz);
    b.teleport(gx, gz); wait(&a, &b, 250);
    for (int i = 0; i < 5; ++i) { b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    b.inputShip(0, 0, 0, ShipEvBoard); wait(&a, &b, 200);
    CHECK(b.me() && b.me()->ship == 1 && b.me()->shipIdx == ShipGalleon, "B embarcou no galeão (ship=%d idx=%d)", b.me() ? b.me()->ship : -1, b.me() ? b.me()->shipIdx : -1);
    b.sumRes = -1; b.summon(ShipGalleon); wait(&a, &b, 300);
    CHECK(b.sumRes == SumAboard, "quem está a bordo não pode chamar (resultado %d)", b.sumRes);
    a.sumRes = -1; a.summon(ShipGalleon); wait(&a, &b, 300);
    CHECK(a.sumRes == SumInUse, "navio com alguém a bordo não pode ser chamado por outro (resultado %d)", a.sumRes);
    // 4) tipo inválido: ignorado
    int g = a.sumGot; a.summon(77); wait(&a, &b, 200);
    CHECK(a.sumGot == g, "tipo de navio inválido é ignorado");
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE CHAMAR NAVIO PASSARAM\n", fails);
    return fails ? 1 : 0;
}

// NAVIOS SÃO DA CONTA: build/nettest <porta> shipown  (servidor COM mundo + --debug, SEM --own-all-ships, NOVO).
static int shipOwnTests(uint16_t port) {
    Cli a(port), b(port);
    a.join(); b.join(); wait(&a, &b, 500);
    CHECK(a.id >= 0 && b.id >= 0, "A e B entraram");
    CHECK(a.ships == 0 && b.ships == 0, "conta nova começa SEM nenhum navio (A=%d B=%d)", a.ships, b.ships);
    a.teleport(0, 40); wait(&a, &b, 300);
    for (int i = 0; i < 10; ++i) { a.inputShip(0, 1, 3.14159265f, 0); wait(&a, &b, 33); }
    a.sumRes = -1; a.summon(ShipBoat); wait(&a, &b, 300);
    CHECK(a.sumRes == SumNotOwned, "chamar sem ter o navio e recusado (resultado %d)", a.sumRes);
    a.act(ActBuyShip, ShipBoat, 0); wait(&a, &b, 300);
    CHECK(a.lastActRes == 0 && a.ships == 0, "comprar sem moedas e recusado (res=%d)", a.lastActRes);
    a.debugCoins(2000); b.debugCoins(2000); wait(&a, &b, 400);
    CHECK(a.coins == 2000, "A tem 2000 moedas (%d)", a.coins);
    a.act(ActBuyShip, ShipBoat, 0); wait(&a, &b, 400);
    CHECK(a.lastActRes == 1 && (a.ships & (1 << ShipBoat)) && a.coins == 2000 - kShipPrice[ShipBoat], "A comprou a chalupa (res=%d, ships=%d, moedas=%d)", a.lastActRes, a.ships, a.coins);
    a.act(ActBuyShip, ShipBoat, 0); wait(&a, &b, 300);
    CHECK(a.lastActRes == 0 && a.coins == 2000 - kShipPrice[ShipBoat], "comprar de novo o mesmo navio e recusado (sem cobrar)");
    a.act(ActBuyShip, 9, 0); wait(&a, &b, 300);
    CHECK(a.lastActRes == 0, "tipo de navio invalido e recusado");
    a.sumRes = -1; a.summon(ShipBoat); wait(&a, &b, 500);
    CHECK(a.sumRes == SumOk && a.boat.exists && a.boat.owner == a.id, "agora o chamado funciona e a chalupa e de A (dono=%d)", a.boat.owner);
    a.sumRes = -1; a.summon(ShipGalleon); wait(&a, &b, 300);
    CHECK(a.sumRes == SumNotOwned, "o galeao continua nao sendo dele (resultado %d)", a.sumRes);
    b.act(ActBuyShip, ShipBoat, 0); wait(&a, &b, 400);
    CHECK(b.lastActRes == 1, "B tambem compra a chalupa dele");
    b.sumRes = -1; b.summon(ShipBoat); wait(&a, &b, 400);
    CHECK(b.sumRes == SumInUse && a.boat.owner == a.id, "B nao consegue tomar a chalupa de A (resultado %d, dono continua %d)", b.sumRes, a.boat.owner);
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE NAVIO DA CONTA PASSARAM\n", fails);
    return fails ? 1 : 0;
}

// LIMPEZA DE NAVIOS: build/nettest <porta> cleanup  (servidor COM mundo, --debug --ship-cleanup 6, NOVO). A chama a chalupa e sai de perto;
// B chama o galeão e embarca. Na limpeza a chalupa (ninguém usando) some e o galeão (B a bordo) fica; depois B desce, sai de perto e o galeão some.
static int cleanupTests(uint16_t port) {
    Cli a(port), b(port);
    a.join(); b.join(); wait(&a, &b, 400);
    CHECK(a.id >= 0 && b.id >= 0, "A e B entraram");
    CHECK(!a.boat.exists && !a.galleon.exists, "nenhum navio ao entrar");
    a.teleport(0, 40); b.teleport(-60, 40); wait(&a, &b, 300);
    for (int i = 0; i < 10; ++i) { a.inputShip(0, 1, 3.14159265f, 0); b.inputShip(0, 1, 3.14159265f, 0); wait(&a, &b, 33); }
    a.summon(ShipBoat); b.summon(ShipGalleon); wait(&a, &b, 500);
    CHECK(a.boat.exists && a.galleon.exists, "chalupa e galeão chamados");
    float gx, gz; shipLocalToWorld(b.galleon, 0.0f, shipDef(ShipGalleon).hullHalfB + 1.5f, gx, gz);
    b.teleport(gx, gz); a.teleport(0, 0); wait(&a, &b, 250);
    for (int i = 0; i < 5; ++i) { b.inputShip(0, 0, 0, 0); a.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    b.inputShip(0, 0, 0, ShipEvBoard); wait(&a, &b, 200);
    CHECK(b.me() && b.me()->ship && b.me()->shipIdx == ShipGalleon, "B embarcou no galeão");
    const int c0 = a.cleanIn;
    CHECK(c0 > 0 && c0 <= 6, "contador no snapshot (%d s)", c0);
    for (int i = 0; i < 230 && a.boat.exists; ++i) { a.inputShip(0, 0, 0, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    CHECK(!a.boat.exists, "chalupa SEM NINGUEM sumiu na limpeza");
    CHECK(a.galleon.exists, "galeão com B a bordo continuou");
    CHECK(a.cleanIn >= 4, "contador reiniciou (%d s)", a.cleanIn);
    b.inputShip(0, 0, 0, ShipEvLeave); wait(&a, &b, 200);
    b.teleport(0, -200); wait(&a, &b, 300);
    for (int i = 0; i < 230 && a.galleon.exists; ++i) { a.inputShip(0, 0, 0, 0); b.inputShip(0, 0, 0, 0); wait(&a, &b, 33); }
    CHECK(!a.galleon.exists, "galeão sumiu depois que B desceu e saiu de perto");
    a.summon(ShipBoat); wait(&a, &b, 400);
    CHECK(a.sumRes == SumOk || a.sumRes == SumCooldown, "depois de sumir o navio pode ser chamado de novo (resultado %d)", a.sumRes);
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DA LIMPEZA DE NAVIOS PASSARAM\n", fails);
    return fails ? 1 : 0;
}

static int chatTests(uint16_t port) {
    Cli a(port), b(port), c(port);
    a.join(); b.join(); c.join(); wait(&a, &b, 300); wait(&c, nullptr, 100);
    CHECK(a.id >= 0 && b.id >= 0 && c.id >= 0, "3 jogadores conectados (%d %d %d)", a.id, b.id, c.id);
    a.teleport(0, 0); b.teleport(0, -20); c.teleport(0, 400); wait(&a, &b, 400); wait(&c, nullptr, 100);
    // 1) mensagem normal: quem está perto recebe (inclusive quem falou); quem está longe (400 m) não
    a.chat(1, "Ola, pessoal!", 3); wait(&a, &b, 400); wait(&c, nullptr, 100);
    CHECK(b.chatGot == 1 && b.chatFrom == a.id && std::strcmp(b.chatText, "Ola, pessoal!") == 0, "B (perto) recebeu a mensagem de A (%d, de %d, '%s'): 3 copias do mesmo chatId viram 1", b.chatGot, b.chatFrom, b.chatText);
    CHECK(a.chatGot == 1 && a.chatFrom == a.id, "quem falou recebe o eco (confirmação de entrega)");
    CHECK(c.chatGot == 0, "C (400 m) não recebe");
    // 2) rápido demais (< %.1f s): descartado
    a.chat(2, "spam", 1); wait(&a, &b, 300);
    CHECK(b.chatGot == 1, "segunda mensagem em menos de %.1f s é descartada (B tem %d)", kChatInterval, b.chatGot);
    // 3) depois do intervalo passa; o texto é limpo (acentos/controle saem, espaços repetidos viram 1, 80 caracteres no máx.)
    wait(&a, &b, (int)(kChatInterval * 1000) + 100);
    char longTxt[200]; std::memset(longTxt, 'x', sizeof longTxt - 1); longTxt[sizeof longTxt - 1] = 0; std::memcpy(longTxt, "  a   b\x01\xE9" "\xA9" "c  ", 12);
    a.chat(3, longTxt, 2); wait(&a, &b, 400);
    CHECK(b.chatGot == 2 && std::strlen(b.chatText) == (size_t)kChatMaxLen && std::strncmp(b.chatText, "a b\xE9" "c x", 7) == 0, "texto limpo: %zu caracteres; controle e (c) saem, o acento (Latin-1) fica", std::strlen(b.chatText));
    // 4) só espaços/vazio: ignorado
    wait(&a, &b, (int)(kChatInterval * 1000) + 100);
    a.chat(4, "   ", 1); a.chat(5, "", 1); wait(&a, &b, 300);
    CHECK(b.chatGot == 2, "mensagem vazia é ignorada");
    // 5) B responde (limites são por jogador)
    b.chat(1, "Oi A", 1); wait(&a, &b, 300);
    CHECK(a.chatGot == 3 && a.chatFrom == b.id && std::strcmp(a.chatText, "Oi A") == 0, "B responde e A recebe ('%s' de %d)", a.chatText, a.chatFrom);
    // 6) C chega perto e fala: A e B recebem
    c.teleport(0, 10); wait(&a, &b, 400); wait(&c, nullptr, 100);
    c.chat(1, "Cheguei", 2); wait(&a, &b, 300);
    CHECK(a.chatGot == 4 && b.chatGot == 4 && std::strcmp(b.chatText, "Cheguei") == 0, "C perto de todos: A e B recebem");
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE CHAT PASSARAM\n", fails);
    return fails ? 1 : 0;
}

static int gunTests(uint16_t port) {
    Cli a(port), b(port);
    a.join(); b.join(); wait(&a, &b, 300);
    CHECK(a.id >= 0 && b.id >= 0, "2 jogadores conectados (%d, %d)", a.id, b.id);
    a.teleport(0, 0); b.teleport(0, -10); wait(&a, &b, 300);
    a.held = HeldGun; a.input(0, 0, 0.03f); b.input(0, 0, 0.03f); wait(&a, &b, 200);
    const PlayerState* pa = a.find(a.id);
    CHECK(pa && pa->held == HeldGun, "A está com o BACAMARTE na mão (held=%d)", pa ? pa->held : -1);
    uint16_t sid = 1;
    // 1) cabeça a 10 m (mirando: o 1º chumbo vai exato no centro)
    a.shoot(sid++, 0, 1.45f, 0, 0, kHeadY, -10, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 1 && b.dmgLast == kGunHeadDamage, "TIRO NA CABECA = morte certa (B levou %d, esperado %d)", b.dmgLast, (int)kGunHeadDamage);
    CHECK(a.hitConf >= 1 && a.hitConfAmt == kGunHeadDamage, "A recebe a confirmação de tiro na cabeça (%d)", a.hitConfAmt);
    CHECK(b.shotFx >= 1 && b.fxShooter == a.id && b.fxN == kGunPellets && b.fxKind[0] == ShotHead && b.fxTarget[0] == b.id, "todos recebem o efeito do tiro (B viu %d tiros, 1º chumbo kind=%d alvo=%d)", b.shotFx, b.fxKind[0], b.fxTarget[0]);
    // 2) outro tiro logo em seguida: recarregando -> ignorado
    int fx0 = b.shotFx; b.dmgGot = 0;
    a.shoot(sid++, 0, 1.45f, 0, 0, 1.0f, -10, true); wait(&a, &b, 250);
    CHECK(b.dmgGot == 0 && b.shotFx == fx0, "tiro antes de %.1f s (recarregando) é ignorado", kGunMinInterval);
    wait(&a, &b, (int)(kGunMinInterval * 1000));
    // 3) corpo a 10 m: dano parcial
    a.shoot(sid++, 0, 1.45f, 0, 0, 1.0f, -10, true); wait(&a, &b, 300);
    const int body10 = b.dmgLast;
    CHECK(b.dmgGot == 1 && body10 >= kGunBodyDamage && body10 < kGunHeadDamage && body10 <= kGunBodyDamage * kGunPellets, "tiro no CORPO a 10 m: dano parcial %d (entre %d e %d)", body10, kGunBodyDamage, kGunBodyDamage * kGunPellets);
    // 4) mesmo shotId de novo: ignorado
    wait(&a, &b, (int)(kGunMinInterval * 1000)); b.dmgGot = 0; fx0 = b.shotFx;
    a.shoot((uint16_t)(sid - 1), 0, 1.45f, 0, 0, 1.0f, -10, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 0 && b.shotFx == fx0, "cópia repetida do mesmo tiro (shotId) é ignorada");
    // 5) longe (50 m): o dano cai com a distância
    b.teleport(0, -50); wait(&a, &b, 300); b.dmgGot = 0;
    a.shoot(sid++, 0, 1.45f, 0, 0, 1.0f, -50, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 1 && b.dmgLast < body10, "a 50 m o tiro no corpo tira menos (%d < %d)", b.dmgLast, body10);
    // 6) errou (mira 3 m ao lado): ninguém leva dano, mas o efeito sai
    wait(&a, &b, (int)(kGunMinInterval * 1000)); b.dmgGot = 0; fx0 = b.shotFx;
    a.shoot(sid++, 0, 1.45f, 0, 3.0f, 1.0f, -50, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 0 && b.shotFx == fx0 + 1, "tiro que erra: sem dano, efeito difundido (fx %d -> %d)", fx0, b.shotFx);
    // 7) origem longe do corpo do atirador: recusado
    wait(&a, &b, (int)(kGunMinInterval * 1000)); b.teleport(0, -10); wait(&a, &b, 300); b.dmgGot = 0; fx0 = b.shotFx;
    a.shoot(sid++, 0, 1.45f, -8.0f, 0, kHeadY, -10, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 0 && b.shotFx == fx0, "origem do tiro longe do atirador (8 m) é recusada");
    // 8) compensação de latência: B foi para o lado, mas A atira onde o VIA (viewTick de antes)
    const float oldTick = (float)a.tickS;
    b.teleport(20, -10); wait(&a, &b, 250); b.dmgGot = 0;
    a.shoot(sid++, 0, 1.45f, 0, 0, kHeadY, -10, true, oldTick); wait(&a, &b, 300);
    CHECK(b.dmgGot == 1 && b.dmgLast == kGunHeadDamage, "compensação de latência: acerta onde o atirador via o alvo (dano %d)", b.dmgLast);
    // 9) quem está MORTO não leva tiro
    wait(&a, &b, (int)(kGunMinInterval * 1000)); b.act(ActDie, 0, 0); wait(&a, &b, 250); b.dmgGot = 0;
    a.shoot(sid++, 0, 1.45f, 0, 20, kHeadY, -10, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 0, "jogador MORTO não leva tiro (dano %d)", b.dmgGot);
    b.respawn(); wait(&a, &b, 250);
    // 10) sem a arma na mão: não atira
    b.teleport(0, -10); a.held = HeldNone; a.input(0, 0, 0.03f); wait(&a, &b, (int)(kGunMinInterval * 1000)); b.dmgGot = 0; fx0 = b.shotFx;
    a.shoot(sid++, 0, 1.45f, 0, 0, kHeadY, -10, true); wait(&a, &b, 300);
    CHECK(b.dmgGot == 0 && b.shotFx == fx0, "sem o bacamarte na mão o tiro é recusado");
    // 11) mirando: o servidor deixa andar só devagar e o corpo vira para a câmera
    a.held = HeldGun; a.teleport(0, 0); wait(&a, &b, 250);
    {
        uint8_t pk[kMaxPacket]; Writer w(pk, sizeof pk); w.header(MsgInput); w.put<uint32_t>(++a.tok); w.put<uint8_t>(15);
        for (int i = 0; i < 15; ++i) { InputCmd c; c.seq = a.seq++; c.moveX = 1.0f; c.moveY = 0; c.camYaw = 1.0f; c.camPitch = 0.3f; c.dt = 1.0f / 30; c.flags = InputRun; c.held = HeldGun; c.aim = AimOn; w.cmd(c); }
        a.send(pk, w.size()); wait(&a, &b, 250);
    }
    pa = b.find(a.id);
    CHECK(pa && pa->speed <= kAimWalkSpeed + 0.01f && std::fabs(wrapPi(pa->yaw - 1.0f)) < 0.05f && (pa->aim & AimOn) && pa->aimPitch == 17,
          "mirando: anda a %.1f m/s (máx %.1f, sem correr), corpo virado p/ a câmera (yaw %.2f), mira replicada (aim=%d pitch=%d)", pa ? pa->speed : -1.0f, kAimWalkSpeed, pa ? pa->yaw : 0.0f, pa ? pa->aim : -1, pa ? pa->aimPitch : -99);
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DO BACAMARTE PASSARAM\n", fails);
    return fails ? 1 : 0;
}

// Uso: build/nettest <porta> rescue
static int rescueTests(uint16_t port) {
    Cli a(port), b(port);
                a.join(); b.join(); wait(&a, &b, 300);
        a.teleport(0, 0); b.teleport(1.0f, 0); wait(&a, &b, 250);
        b.act(ActRevive, (uint16_t)a.id, 0); wait(&a, &b, 200);
        CHECK(b.lastActRes == 0, "salvar quem está VIVO é recusado (res=%d)", b.lastActRes);
        a.act(ActDie, 0, 0); wait(&a, &b, 250);
        CHECK(a.lastActRes == 1 && b.find(a.id) && (b.find(a.id)->status & StDead), "ActDie: A fica MORTO e B vê o status (st=%d)", b.find(a.id) ? b.find(a.id)->status : -1);
        // salvar: RCP de kReviveTime s
        b.act(ActRevive, (uint16_t)a.id, 0); wait(&a, &b, 300);
        const PlayerState* sb = b.find(b.id);
        CHECK(b.lastActRes == 1 && sb && (sb->status & StRescuing) && sb->link == a.id + 1, "B começou a salvar A (status=%d link=%d)", sb ? sb->status : -1, sb ? sb->link : -1);
        wait(&a, &b, (int)(kReviveTime * 1000) - 100);
        CHECK(a.find(a.id) && !(a.find(a.id)->status & StDead), "após %.0f s de RCP, A volta à vida (st=%d)", kReviveTime, a.find(a.id) ? a.find(a.id)->status : -1);
        // carregar: o corpo segue quem carrega
        a.act(ActDie, 0, 0); wait(&a, &b, 250);
        b.act(ActCarry, (uint16_t)a.id, 0); wait(&a, &b, 250);
        const PlayerState* pa = b.find(a.id); sb = b.find(b.id);
        CHECK(b.lastActRes == 1 && pa && (pa->status & StCarried) && pa->link == b.id + 1 && sb && (sb->status & StCarrying), "B carrega A (A.status=%d link=%d, B.status=%d)", pa ? pa->status : -1, pa ? pa->link : -1, sb ? sb->status : -1);
        for (int i = 0; i < 30; ++i) { b.input(0, 1, 1.0f / 30); wait(&a, &b, 33); }
        wait(&a, &b, 150);
        pa = b.find(a.id); sb = b.find(b.id);
        float dd = (pa && sb) ? std::sqrt((pa->x - sb->x) * (pa->x - sb->x) + (pa->z - sb->z) * (pa->z - sb->z)) : 99.0f;
        CHECK(pa && sb && dd < 0.4f && sb->z < -1.0f, "o carregado ACOMPANHA quem carrega (distância %.2f m, B andou até z=%.1f)", dd, sb ? sb->z : 0.0f);
        b.act(ActDropCarry, 0, 0); wait(&a, &b, 250);
        pa = b.find(a.id); sb = b.find(b.id);
        CHECK(b.lastActRes == 1 && pa && (pa->status & StDead) && !(pa->status & StCarried) && sb && !(sb->status & StCarrying), "B larga A: continua morto, no chão (A.status=%d)", pa ? pa->status : -1);
        // longe demais: recusado
        a.teleport(40, 40); wait(&a, &b, 200);
        b.act(ActCarry, (uint16_t)a.id, 0); wait(&a, &b, 200);
        CHECK(b.lastActRes == 0, "carregar de LONGE é recusado (res=%d)", b.lastActRes);
        // renascer sozinho
        a.respawn(); wait(&a, &b, 250);
        CHECK(a.find(a.id) && a.find(a.id)->status == 0, "MsgRespawn: A volta à vida sozinho (st=%d)", a.find(a.id) ? a.find(a.id)->status : -1);
        // cancelar o RCP: B se afasta no meio
        a.teleport(0, 0); b.teleport(0.8f, 0); wait(&a, &b, 250);
        a.act(ActDie, 0, 0); wait(&a, &b, 250);
        b.act(ActRevive, (uint16_t)a.id, 0); wait(&a, &b, 400);
        b.teleport(30, 30); wait(&a, &b, 600);
        wait(&a, &b, (int)(kReviveTime * 1000));
        CHECK(a.find(a.id) && (a.find(a.id)->status & StDead), "RCP é CANCELADO se quem salva se afasta (A segue morto)");
        a.respawn(); wait(&a, &b, 200);
    // DANO ENTRE JOGADORES: depende do que está na mão; só acerta à frente, em quem está vivo
    {
        Cli a2(port), b2(port);
        a2.join(); b2.join(); wait(&a2, &b2, 300);
        auto punch = [&](Cli& x) { x.input(0, 0, 0.03f, 1, (uint8_t)(ActPunch << InputActShift)); };
        a2.teleport(0, 0); b2.teleport(0, -1.0f); wait(&a2, &b2, 250);   // B logo à FRENTE de A (yaw 0 olha para -Z)
        punch(a2); wait(&a2, &b2, 700);
        CHECK(b2.dmgGot == 1 && b2.dmgLast == meleeDamage(HeldNone) && a2.hitConf == 1, "soco com a mão nua acerta (-%d): B levou %d, A recebeu confirmação %d", meleeDamage(HeldNone), b2.dmgLast, a2.hitConf);
        a2.held = HeldDagger; a2.input(0, 0, 0.03f); wait(&a2, &b2, 150); b2.dmgGot = 0;
        punch(a2); wait(&a2, &b2, 700);
        CHECK(b2.dmgGot == 1 && b2.dmgLast == meleeDamage(HeldDagger), "com a ADAGA na mão o dano é maior (-%d, esperado %d)", b2.dmgLast, meleeDamage(HeldDagger));
        a2.held = HeldNone; a2.input(0, 0, 0.03f); b2.dmgGot = 0; wait(&a2, &b2, 150);
        b2.teleport(0, 1.0f); wait(&a2, &b2, 250);   // B ATRÁS de A: fora do cone
        punch(a2); wait(&a2, &b2, 700);
        CHECK(b2.dmgGot == 0, "golpe não acerta quem está atrás (dano recebido %d)", b2.dmgGot);
        b2.teleport(0, -4.0f); wait(&a2, &b2, 250);  // longe
        punch(a2); wait(&a2, &b2, 700);
        CHECK(b2.dmgGot == 0, "golpe não alcança quem está longe (dano recebido %d)", b2.dmgGot);
        b2.teleport(0, -1.0f); wait(&a2, &b2, 250);
        b2.act(ActDie, 0, 0); wait(&a2, &b2, 250);
        punch(a2); wait(&a2, &b2, 700);
        CHECK(b2.dmgGot == 0, "quem está MORTO não leva dano (dano recebido %d)", b2.dmgGot);
    }
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE SALVAMENTO E DANO PASSARAM\n", fails);
    return fails ? 1 : 0;
}

// PERSISTÊNCIA (v42): servidor com mundo + --debug --data DIR --jwk DIR_JWT/test.jwk. Fase 1 (persist1): contas, moedas, inventário, recibos,
// adulteração, 2 contas, mesma conta em 2 sessões. Depois o servidor é REINICIADO (tools/persist_test.sh) e a fase 2 (persist2) confere os dados.
static std::string readTxt(const std::string& p) { std::string s; if (FILE* f = std::fopen(p.c_str(), "rb")) { char b[4096]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, r); std::fclose(f); } return s; }
static bool joinWait(Cli& c, int ms = 600) { c.join(); for (int t = 0; t < ms && c.id < 0; t += 10) { usleep(10000); c.pump(); } return c.id >= 0; }
static void idle(std::vector<Cli*> cs, int ms) { for (int t = 0; t < ms; t += 33) { for (Cli* c : cs) { c->input(0, 0, 1.0f / 30); c->pump(); } usleep(33000); } }
static bool loginWait(Cli& c, std::vector<Cli*> others, uint8_t kind, const std::string& uid, const std::string& tok, const std::string& prev = "") {
    others.push_back(&c); c.loginRes = -1;
    for (int k = 0; k < 6 && c.loginRes < 0; ++k) { c.login(kind, uid, tok, prev); idle(others, 300); }
    return c.loginRes >= 0;
}
static const uint8_t kTestFlag[kFlagBytes] = {3, 7, 1, 12, 2, 1, 40, 2, 1, 0, 1};
static int persistTests(uint16_t port, int phase, const char* jdir) {
    const std::string d = jdir ? jdir : "build/jwttest";
    const std::string tA = readTxt(d + "/token_alice.txt"), tB = readTxt(d + "/token_bob.txt");
    CHECK(!tA.empty() && !tB.empty(), "tokens de teste lidos de %s", d.c_str());
    if (phase == 1) {
        Cli a(port), b(port), c(port), e(port);
        CHECK(joinWait(a) && joinWait(b) && joinWait(c), "A, B e C entraram");
        idle({&a, &b, &c}, 200);
        CHECK(a.coins == 0 && a.count(item::Dagger) == 1 && a.count(item::Coconut) == 3 && a.count(item::Hatchet) == 0, "sem login: conta efemera com o kit inicial (moedas %d, cocos %d)", a.coins, a.count(item::Coconut));
        // tokens ruins
        loginWait(a, {&b, &c}, LoginFirebase, "", readTxt(d + "/token_alice_expirado.txt"));
        CHECK(a.loginRes == LoginExpired, "token expirado -> LoginExpired (%d)", a.loginRes);
        { std::string x = tA; x[x.size() - 4] = x[x.size() - 4] == 'A' ? 'B' : 'A'; a.loginRes = -1; loginWait(a, {&b, &c}, LoginFirebase, "", x); }
        CHECK(a.loginRes == LoginInvalid, "assinatura adulterada -> LoginInvalid (%d)", a.loginRes);
        loginWait(a, {&b, &c}, LoginFirebase, "", readTxt(d + "/token_alice_outroaud.txt"));
        CHECK(a.loginRes == LoginInvalid, "token de outro projeto -> LoginInvalid (%d)", a.loginRes);
        loginWait(a, {&b, &c}, LoginLocal, "alice", "");
        CHECK(a.loginRes == LoginBadUid, "uid sem 'local-' (tentando pegar a conta 'alice' so pelo uid) -> recusado (%d)", a.loginRes);
        // login válido
        loginWait(a, {&b, &c}, LoginFirebase, "", tA);
        idle({&a, &b, &c}, 300);
        CHECK(a.loginRes == LoginOk && (a.loginFlags & LoginNewAccount), "alice entrou (verificada, conta nova) res=%d flags=%d", a.loginRes, a.loginFlags);
        CHECK(a.coins == 0 && a.count(item::Dagger) == 1 && a.count(item::Coconut) == 3 && a.count(item::Gun) == 0, "conta nova: kit inicial (adaga + 3 cocos) e 0 moedas (%d)", a.coins);
        loginWait(b, {&a, &c}, LoginFirebase, "", tB);
        loginWait(c, {&a, &b}, LoginLocal, "local-alice0000", "");
        idle({&a, &b, &c}, 300);
        CHECK(b.loginRes == LoginOk && c.loginRes == LoginGuest, "bob verificado (%d) e visitante local (%d)", b.loginRes, c.loginRes);
        // moedas: anúncio premiado com limite de taxa, recibo (anti-duplicação), cheat
        a.act(ActAdReward, 1, 0); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 1 && a.coins == kAdRewardCoins, "anuncio premiado: +%d (moedas %d)", kAdRewardCoins, a.coins);
        a.act(ActAdReward, 1, 0); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 0 && a.coins == kAdRewardCoins, "2o anuncio logo em seguida: recusado pelo limite de taxa (moedas %d)", a.coins);
        a.purchase("coins_500|test-recibo-123456"); idle({&a, &b, &c}, 300);
        CHECK(a.accRes == PurchCredited && a.accCoins == 500 && a.coins == kAdRewardCoins + 500, "recibo creditado: +500 (moedas %d)", a.coins);
        a.purchase("coins_500|test-recibo-123456"); idle({&a, &b, &c}, 300);
        CHECK(a.accRes == PurchDuplicate && a.coins == kAdRewardCoins + 500, "mesmo recibo de novo: NAO credita (res %d, moedas %d)", a.accRes, a.coins);
        b.purchase("coins_500|test-recibo-123456"); idle({&a, &b, &c}, 300);
        CHECK(b.accRes == PurchDuplicate && b.coins == 0, "mesmo recibo em OUTRA conta: NAO credita (res %d, moedas %d)", b.accRes, b.coins);
        a.purchase("coins_999999|test-recibo-x9"); idle({&a, &b, &c}, 300);
        CHECK(a.accRes == PurchRejected, "sku desconhecido recusado (%d)", a.accRes);
        c.purchase("coins_500|test-recibo-777777"); idle({&a, &b, &c}, 300);
        CHECK(c.accRes == PurchCredited && c.coins == 500, "visitante tambem compra (%d)", c.coins);
        // inventário: craft, mover, soltar, usar (o kit novo é só adaga + 3 cocos: a munição vem do cheat e cai no slot 0 da mochila)
        a.debugItems(item::Ammo, 20); idle({&a, &b, &c}, 250);
        CHECK(a.inv[0].id == item::Ammo && a.inv[0].n == 20, "cheat: 20 municoes no slot 0 (id %d n %d)", a.inv[0].id, a.inv[0].n);
        a.debugItems(item::Frond, 3); idle({&a, &b, &c}, 250);
        a.act(ActCraft, 0, 0); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 1 && a.count(item::Rope) == 1 && a.count(item::Frond) == 0, "craft no servidor: 3 folhas -> 1 corda (corda %d, folhas %d, res %d)", a.count(item::Rope), a.count(item::Frond), a.lastActRes);
        a.act(ActCraft, 0, 0); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 0 && a.count(item::Rope) == 1, "ADULTERACAO: craft sem ingredientes recusado");
        a.act(ActInvMove, (uint16_t)(kInvHotFirst | (3 << 8)), 0); idle({&a, &b, &c}, 300);
        CHECK(a.inv[3].id == item::Dagger && a.inv[kInvHotFirst].id == 0, "mover adaga do acesso rapido para a mochila (slot 3 = %d, slot 25 = %d, res %d)", a.inv[3].id, a.inv[kInvHotFirst].id, a.lastActRes);
        a.act(ActDrop, (uint16_t)(item::Ammo | (1 << 8)), 5); idle({&a, &b, &c}, 400);
        CHECK(a.lastActRes == 1 && a.count(item::Ammo) == 15, "soltou 5 municoes do slot 0 (sobraram %d)", a.count(item::Ammo));
        a.act(ActDrop, (uint16_t)item::Diamond, 1); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 0, "ADULTERACAO: soltar diamante que nao tem e recusado");
        a.act(ActDrop, (uint16_t)(item::Ammo | (1 << 8)), 500); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 0 && a.count(item::Ammo) == 15, "ADULTERACAO: soltar 500 municoes tendo 15 e recusado (%d)", a.count(item::Ammo));
        a.act(ActUse, 0, item::Coconut); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 1 && a.count(item::Coconut) == 2, "comer coco do kit gasta 1 (sobraram %d)", a.count(item::Coconut));
        a.act(ActUse, 0, item::PotHeal); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 0, "ADULTERACAO: beber pocao que nao tem e recusado");
        a.act(ActUse, 0, item::Ammo); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 1 && a.count(item::Ammo) == 14, "recarregar gasta 1 municao (%d)", a.count(item::Ammo));
        a.act(ActGive, (uint16_t)item::Gold, 3); idle({&a, &b, &c}, 300);
        CHECK(a.lastActRes == 0, "ADULTERACAO: entregar ouro que nao tem e recusado");
        { uint8_t pk[32]; Writer w(pk, sizeof pk); w.header(MsgAct); w.put<uint32_t>(++a.actSeq); w.put<uint8_t>(ActPickupDrop); w.put<uint16_t>(60000); w.put<uint16_t>(0); a.send(pk, w.size()); }
        idle({&a, &b, &c}, 200);
        CHECK(a.count(item::Diamond) == 0 && a.coins == kAdRewardCoins + 500, "inventario intacto depois das adulteracoes");
        a.vitals(77, 66); a.profile(kTestFlag); idle({&a, &b, &c}, 300);
        {   // MIGRAÇÃO 1x: visitante local -> conta nova do Firebase (prevLocal)
            Cli g(port); CHECK(joinWait(g), "G entrou");
            loginWait(g, {&a, &b, &c}, LoginLocal, "local-migra00001", "");
            g.act(ActAdReward, 1, 0); g.debugItems(item::Gold, 4); idle({&a, &b, &c, &g}, 400);
            CHECK(g.coins == kAdRewardCoins && g.count(item::Gold) == 4, "visitante G juntou %d moedas e 4 ouros", g.coins);
            g.leave(); idle({&a, &b, &c}, 300);
            Cli h(port); CHECK(joinWait(h), "H entrou");
            loginWait(h, {&a, &b, &c}, LoginFirebase, "", readTxt(d + "/token_carol.txt"), "local-migra00001");
            idle({&a, &b, &c, &h}, 600);
            CHECK(h.loginRes == LoginOk && h.coins == kAdRewardCoins && h.count(item::Gold) == 4, "conta nova do Firebase (carol) recebeu os dados do visitante (moedas %d, ouro %d)", h.coins, h.count(item::Gold));
            h.leave(); idle({&a, &b, &c}, 200);
            Cli g2(port); joinWait(g2); loginWait(g2, {&a, &b, &c}, LoginLocal, "local-migra00001", ""); idle({&a, &b, &c, &g2}, 300);
            CHECK(g2.coins == 0 && g2.count(item::Gold) == 0, "o visitante antigo ficou vazio (sem duplicar: moedas %d)", g2.coins);
            g2.leave(); idle({&a, &b, &c}, 200);
        }
        // mesma conta em 2 sessões: a nova derruba a antiga e herda tudo
        CHECK(joinWait(e), "E entrou");
        loginWait(e, {&a, &b, &c}, LoginFirebase, "", tA);
        idle({&a, &b, &c, &e}, 400);
        CHECK(e.loginRes == LoginOk && a.rejectReason == 2, "alice entrou de novo em E: a sessao A foi derrubada (MsgReject %d)", a.rejectReason);
        CHECK(e.coins == kAdRewardCoins + 500 && e.count(item::Rope) == 1 && e.count(item::Ammo) == 14 && e.inv[3].id == item::Dagger && e.count(item::Coconut) == 2, "E herdou moedas (%d) e inventario de A", e.coins);
        CHECK(e.loginHealth == 77 && e.loginHunger == 66 && std::memcmp(e.loginFlag, kTestFlag, kFlagBytes) == 0 && (e.loginFlags & LoginHasFlag), "E recebeu vida/fome (%d/%d) e a bandeira", e.loginHealth, e.loginHunger);
        CHECK(b.coins == 0 && c.coins == 500, "contas diferentes nao se misturam (bob %d, visitante %d)", b.coins, c.coins);
        e.leave(); b.leave(); c.leave(); idle({}, 300);
    } else {
        Cli e(port), b(port), c(port);
        CHECK(joinWait(e) && joinWait(b) && joinWait(c), "E, B e C entraram depois do REINICIO do servidor");
        idle({&e, &b, &c}, 300);
        CHECK(!e.drops.empty() && e.drops.front().item == item::Ammo && e.drops.front().n == 5, "itens soltos no chao sobreviveram ao reinicio (%zu, item %d x%d)", e.drops.size(), e.drops.empty() ? -1 : e.drops.front().item, e.drops.empty() ? 0 : e.drops.front().n);
        loginWait(e, {&b, &c}, LoginFirebase, "", tA);
        loginWait(b, {&e, &c}, LoginFirebase, "", tB);
        loginWait(c, {&e, &b}, LoginLocal, "local-alice0000", "");
        idle({&e, &b, &c}, 400);
        CHECK(e.loginRes == LoginOk && !(e.loginFlags & LoginNewAccount), "alice voltou (conta existente) res=%d flags=%d", e.loginRes, e.loginFlags);
        CHECK(e.coins == kAdRewardCoins + 500, "MOEDAS sobreviveram ao reinicio (%d)", e.coins);
        CHECK(e.count(item::Rope) == 1 && e.count(item::Ammo) == 14 && e.count(item::Coconut) == 2 && e.inv[3].id == item::Dagger && e.inv[kInvHotFirst].id == 0, "INVENTARIO sobreviveu (corda %d, municao %d, layout)", e.count(item::Rope), e.count(item::Ammo));
        CHECK(e.loginHealth == 77 && e.loginHunger == 66 && std::memcmp(e.loginFlag, kTestFlag, kFlagBytes) == 0, "vida/fome (%d/%d) e bandeira sobreviveram", e.loginHealth, e.loginHunger);
        CHECK((e.loginFlags & LoginRestoredPos) != 0, "voltou na ultima posicao salva (flags %d)", e.loginFlags);
        CHECK(b.coins == 0 && b.count(item::Dagger) == 1 && b.count(item::Coconut) == 3 && c.coins == 500, "outras contas intactas (bob %d, visitante %d)", b.coins, c.coins);
        e.purchase("coins_500|test-recibo-123456"); idle({&e, &b, &c}, 300);
        CHECK(e.accRes == PurchDuplicate && e.coins == kAdRewardCoins + 500, "recibo antigo depois do reinicio: NAO credita de novo (res %d)", e.accRes);
        // MORTE: perde tudo MENOS a adaga (fica 1 no 1º slot do acesso rápido); as moedas ficam
        e.debugItems(item::Gold, 3); e.debugItems(item::Dagger, 1); idle({&e, &b, &c}, 300);   // 2 adagas: sobra só 1
        CHECK(e.count(item::Dagger) == 2 && e.count(item::Gold) == 3, "antes de morrer: 2 adagas, ouro, corda, municao, cocos (adagas %d)", e.count(item::Dagger));
        e.act(ActDie, 0, 0); idle({&e, &b, &c}, 300);
        { int other = 0; for (int i = 0; i < kInvSlots; ++i) if (e.inv[i].id && e.inv[i].id != item::Dagger) other += e.inv[i].n;
          CHECK(other == 0 && e.count(item::Dagger) == 1 && e.inv[kInvHotFirst].id == item::Dagger && e.inv[kInvHotFirst].n == 1 && e.coins == kAdRewardCoins + 500,
                "morte COM adaga: sobra so 1 adaga no acesso rapido 1 (outros itens %d, adagas %d), moedas ficam (%d)", other, e.count(item::Dagger), e.coins); }
        e.respawn(); idle({&e, &b, &c}, 400);
        e.act(ActDrop, (uint16_t)(item::Dagger | ((kInvHotFirst + 1) << 8)), 1); idle({&e, &b, &c}, 300);
        e.debugItems(item::Wood, 2); idle({&e, &b, &c}, 300);
        CHECK(e.count(item::Dagger) == 0 && e.count(item::Wood) == 2, "renasceu, soltou a adaga e pegou 2 madeiras (adagas %d, madeira %d)", e.count(item::Dagger), e.count(item::Wood));
        e.act(ActDie, 0, 0); idle({&e, &b, &c}, 300);
        { int tot = 0; for (int i = 0; i < kInvSlots; ++i) tot += e.inv[i].n;
          CHECK(tot == 0 && e.coins == kAdRewardCoins + 500, "morte SEM adaga: inventario vazio (%d itens), moedas ficam (%d)", tot, e.coins); }
        e.leave(); b.leave(); c.leave(); idle({}, 300);
    }
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}

// TRIPULAÇÃO no jogo (tools/team_test.sh, depois do "teamtest after"): alice (Firebase) é a capitã de "Corsarios do Sul"; o marujo e o
// capitão antigo (visitantes locais) estão sem tripulação. Bandeira por id (MsgTeamQuery), convite, recusar, aceitar, já tem tripulação.
struct Who2 { uint8_t kind; std::string uid, token, name; };

// DOMINAÇÃO DE ILHAS (servidor com mundo + ilhas + --data + --debug --cap-time-scale 0.05 --payout-sec 4; tripulações de `teamtest conqseed`):
// ilha livre sobe sem luta, protegida depois de dominada, contestada com defensor, arriar + hastear toma a ilha, avisos, renda e quem não tem tripulação não conta.
// Fase 2 (`conquest2`, depois de REINICIAR o servidor): a dona continua a mesma.
static int conquestTests(uint16_t port, const char* worldPath, int phase) {
    World mw; CHECK(World::load(worldPath, mw), "mundo carregado (%s)", worldPath);
    int slot = -1; float fx = 0, fz = 0;
    for (int sx = -1; sx <= 1 && slot < 0; ++sx) for (int sz = -1; sz <= 1 && slot < 0; ++sz) {
        if (!sx && !sz) continue;
        IslandSpec sp = islandSpec(mw.seed, sx, sz); if (!sp.exists) continue;
        World w; if (!World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, w)) continue;
        int32_t xc, zc; if (!conquestPoint(w, xc, zc)) continue;
        slot = sp.slot; fx = xc * 0.01f; fz = zc * 0.01f;
    }
    CHECK(slot > 0, "ilha vizinha com mastro: slot %d em (%.1f, %.1f)", slot, fx, fz);
    if (slot <= 0) return 1;
    Cli a(port), b(port), c(port);
    CHECK(joinWait(a, 3000) && joinWait(b, 3000) && joinWait(c, 3000), "3 jogadores entraram");
    loginWait(a, {&b, &c}, LoginLocal, "local-conqazul01", "");
    loginWait(b, {&a, &c}, LoginLocal, "local-conqverm01", "");
    loginWait(c, {&a, &b}, LoginLocal, "local-conqsolo01", "");
    if (phase == 2) {
        idle({&a, &b, &c}, 600);
        CHECK(a.ownersMsgs > 0 && a.isleOwner[slot] == 61, "depois de REINICIAR: a ilha continua dos VERMELHOS (%u)", a.isleOwner[slot]);
        a.leave(); b.leave(); c.leave(); idle({}, 200);
        std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails); return fails ? 1 : 0;
    }
    CHECK(a.ownersMsgs > 0 && a.isleOwner[slot] == 0, "no Welcome veio a lista de ilhas dominadas (nenhuma)");
    // sem tripulação não domina
    c.teleport(fx, fz); a.teleport(fx + 400, fz); b.teleport(fx + 420, fz); idle({&a, &b, &c}, 2500);
    CHECK(c.cap[slot].msgs > 0 && c.cap[slot].pole == 0 && c.cap[slot].nAtt == 0 && a.cap[slot].msgs == 0, "quem não tem tripulação não conta (e quem está longe não recebe o estado do mastro)");
    // ilha livre: sobe direto (2 s com escala 0,05)
    a.teleport(fx + 2, fz); idle({&a, &b, &c}, 700);
    CHECK(a.cap[slot].state == CapRaising && a.cap[slot].pole == 60 && a.cap[slot].prog > 0.05f, "AZUIS hasteando a bandeira (%.2f)", a.cap[slot].prog);
    idle({&a, &b, &c}, 2600);
    CHECK(a.isleOwner[slot] == 60 && a.cap[slot].state == CapShielded && a.cap[slot].owner == 60, "ilha DOMINADA pelos AZUIS e protegida (estado %d)", a.cap[slot].state);
    CHECK(c.lastIev(IeCaptured) && c.lastIev(IeCaptured)->value == 60 && c.lastIev(IeCaptured)->name == "AZUIS", "todos ficam sabendo (IeCaptured)");
    // protegida: os VERMELHOS chegam e nada muda
    b.teleport(fx - 2, fz); idle({&a, &b, &c}, 1500);
    CHECK(b.cap[slot].state == CapShielded && b.cap[slot].pole == 60 && b.cap[slot].prog > 0.99f, "recém-dominada: protegida (%d s)", b.cap[slot].shield);
    // fim da proteção (15 s): com o defensor no mastro = CONTESTADA
    for (int k = 0; k < 40 && b.cap[slot].state == CapShielded; ++k) idle({&a, &b, &c}, 500);
    idle({&a, &b, &c}, 600);
    CHECK(b.cap[slot].state == CapContested && b.cap[slot].nAtt == 1 && b.cap[slot].nDef == 1 && b.cap[slot].prog > 0.99f, "CONTESTADA: defensor na área, nada anda (estado %d)", b.cap[slot].state);
    CHECK(a.lastIev(IeUnderAttack) && a.lastIev(IeUnderAttack)->name == "VERMELHOS", "a dona é avisada do ataque");
    // o defensor sai: arriar (1,5 s) e hastear (2 s)
    a.teleport(fx + 400, fz); idle({&a, &b, &c}, 600);
    CHECK(b.cap[slot].state == CapLowering && b.cap[slot].pole == 60 && b.cap[slot].prog < 0.99f, "sem defensor: arriando a bandeira dos AZUIS (%.2f)", b.cap[slot].prog);
    idle({&a, &b, &c}, 3600);
    CHECK(b.isleOwner[slot] == 61 && c.lastIev(IeCaptured) && c.lastIev(IeCaptured)->value == 61, "os VERMELHOS TOMARAM a ilha (%u)", b.isleOwner[slot]);
    CHECK(a.lastIev(IeLost) && a.lastIev(IeLost)->name == "VERMELHOS", "os AZUIS recebem PERDEMOS");
    // renda (servidor com --payout-sec 4)
    a.iev.clear(); b.iev.clear(); idle({&a, &b, &c}, 9000);
    CHECK(b.lastIev(IeIncome) && b.lastIev(IeIncome)->value > 0 && !a.lastIev(IeIncome), "a ilha rendeu %d moedas para o cofre dos VERMELHOS (e nada para os AZUIS)", b.lastIev(IeIncome) ? b.lastIev(IeIncome)->value : 0);
    a.leave(); b.leave(); c.leave(); idle({}, 300);
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}

// MELHORIAS DA ILHA (rodar DEPOIS de `conquest`, no mesmo servidor: os VERMELHOS são donos da ilha). Fase 2 (`isle2`, depois de REINICIAR): a estrutura continua.
static int isleTests(uint16_t port, const char* worldPath, int phase) {
    World mw; CHECK(World::load(worldPath, mw), "mundo carregado");
    int slot = -1; World isl; float px = 0, pz = 0;
    for (int sx = -1; sx <= 1 && slot < 0; ++sx) for (int sz = -1; sz <= 1 && slot < 0; ++sz) {
        if (!sx && !sz) continue;
        IslandSpec sp = islandSpec(mw.seed, sx, sz); if (!sp.exists) continue;
        World w; if (!World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, w)) continue;
        int32_t xc, zc; if (!conquestPoint(w, xc, zc)) continue;
        slot = sp.slot; isl = w; px = xc * 0.01f; pz = zc * 0.01f;
    }
    if (slot <= 0) { CHECK(false, "ilha com mastro"); return 1; }
    terrain().setMain(&mw); terrain().setIsland(slot, &isl);
    Cli a(port), b(port);
    CHECK(joinWait(a, 3000) && joinWait(b, 3000), "2 jogadores entraram");
    loginWait(a, {&b}, LoginLocal, "local-conqazul01", "");
    loginWait(b, {&a}, LoginLocal, "local-conqverm01", "");
    idle({&a, &b}, 800);
    if (phase == 2) {
        CHECK(b.structs[slot].size() == 1 && b.structs[slot][0].kind == SkWorkshop, "depois de REINICIAR: a MARCENARIA continua na ilha (%zu)", b.structs[slot].size());
        a.leave(); b.leave(); idle({}, 200);
        std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails); return fails ? 1 : 0;
    }
    CHECK(b.isleOwner[slot] == 61, "a ilha é dos VERMELHOS (%u)", b.isleOwner[slot]);
    // lugares válidos para a paliçada e a marcenaria (a mesma validação do servidor)
    auto findSpot = [&](uint8_t kind, float& ox, float& oz) {
        for (float r = 8.0f; r < 90.0f; r += 2.0f) for (int k = 0; k < 24; ++k) {
            const float an = 6.2831853f * k / 24.0f, x = px + std::cos(an) * r, z = pz + std::sin(an) * r; float y;
            if (structCheckPlace(isl, slot, kind, 0, x, z, px, pz, y) == SrOk) { ox = x; oz = z; return true; }
        }
        return false;
    };
    float mx = 0, mz = 0;
    a.teleport(px + 2, pz + 2); b.teleport(px - 2, pz - 2); idle({&a, &b}, 500);
    a.structReq(1, slot, SkWall, 0, 0, 0, 0); idle({&a, &b}, 400);
    CHECK(a.structRes == SrNotOwner, "quem não é da tripulação dona não constrói (%d)", a.structRes);
    b.structReq(1, slot, SkWall, 0, 0, 0, 0); idle({&a, &b}, 400);
    CHECK(b.structRes == SrCost, "sem materiais na mochila: recusado (%d)", b.structRes);
    b.debugItems(item::Stone, 2); b.debugItems(item::Rope, 4); idle({&a, &b}, 200); b.debugItems(item::Wood, 22); idle({&a, &b}, 500);   // a mochila tem limite de peso
    const int wood0 = b.count(item::Wood);
    b.structReq(1, slot, SkWall, 0, 0, 0, 0); idle({&a, &b}, 600);
    CHECK(b.structRes == SrOk && b.structs[slot].size() == 1 && b.structs[slot][0].kind == SkWall && a.structs[slot].size() == 1 && b.count(item::Wood) == wood0 - 20,
          "MURALHA erguida em volta da ilha: todos recebem e gastou 20 madeiras (%d -> %d)", wood0, b.count(item::Wood));
    b.structReq(1, slot, SkWall, 0, 0, 0, 0); idle({&a, &b}, 400);
    CHECK(b.structRes == SrKindLimit || b.structRes == SrSlow, "segunda muralha: recusada (%d)", b.structRes);
    isleStructs(slot) = b.structs[slot];   // a geometria/validação local considera o que já existe
    const IsleWall* W = isleWallOf(slot);
    CHECK(W && W->x.size() >= 12 && W->gate >= 0 && W->p1 > W->p0, "anel calculado: %zu trechos, portão no trecho %d, cais de %.1f m", W ? W->x.size() : 0, W ? W->gate : -1, W ? W->p1 - W->p0 : 0.0f);
    if (W) {
        // trecho de muralha (longe do portão) é sólido
        const int n = (int)W->x.size(), i = (W->gate + n / 2) % n, j = (i + 1) % n;
        const float sx = (W->x[i] + W->x[j]) * 0.5f, sz = (W->z[i] + W->z[j]) * 0.5f;
        a.teleport(sx, sz); idle({&a, &b}, 300);
        for (int k = 0; k < 10; ++k) { a.input(0, 0, 0.033f); idle({&a, &b}, 30); }
        idle({&a, &b}, 300);
        const PlayerState* m = a.me(); float qx = m ? m->x : sx, qz = m ? m->z : sz, ox = qx, oz = qz;
        pushFromSeg(ox, oz, W->x[i], W->z[i], W->x[j], W->z[j], kWallR + kPlayerRadius - 0.02f);
        CHECK(m && std::fabs(ox - qx) + std::fabs(oz - qz) < 1e-3f, "a MURALHA é sólida: jogador empurrado para fora do trecho");
        // o cais é chão: no meio dele o jogador fica no tabuado (não nada)
        const float u = (W->p0 + W->p1) * 0.5f;
        a.teleport(W->gx + W->nx * u, W->gz + W->nz * u); idle({&a, &b}, 300);
        for (int k = 0; k < 10; ++k) { a.input(0, 0, 0.033f); idle({&a, &b}, 30); }
        idle({&a, &b}, 300);
        m = a.me();
        CHECK(m && std::fabs(m->y - kPierDeckY) < 0.2f, "o CAIS é chão: jogador em pé no tabuado (y %.2f)", m ? m->y : -99.0f);
        // navio chamado na ilha com muralha atraca na frente do cais
        b.teleport(W->gx - W->nx * 3.0f, W->gz - W->nz * 3.0f); idle({&a, &b}, 400);
        b.summon(ShipBoat, 2); idle({&a, &b}, 600);
        const float ex = W->gx + W->nx * W->p1, ez = W->gz + W->nz * W->p1;
        const float fwd = (b.boat.x - ex) * W->nx + (b.boat.z - ez) * W->nz;
        CHECK(b.sumRes == SumOk && b.boat.exists && fwd > 20.0f && fwd < 430.0f, "navio chamado ATRACA na frente do CAIS (%.0f m à frente da ponta)", fwd);
        // estrutura em cima do anel é recusada
        float y; const int i2 = (W->gate + n / 4) % n;
        CHECK(structCheckPlace(isl, slot, SkCampfire, 0, W->x[i2], W->z[i2], px, pz, y) == SrOverlap, "fogueira em cima da muralha: recusada");
    }
    b.debugItems(item::Stone, 4); b.debugItems(item::Rope, 2); idle({&a, &b}, 200); b.debugItems(item::Wood, 14); idle({&a, &b}, 500);
    CHECK(findSpot(SkWorkshop, mx, mz), "achou lugar para a marcenaria");
    a.teleport(px + 60, pz + 60); b.teleport(px - 2, pz - 2); idle({&a, &b}, 400);
    b.structReq(1, slot, SkWorkshop, 0, mx, mz, 0); idle({&a, &b}, 500);
    CHECK(b.structRes == SrOk && b.structs[slot].size() == 2, "MARCENARIA erguida (%d)", b.structRes);
    uint16_t wid = 0; for (const auto& q : b.structs[slot]) if (q.kind == SkWall) wid = q.id;
    const int w1 = b.count(item::Wood);
    b.structReq(2, slot, 0, 0, 0, 0, wid); idle({&a, &b}, 500);
    CHECK(b.structRes == SrOk && b.structs[slot].size() == 1 && b.count(item::Wood) == w1 + 10, "DEMOLIU a muralha: devolveu 10 madeiras (%d -> %d)", w1, b.count(item::Wood));
    a.leave(); b.leave(); idle({}, 300);
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}
static int teamInviteTests(uint16_t port, const std::string& d) {
    Cli a(port), b(port), c(port);
    CHECK(joinWait(a, 3000) && joinWait(b, 3000) && joinWait(c, 3000), "3 jogadores entraram (%d, %d, %d)", a.id, b.id, c.id);
    loginWait(a, {&b, &c}, LoginFirebase, "", readTxt(d + "/token_alice.txt"));
    loginWait(b, {&a, &c}, LoginLocal, "local-teammarujo01", "");
    loginWait(c, {&a, &b}, LoginLocal, "local-teamcapitao01", "");
    CHECK(a.loginRes == LoginOk && b.loginRes == LoginGuest && c.loginRes == LoginGuest, "logins (%d %d %d)", a.loginRes, b.loginRes, c.loginRes);
    a.teleport(0, 0); b.teleport(2, 0); c.teleport(4, 0); idle({&a, &b, &c}, 300);   // perto (o nascimento em espiral pode deixá-los a > 10 m)
    a.teamQuery({(uint16_t)a.id, (uint16_t)b.id}); idle({&a, &b, &c}, 400);
    CHECK(a.tmKnown[a.id] && a.tmTeam[a.id] != 0 && a.tmKnown[b.id] && a.tmTeam[b.id] == 0 && a.tmFlag[a.id][0] == 2, "bandeira por id: alice tem tripulação (%u), marujo não", a.tmTeam[a.id]);
    b.teamInvite((uint16_t)c.id); idle({&a, &b, &c}, 400);
    CHECK(b.lastNotice(TnInviteSent) && b.lastNotice(TnInviteSent)->result == TeamErrNoTeam && c.offers == 0, "sem tripulação não convida");
    a.teamInvite((uint16_t)b.id, 3); idle({&a, &b, &c}, 500);
    CHECK(a.lastNotice(TnInviteSent) && a.lastNotice(TnInviteSent)->result == TeamOk && b.offers == 1 && b.offerFrom == a.id && b.offerName == "Corsarios do Sul", "convite chegou (3 cópias = 1 convite, '%s')", b.offerName.c_str());
    a.teamInvite((uint16_t)c.id); idle({&a, &b, &c}, 300);
    CHECK(a.lastNotice(TnInviteSent)->result == TeamErrSlow && c.offers == 0, "segundo convite em < 2 s recusado");
    b.teamReply(b.offerId, false); idle({&a, &b, &c}, 400);
    CHECK(a.lastNotice(TnDeclined) && a.lastNotice(TnDeclined)->other == b.id, "recusou: a capitã é avisada");
    idle({&a, &b, &c}, 1800);
    a.teamInvite((uint16_t)b.id); idle({&a, &b, &c}, 500);
    CHECK(b.offers == 2, "novo convite (%d)", b.offers);
    b.teamReply(b.offerId, true); b.teamReply(b.offerId, true); idle({&a, &b, &c}, 500);
    CHECK(b.lastNotice(TnJoined) && b.lastNotice(TnJoined)->result == TeamOk && a.lastNotice(TnAccepted) && a.lastNotice(TnAccepted)->other == b.id, "aceitou: entrou e a capitã foi avisada");
    CHECK(a.tmKnown[b.id] && a.tmTeam[b.id] == a.tmTeam[a.id], "a bandeira do marujo atualizou na hora para quem está perto");
    idle({&a, &b, &c}, 2100);
    a.teamInvite((uint16_t)b.id); idle({&a, &b, &c}, 400);
    CHECK(a.lastNotice(TnInviteSent)->result == TeamErrTargetTeam, "quem já tem tripulação não é convidado (r %d)", a.lastNotice(TnInviteSent)->result);
    c.teamReply(12345, true); idle({&a, &b, &c}, 300);
    CHECK(c.lastNotice(TnOfferGone) != nullptr, "aceitar convite inexistente: avisa que não vale mais");
    // ---- COOP: posições dos tripulantes, chat chegando no jogo, fogo amigo, passar o cargo, PUSH para quem não está no jogo ----
    idle({&a, &b, &c}, 1300);
    CHECK(a.tmPosMsgs > 0 && a.tmPos[b.id] && b.tmPos[a.id] && !a.tmPos[c.id] && c.tmPosMsgs == 0, "tripulantes recebem a posição um do outro (e só eles)");
    auto tcall = [&](const Who2& w, uint8_t op, const std::string& text, uint8_t idx, TeamView& v) {
        TeamRequest q; q.authKind = w.kind; q.uid = w.uid; q.token = w.token; q.playerName = w.name; q.op = op; q.text = text; q.index = idx;
        uint8_t res = 0; if (!teamCall("127.0.0.1", port, q, res, v, 4000)) return 0; return (int)res;
    };
    const Who2 WA{LoginFirebase, "", readTxt(d + "/token_alice.txt"), "Alice"}, WB{LoginLocal, "local-teammarujo01", "", "Marujo"};
    TeamView tv;
    CHECK(tcall(WA, TeamChat, "Todos ao navio", 0, tv) == TeamOk, "Alice manda mensagem pela tela da tripulação");
    idle({&a, &b, &c}, 400);
    CHECK(b.crewChats >= 1 && b.crewChatLast == "Todos ao navio" && c.crewChats == 0, "o marujo (no jogo) recebe a mensagem na hora; quem não é da tripulação não ('%s')", b.crewChatLast.c_str());
    // fogo amigo: alice soca o marujo à frente = nada; o capitão antigo (fora da tripulação) soca = dano
    auto punch = [&](Cli& x) { x.input(0, 0, 0.03f, 1, (uint8_t)(ActPunch << InputActShift)); };
    a.teleport(0, 0); b.teleport(0, -1.0f); c.teleport(30, 30); idle({&a, &b, &c}, 300);
    const int d0 = b.dmgGot;
    punch(a); idle({&a, &b, &c}, 700);
    CHECK(b.dmgGot == d0, "SEM FOGO AMIGO: tripulante não leva soco de tripulante (%d)", b.dmgGot - d0);
    a.teleport(30, -30); c.teleport(0, 0); idle({&a, &b, &c}, 300);
    punch(c); idle({&a, &b, &c}, 700);
    CHECK(b.dmgGot == d0 + 1, "quem é de fora continua acertando (%d)", b.dmgGot - d0);
    // DAR para UMA pessoa (menu de segurar): o marujo está mais perto, mas o item vai para quem foi escolhido
    {
        a.teleport(0, 0); b.teleport(0.8f, 0); c.teleport(2.5f, 0); idle({&a, &b, &c}, 400);
        int slot = -1; for (int k = 0; k < kInvSlots; ++k) if (a.inv[k].id) { slot = k; break; }
        if (slot >= 0) {
            const uint8_t it = a.inv[slot].id; const int before = invCount(c.inv, it), bb = invCount(b.inv, it);
            a.act(ActGiveTo, (uint16_t)(slot | (c.id << 5)), 1); idle({&a, &b, &c}, 600);
            CHECK(a.lastActRes == 1 && invCount(c.inv, it) == before + 1 && invCount(b.inv, it) == bb, "DAR PARA: o item foi para o escolhido, não para o mais perto");
            c.teleport(40, 40); idle({&a, &b, &c}, 300);
            int s2 = -1; for (int k = 0; k < kInvSlots; ++k) if (a.inv[k].id) { s2 = k; break; }
            if (s2 >= 0) { a.act(ActGiveTo, (uint16_t)(s2 | (c.id << 5)), 1); idle({&a, &b, &c}, 500); CHECK(a.lastActRes == 0, "DAR PARA alguém longe é recusado"); }
        } else CHECK(false, "alice sem item para o teste de DAR");
    }
    // passar o cargo de capitão (a lista: capitã primeiro, o marujo é o índice 1)
    CHECK(tcall(WB, TeamPromote, "Marujo", 1, tv) == TeamErrNotOwner, "quem não é capitão não passa o cargo");
    CHECK(tcall(WA, TeamPromote, "Outro Nome", 1, tv) == TeamErrBadReq, "nome errado: não passa o cargo (a lista mudou)");
    CHECK(tcall(WA, TeamPromote, "Marujo", 1, tv) == TeamOk && !tv.owner, "Alice passou o cargo para o marujo");
    CHECK(tcall(WB, TeamGet, "", 0, tv) == TeamOk && tv.owner, "o marujo agora é o capitão");
    // PUSH: o marujo registra o token do aparelho e sai do jogo; a mensagem vira notificação (servidor com --fcm-cmd)
    CHECK(tcall(WB, TeamPushToken, "tok-marujo_123:abc", 0, tv) == TeamOk, "token do push registrado");
    CHECK(tcall(WB, TeamPushToken, "token com espaco", 0, tv) == TeamErrBadReq, "token inválido recusado");
    b.leave(); idle({&a, &c}, 400);
    CHECK(tcall(WA, TeamChat, "Cade voce?", 0, tv) == TeamOk, "Alice manda outra mensagem com o marujo fora do jogo");
    idle({&a, &c}, 1100);
    CHECK(tcall(WA, TeamChat, "Responde!", 0, tv) == TeamOk, "e mais uma (junta na mesma notificação)");
    idle({&a, &c}, 3500);
    if (const char* pl = std::getenv("NETTEST_PUSHLOG")) {
        const std::string log = readTxt(pl);
        CHECK(log.find("tok-marujo_123:abc") != std::string::npos && log.find("2 novas mensagens") != std::string::npos && log.find("Alice: Responde!") != std::string::npos,
              "PUSH saiu para o ajudante: 1 notificação juntando as 2 mensagens (%zu bytes no log)", log.size());
    }
    a.leave(); c.leave(); idle({}, 300);
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}

// CONTA num servidor qualquer (produção: NETTEST_HOST=ip build/nettest 7777 acct1 local-xxxx): fase 1 entra como visitante local e pede o
// crédito do anúncio (+50, limite de taxa); fase 2 (depois de reiniciar o servidor) confere que a conta voltou com as moedas.
static int acctTests(uint16_t port, int phase, const char* uid, int expect) {
    Cli a(port);
    CHECK(joinWait(a, 3000), "entrou no servidor (id %d)", a.id);
    loginWait(a, {}, LoginLocal, uid, "");
    idle({&a}, 800);
    CHECK(a.loginRes == LoginGuest, "login do visitante %s (res %d flags %d, moedas %d)", uid, a.loginRes, a.loginFlags, a.coins);
    if (phase == 1) { a.act(ActAdReward, 1, 0); idle({&a}, 800); std::printf("moedas agora: %d\n", a.coins); CHECK(a.coins >= kAdRewardCoins, "credito do anuncio (%d)", a.coins); }
    else CHECK(!(a.loginFlags & LoginNewAccount) && a.coins == expect, "conta voltou depois do reinicio (moedas %d, esperado %d, nova=%d)", a.coins, expect, (a.loginFlags & LoginNewAccount) ? 1 : 0);
    a.leave(); idle({}, 300);
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}

// ADMINS: servidor SEM --debug, com --data DIR --jwk DIR_JWT/test.jwk --admins ARQ (tools/admin_test.sh). Só a conta fb:<uid> com idToken
// VERIFICADO e listada em ARQ usa os comandos de debug; o arquivo é relido quando muda (até ~2 s).
static void writeAdmins(const std::string& path, const char* text) {
    if (!text) { std::remove(path.c_str()); return; }
    const std::string tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) { std::fputs(text, f); std::fclose(f); std::rename(tmp.c_str(), path.c_str()); }
}
static int adminTests(uint16_t port, const char* jdir, const char* adminsPath) {
    const std::string d = jdir, ap = adminsPath;
    const std::string tA = readTxt(d + "/token_alice.txt"), tB = readTxt(d + "/token_bob.txt");
    CHECK(!tA.empty() && !tB.empty(), "tokens de teste lidos de %s", d.c_str());
    writeAdmins(ap, nullptr);
    Cli a(port), b(port), c(port), e(port);
    CHECK(joinWait(a) && joinWait(b) && joinWait(c) && joinWait(e), "A, B, C e E entraram");
    std::vector<Cli*> all = {&a, &b, &c, &e};
    loginWait(a, {&b, &c, &e}, LoginFirebase, "", tA);
    loginWait(b, {&a, &c, &e}, LoginFirebase, "", tB);
    loginWait(c, {&a, &b, &e}, LoginLocal, "local-visit0001", "");
    idle(all, 300);
    CHECK(a.loginRes == LoginOk && b.loginRes == LoginOk && c.loginRes == LoginGuest, "alice e bob verificados, C visitante (%d %d %d)", a.loginRes, b.loginRes, c.loginRes);
    // E tenta se passar pela alice: token adulterado, expirado e de outro projeto (o uid "alice" vai no campo uid também)
    { std::string x = tA; x[x.size() - 4] = x[x.size() - 4] == 'A' ? 'B' : 'A'; loginWait(e, {&a, &b, &c}, LoginFirebase, "alice", x); }
    CHECK(e.loginRes == LoginInvalid, "E com token adulterado da alice -> LoginInvalid (%d)", e.loginRes);
    loginWait(e, {&a, &b, &c}, LoginFirebase, "alice", readTxt(d + "/token_alice_expirado.txt"));
    CHECK(e.loginRes == LoginExpired, "E com token expirado da alice -> LoginExpired (%d)", e.loginRes);
    loginWait(e, {&a, &b, &c}, LoginFirebase, "alice", readTxt(d + "/token_alice_outroaud.txt"));
    CHECK(e.loginRes == LoginInvalid, "E com token de outro projeto -> LoginInvalid (%d)", e.loginRes);

    auto tryCheats = [&](Cli& x, const char* who, bool expect) {
        const int c0 = x.coins, d0 = x.count(item::Diamond);
        x.debugCoins(100); x.debugItems(item::Diamond, 2); idle(all, 350);
        const bool gotCoins = x.coins == c0 + 100, gotItems = x.count(item::Diamond) == d0 + 2;
        if (expect) CHECK(gotCoins && gotItems, "%s: cheats de moedas/itens VALEM (moedas %d -> %d, diamantes %d -> %d)", who, c0, x.coins, d0, x.count(item::Diamond));
        else CHECK(x.coins == c0 && x.count(item::Diamond) == d0, "%s: cheats de moedas/itens IGNORADOS (moedas %d -> %d, diamantes %d -> %d)", who, c0, x.coins, d0, x.count(item::Diamond));
        const PlayerState* m0 = x.me(); const float x0 = m0 ? m0->x : 0, z0 = m0 ? m0->z : 0;
        x.teleport(x0 + 60.0f, z0); idle(all, 300);
        const PlayerState* m1 = x.me(); const bool moved = m1 && std::fabs(m1->x - (x0 + 60.0f)) < 3.0f;
        if (expect) CHECK(moved, "%s: teleporte VALE (x %.1f -> %.1f)", who, x0, m1 ? m1->x : 0.0f);
        else CHECK(m1 && std::fabs(m1->x - x0) < 5.0f, "%s: teleporte IGNORADO (x %.1f -> %.1f)", who, x0, m1 ? m1->x : 0.0f);
        if (moved) { x.teleport(x0, z0); idle(all, 300); }
        const float target = std::fabs(x.cycle - 1350.0f) > 300.0f ? 1350.0f : 450.0f;
        { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgDebugTime); w.put<float>(target); x.send(pk, w.size()); }
        idle(all, 300);
        if (expect) CHECK(std::fabs(x.cycle - target) < 3.0f, "%s: troca de hora VALE (%.0f)", who, x.cycle);
        else CHECK(std::fabs(x.cycle - target) > 100.0f, "%s: troca de hora IGNORADA (%.0f, pedido %.0f)", who, x.cycle, target);
        const PlayerState* f0 = x.me(); const float y0 = f0 ? f0->y : 0;
        for (int i = 0; i < 20; ++i) { x.input(0, 0, 1.0f / 30, 1, (uint8_t)(InputFly | InputJump)); for (Cli* o : all) o->pump(); usleep(33000); }
        idle(all, 150);
        const PlayerState* f1 = x.me(); const float y1 = f1 ? f1->y : 0;
        if (expect) CHECK(y1 > y0 + 2.0f, "%s: VOAR vale (y %.2f -> %.2f)", who, y0, y1);
        else CHECK(y1 < y0 + 2.0f, "%s: VOAR ignorado (y %.2f -> %.2f)", who, y0, y1);
        for (int i = 0; i < 60; ++i) { x.input(0, 0, 1.0f / 30); for (Cli* o : all) o->pump(); usleep(33000); }   // desce
    };
    std::printf("-- sem arquivo de admins --\n");
    tryCheats(a, "alice (sem arquivo)", false);
    std::printf("-- arquivo com alice (e o uid do visitante) --\n");
    writeAdmins(ap, "# admins de teste\nfb:alice   # conta da alice\n\nlocal-visit0001\nuid com espaco invalido\n");
    idle(all, 2600);
    tryCheats(a, "alice ADMIN (token verificado)", true);
    tryCheats(b, "bob (verificado, fora da lista)", false);
    tryCheats(c, "visitante com uid listado", false);
    tryCheats(e, "E (token falso da alice)", false);
    std::printf("-- arquivo so com comentarios --\n");
    writeAdmins(ap, "# vazio\n   \n");
    idle(all, 2600);
    tryCheats(a, "alice (arquivo vazio)", false);
    std::printf("-- alice de volta (recarga) --\n");
    writeAdmins(ap, "alice\n");
    idle(all, 2600);
    tryCheats(a, "alice ADMIN de novo", true);
    std::printf("-- arquivo apagado --\n");
    writeAdmins(ap, nullptr);
    idle(all, 2600);
    tryCheats(a, "alice (arquivo apagado)", false);
    a.leave(); b.leave(); c.leave(); e.leave(); idle({}, 200);
    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE ADMIN PASSARAM\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : kDefaultPort;
    if (argc > 2 && std::strcmp(argv[2], "world") == 0) return collectTests(port);
    if (argc > 2 && std::strcmp(argv[2], "ship") == 0) return shipTests(port);
    if (argc > 2 && std::strcmp(argv[2], "rescue") == 0) return rescueTests(port);
    if (argc > 2 && std::strcmp(argv[2], "gun") == 0) return gunTests(port);
    if (argc > 2 && std::strcmp(argv[2], "chat") == 0) return chatTests(port);
    if (argc > 2 && std::strcmp(argv[2], "summon") == 0) return summonTests(port);
    if (argc > 2 && std::strcmp(argv[2], "shipown") == 0) return shipOwnTests(port);
    if (argc > 2 && std::strcmp(argv[2], "cleanup") == 0) return cleanupTests(port);
    if (argc > 3 && std::strcmp(argv[2], "teaminv") == 0) return teamInviteTests(port, argv[3]);
    if (argc > 3 && std::strcmp(argv[2], "conquest") == 0) return conquestTests(port, argv[3], 1);
    if (argc > 3 && std::strcmp(argv[2], "conquest2") == 0) return conquestTests(port, argv[3], 2);
    if (argc > 3 && std::strcmp(argv[2], "isle") == 0) return isleTests(port, argv[3], 1);
    if (argc > 3 && std::strcmp(argv[2], "isle2") == 0) return isleTests(port, argv[3], 2);
    if (argc > 3 && std::strcmp(argv[2], "acct1") == 0) return acctTests(port, 1, argv[3], 0);
    if (argc > 4 && std::strcmp(argv[2], "acct2") == 0) return acctTests(port, 2, argv[3], std::atoi(argv[4]));
    if (argc > 4 && std::strcmp(argv[2], "admin") == 0) return adminTests(port, argv[3], argv[4]);
    if (argc > 2 && std::strcmp(argv[2], "persist1") == 0) return persistTests(port, 1, argc > 3 ? argv[3] : nullptr);
    if (argc > 2 && std::strcmp(argv[2], "persist2") == 0) return persistTests(port, 2, argc > 3 ? argv[3] : nullptr);
    const bool expectDebug = !(argc > 2 && std::strcmp(argv[2], "nodebug") == 0);  // servidor iniciado com --debug?
    // stateRel (v41): fora da faixa do i16 vai como float (antes saturava em ±327,67 m e 25,5 m/s)
    {
        PlayerState s; s.id = 7; s.x = 1000.0f + 512.25f; s.y = 401.5f; s.z = 2000.0f - 3.5f; s.speed = 31.7f; s.vy = -420.0f; s.ship = 1; s.sa = 12.34f; s.sb = -2.0f;
        uint8_t pb[128]; Writer w(pb, sizeof pb); w.stateRel(s, 1000.0f, 2000.0f);
        Reader r(pb, w.size()); PlayerState q = r.stateRel(1000.0f, 2000.0f);
        CHECK(r.ok() && std::fabs(q.x - s.x) < 0.01f && std::fabs(q.y - s.y) < 0.01f && std::fabs(q.z - s.z) < 0.01f && std::fabs(q.speed - s.speed) < 0.01f && std::fabs(q.vy - s.vy) < 0.01f && std::fabs(q.sa - s.sa) < 0.01f,
              "stateRel sem saturar: rel x 512 m, y 401 m, 31.7 m/s, vy -420 -> (%.2f %.2f %.2f) %.2f %.2f", q.x - 1000.0f, q.y, q.z - 2000.0f, q.speed, q.vy);
        PlayerState n2; n2.x = 1003.0f; n2.y = 2.0f; n2.speed = 6.0f; uint8_t pc[128]; Writer w2(pc, sizeof pc); w2.stateRel(n2, 1000.0f, 0.0f);
        CHECK(w2.size() < w.size(), "caso normal continua compacto (%zu B; com marcadores %zu B)", w2.size(), w.size());
    }
    // HANDSHAKE (v41): Join curto não tem resposta; Join sem cookie/cookie errado só recebe o desafio (9 B, menor que o Join)
    {
        Cli h(port);
        uint8_t sh[8]; Writer w(sh, sizeof sh); w.header(MsgJoin); w.put<uint8_t>(0); h.send(sh, w.size());   // formato antigo (6 B)
        int nRecv = 0; uint8_t rb[kMaxPacket];
        for (int i = 0; i < 40; ++i) { usleep(5000); while (recvfrom(h.fd, rb, sizeof rb, 0, nullptr, nullptr) > 0) ++nRecv; }
        CHECK(nRecv == 0, "Join curto (sem preenchimento) é ignorado: %d pacotes de volta", nRecv);
        uint8_t jb[kJoinPacketSize]; writeJoin(jb, 0, 0x1234567u); h.send(jb, sizeof jb);   // cookie errado
        size_t got = 0; uint8_t typ = 0; uint32_t ck = 0;
        for (int i = 0; i < 40 && !got; ++i) { usleep(5000); ssize_t n = recvfrom(h.fd, rb, sizeof rb, 0, nullptr, nullptr); if (n > 0) { got = (size_t)n; Reader r(rb, (size_t)n); typ = r.header(); ck = r.get<uint32_t>(); } }
        CHECK(typ == MsgChallenge && ck != 0 && got < (size_t)kJoinPacketSize, "Join sem cookie certo => só o desafio (%zu B < %d B do Join), sem vaga", got, kJoinPacketSize);
        h.cookie = ck; h.join(); wait(&h, nullptr, 150);
        CHECK(h.id >= 0, "com o cookie do desafio entra (id=%d)", h.id);
        uint8_t lv[8]; Writer wl(lv, sizeof lv); wl.header(MsgLeave); h.send(lv, wl.size()); wait(&h, nullptr, 50);
    }
    Cli a(port), b(port);
    std::chrono::steady_clock::time_point t0;
    a.join(); wait(&a, nullptr, 200);
    CHECK(a.id >= 0, "A recebeu Welcome (id=%d)", a.id);
    CHECK(a.challenges == 1, "A passou pelo desafio do handshake (%d)", a.challenges);
    b.join(); wait(&a, &b, 200);
    CHECK(b.id >= 0 && b.id != a.id, "B recebeu Welcome com id distinto (id=%d)", b.id);
    float cyc0 = a.cycle;
    t0 = std::chrono::steady_clock::now();
    CHECK(a.last.size() == 2, "snapshot contém 2 jogadores (%zu)", a.last.size());

    // 1s de "frente" a 30 Hz => ~4 m em -Z
    const PlayerState* m0 = a.me(); float z0 = m0 ? m0->z : 0;
    for (int i = 0; i < 30; ++i) { a.input(0, 1, 1.0f / 30); wait(&a, &b, 33); }
    wait(&a, &b, 150);
    const PlayerState* m1 = a.me();
    float dz = m1 ? m1->z - z0 : 0;
    CHECK(m1 && dz < -2.6f && dz > -3.4f, "andar: dz=%.2f (esperado ~-%.1f)", dz, kWalkSpeed);
    CHECK(a.ack >= 28, "ack de inputs: %u", a.ack);
    { const uint32_t ack0 = a.ack; a.input(0, 0, 1.0f / 120, kMaxCmdsPerPacket); wait(&a, &b, 120);
      CHECK(a.ack == ack0 + (uint32_t)kMaxCmdsPerPacket, "Input com %d comandos (120 Hz) aceito: ack %u -> %u", kMaxCmdsPerPacket, ack0, a.ack); }
    CHECK(a.echo == a.tok && a.hold < 300, "eco p/ RTT: echo=%u (enviado %u) hold=%u ms", a.echo, a.tok, a.hold);
    CHECK(b.snaps > 10, "B recebe snapshots continuamente (%d)", b.snaps);

    // speed-hack: 16 comandos de dt=0.1 de uma vez (1.6 s de simulação em ~0 s)
    float zb = a.me()->z;
    a.input(0, 1, 0.1f, 16); wait(&a, &b, 150);
    float hack = zb - a.me()->z;
    CHECK(hack < 3.1f, "anti speed-hack limitou o avanço: %.2f m (sem limite seria 4.8)", hack);

    // correr: 1s com a flag => ~6 m; e andar para a direita gira o corpo para +X (yaw ~ pi/2)
    float zr = a.me()->z;
    for (int i = 0; i < 30; ++i) { a.input(0, 1, 1.0f / 30, 1, InputRun); wait(&a, &b, 33); }
    wait(&a, &b, 150);
    float run = zr - a.me()->z;
    CHECK(run > 5.2f && run < 6.8f, "correr: %.2f m em 1 s (esperado ~%.1f)", run, kRunSpeed);
    float xr = a.me()->x;
    for (int i = 0; i < 30; ++i) { a.input(1, 0, 1.0f / 30); wait(&a, &b, 33); }
    wait(&a, &b, 150);
    CHECK(a.me()->x - xr > 2.6f && std::fabs(a.me()->yaw - kPi / 2) < 0.1f, "lateral: dx=%.2f yaw=%.2f (esperado ~%.2f)", a.me()->x - xr, a.me()->yaw, kPi / 2);

    // pulo: 1 comando com InputJump, depois parado ~1 s; pico ~1.17 m e volta ao chão (y=0)
    a.input(0, 0, 1.0f / 30, 1, InputJump);
    float peak = 0;
    for (int i = 0; i < 36; ++i) { a.input(0, 0, 1.0f / 30); wait(&a, &b, 33); if (a.me()) peak = std::max(peak, a.me()->y); }
    wait(&a, &b, 150);
    CHECK(peak > 0.9f && peak < 1.4f && a.me()->y == 0.0f, "pulo: pico %.2f m, final y=%.2f", peak, a.me()->y);
    a.input(0, 0, 1.0f / 30, 1, InputJump); a.input(0, 0, 1.0f / 30, 1, InputJump); wait(&a, &b, 100);
    CHECK(a.me()->y > 0.0f, "pulo no chão aceito");

    // jitter de rede: cliente honesto (cmds em tempo real) cuja entrega atrasa 0.4 s e chega em rajada.
    // O servidor NÃO pode perder movimento legítimo (senão o jogador toma "teleporte de volta").
    {
        Cli j(port);
        j.join(); wait(&j, nullptr, 200);
        for (int i = 0; i < 30; ++i) { j.input(0, 1, 1.0f / 30); wait(&j, nullptr, 33); }  // 1 s estável
        wait(&j, nullptr, 100);
        float z0 = j.me()->z;
        wait(&j, nullptr, 400);                       // pico de latência: nada chega
        j.input(0, 1, 1.0f / 30, 12);                 // 12 cmds = 0.4 s de movimento, em rajada
        wait(&j, nullptr, 150);
        float got = z0 - j.me()->z;
        CHECK(got > 1.1f, "jitter 0.4 s: avanço %.2f m (esperado ~1.20, sem perda)", got);
        uint8_t lb2[8]; Writer w2(lb2, 8); w2.header(MsgLeave); j.send(lb2, w2.size());
        wait(&j, nullptr, 100);
    }

    // ÁREA DE INTERESSE: quem está longe (> kInterestRadius) não vem no snapshot; ao voltar, reaparece (só com --debug: teleporte)
    if (expectDebug) {
        Cli p(port), q(port);
        p.join(); q.join(); wait(&p, &q, 250);
        p.input(0, 0, 0.033f); q.input(0, 0, 0.033f); wait(&p, &q, 150);
        CHECK(p.me() && q.me() && p.find(q.id), "dois jogadores perto se veem");
        q.teleport(p.me()->x + kInterestRadius + 60.0f, p.me()->z); q.input(0, 0, 0.033f); wait(&p, &q, 250);
        CHECK(p.find(q.id) == nullptr && q.me() && q.me()->x > p.me()->x + kInterestRadius, "jogador a %.0f m some do snapshot do outro (P vê %zu)", kInterestRadius + 60.0f, p.last.size());
        q.teleport(p.me()->x + 3.0f, p.me()->z - 1.5f); q.input(0, 0, 0.033f); wait(&p, &q, 250);
        const PlayerState* pq = p.find(q.id);
        CHECK(pq && q.me() && std::fabs(pq->x - q.me()->x) < 0.02f && std::fabs(pq->z - q.me()->z) < 0.02f, "de volta perto: reaparece na posição certa (formato compacto, erro %.3f m)", pq && q.me() ? std::fabs(pq->x - q.me()->x) : -1.0f);
        uint8_t lb3[8]; Writer w3(lb3, 8); w3.header(MsgLeave); p.send(lb3, w3.size()); q.send(lb3, w3.size()); wait(&p, &q, 100);
    }

    // pacote lixo não derruba o servidor
    uint8_t junk[7] = {1, 2, 3, 4, 5, 6, 7}; a.send(junk, sizeof junk);
    a.input(0, 0, 0.033f); wait(&a, &b, 150);
    CHECK(a.me() != nullptr, "servidor sobreviveu a pacote inválido");

    uint8_t lb[8]; Writer w(lb, 8); w.header(MsgLeave); b.send(lb, w.size());
    wait(&a, nullptr, 200);
    CHECK(a.last.size() == 1, "B saiu: snapshot volta a 1 jogador (%zu)", a.last.size());
    // relógio do mundo: avança em tempo real e é igual para todos os clientes
    float ela = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    CHECK(cyc0 >= 0 && std::fabs(std::fmod(a.cycle - cyc0 + kCycleSec, kCycleSec) - ela) < 0.6f, "ciclo avançou %.2f s em %.2f s reais (início %.1f)", std::fmod(a.cycle - cyc0 + kCycleSec, kCycleSec), ela, cyc0);

    // capacidade: A + (kMaxPlayers - 1) = kMaxPlayers jogadores; o próximo é recusado
    std::vector<Cli*> cs;
    for (int i = 0; i < kMaxPlayers - kFirstPublicId - 1; ++i) cs.push_back(new Cli(port));
    Cli& c1 = *cs[0];
    Cli cExtra(port);
    for (size_t i = 0; i < cs.size(); ++i) {
        cs[i]->join();
        if ((i & 7) == 7) wait(&a, nullptr, 15);
    }
    for (int r = 0; r < 80; ++r) {   // o servidor aceita até 400 entradas/s: 1000 levam ~2,5 s
        wait(&a, nullptr, 50);
        int missing = 0;
        for (Cli* c : cs) {
            c->pump();
            if (c->id < 0) { c->join(); ++missing; }
        }
        if (!missing) break;
    }
    wait(&a, &c1, 100);
    for (Cli* c : cs) c->pump();
    CHECK(std::fabs(a.cycle - c1.cycle) < 0.2f, "todos veem a mesma hora: A=%.2f C1=%.2f", a.cycle, c1.cycle);
    { int ok = 1; for (Cli* c : cs) if (c->id >= 0) ++ok;
      CHECK(ok == kMaxPlayers - kFirstPublicId, "sala cheia com %d jogadores (%d conectados)", kMaxPlayers - kFirstPublicId, ok); }
    CHECK(a.last.size() == (size_t)std::min(kMaxPlayers - kFirstPublicId, kSnapMaxOthers + 1), "ÁREA DE INTERESSE: o snapshot leva eu + os %d mais perto (%zu)", kSnapMaxOthers, a.last.size());
    { size_t mx = 0; for (Cli* c : cs) mx = std::max(mx, c->lastBytes); CHECK(mx > 0 && mx <= 1400, "snapshot cabe num pacote sem fragmentar com a sala cheia (%zu B)", mx); }
    cExtra.join(); wait(&cExtra, nullptr, 200);
    CHECK(cExtra.rejected && cExtra.id < 0, "%dº jogador recusado", kMaxPlayers - kFirstPublicId + 1);

    // hora do mundo por debug: aceita só com --debug; vale para todos os clientes
    {
        uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgDebugTime); w.put<float>(1350.0f);
        a.send(pk, w.size()); wait(&a, &b, 250);
        float d = std::fabs(a.cycle - 1350.0f);
        if (expectDebug) CHECK(d < 3.0f, "--debug: hora do mundo mudou para meia-noite (%.1f)", a.cycle);
        else CHECK(d > 100.0f, "sem --debug: pedido ignorado (hora segue %.1f)", a.cycle);
        w = Writer(pk, sizeof pk); w.header(MsgDebugTime); w.put<float>(std::nanf("")); a.send(pk, w.size());
        w = Writer(pk, sizeof pk); w.header(MsgDebugTime); w.put<float>(99999.0f); a.send(pk, w.size());
        wait(&a, &b, 150);
        CHECK(a.cycle >= 0 && a.cycle < kCycleSec, "valores inválidos de hora são ignorados (%.1f)", a.cycle);
    }

    // CHEATS (MsgDebug*) e VOO: só valem com --debug (produção roda sem a flag)
    {
        const int c0 = a.coins, ammo0 = a.count(item::Ammo), it0 = a.count(item::Diamond);
        a.debugCoins(1000); a.debugItems(item::Diamond, 5); wait(&a, &b, 300);
        if (expectDebug) CHECK(a.coins == c0 + 1000 && a.count(item::Diamond) == it0 + 5, "--debug: cheats de moedas/itens valem (%d moedas, %d diamantes)", a.coins, a.count(item::Diamond));
        else CHECK(a.coins == c0 && a.count(item::Diamond) == it0 && a.count(item::Ammo) == ammo0, "sem --debug: MsgDebugCoins/MsgDebugItems IGNORADOS (moedas %d, diamantes %d)", a.coins, a.count(item::Diamond));
        const PlayerState* m0 = a.me(); const float x0 = m0 ? m0->x : 0, y0 = m0 ? m0->y : 0, z0 = m0 ? m0->z : 0;
        a.teleport(x0 + 60.0f, z0); wait(&a, &b, 250);
        const PlayerState* m1 = a.me();
        if (!expectDebug) CHECK(m1 && std::fabs(m1->x - x0) < 5.0f, "sem --debug: teleporte IGNORADO (x %.1f -> %.1f)", x0, m1 ? m1->x : 0.0f);
        if (!expectDebug) for (int i = 0; i < 20; ++i) { a.input(0, 0, 1.0f / 30, 1, (uint8_t)(InputFly | InputJump)); wait(&a, &b, 33); }
        const PlayerState* m2 = a.me();
        if (!expectDebug) CHECK(m2 && m2->y < y0 + 2.0f, "sem --debug: VOAR ignorado (y %.2f -> %.2f)", y0, m2 ? m2->y : 0.0f);
        for (int i = 0; i < 40; ++i) { a.input(0, 0, 1.0f / 30); wait(&a, &b, 33); }   // volta ao chão
        if (expectDebug) { a.teleport(x0, z0); wait(&a, &b, 250); }
    }

    // ferramenta empunhada: o servidor repassa a todos; valor inválido vira "nada"
    {
        a.held = HeldPickaxe; a.input(0, 0, 0.03f); wait(&a, &c1, 200);
        const PlayerState* sa = c1.last.empty() ? nullptr : nullptr;
        for (auto& st : c1.last) if (st.id == a.id) sa = &st;
        CHECK(sa && sa->held == HeldPickaxe, "ferramenta empunhada replicada aos outros (held=%d)", sa ? sa->held : -1);
        a.held = 200; a.input(0, 0, 0.03f); wait(&a, &c1, 200);
        sa = nullptr; for (auto& st : c1.last) if (st.id == a.id) sa = &st;
        CHECK(sa && sa->held == HeldNone, "held inválido é ignorado (held=%d)", sa ? sa->held : -1);
    }

    // coleta num servidor --flat (sem mundo): ignorada, sem quebrar
    a.collect(3); wait(&a, nullptr, 150);
    CHECK(a.okCount == 0 && a.me() != nullptr, "coleta sem mundo (--flat) é ignorada");
    for (Cli* c : cs) delete c;

    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
    return fails ? 1 : 0;
}
