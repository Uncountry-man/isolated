// Isolated - BOT de terminal para testar o multiplayer (roda no notebook/PC, Linux).
// Conecta ao servidor como jogadores de verdade (mesmo protocolo e mesma simulação de shared/protocol.h)
// e executa ações aleatórias: andar, correr, pular, girar, andar em círculo, ficar parado, voltar ao centro.
// Também simula rede ruim (perda, lag, jitter) e entra/sai da sala, para estressar o servidor e o cliente.
//
// Uso: build/bot [opções]        (build/bot --help)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../shared/protocol.h"
#include "../shared/team.h"
#include "../shared/world_sync.h"
using namespace e0net;
using Clock = std::chrono::steady_clock;

static double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
static uint32_t nowMs32() { return (uint32_t)((uint64_t)(nowSec() * 1000.0)); }
static volatile sig_atomic_t g_run = 1;

struct Opts {
    std::string host = "127.0.0.1";
    int port = kDefaultPort, bots = 1, seed = 0;
    double time = 0;          // 0 = até Ctrl+C
    double loss = 0;          // % de perda (entrada e saída)
    double lag = 0, jitter = 0;  // ms por sentido
    double roam = 10;         // raio (m) em torno do spawn em que os bots passeiam
    bool churn = false, quiet = false;
    // teste de MORTE / SALVAR / CARREGAR
    int victims = 0, medics = 0;   // primeiros N bots = VITIMAS (morrem sozinhos); os N seguintes = MEDICOS (salvam/carregam qualquer jogador caido, inclusive o celular)
    double dieAfter = 6, waitRespawn = 25, carryTime = 6;   // vitima morre apos S s; renasce sozinha apos S s sem socorro; medico carrega por S s
    int skin = -1;                  // --skin N: todos os bots com a skin N (padrão: cada bot uma diferente)
    bool noLogin = false;           // --no-login: não manda MsgLogin (fica com a conta efêmera do servidor)
    bool still = false;             // --still: fica PARADO no lugar (para segurar o dedo nele no celular)
    bool teamAccept = false;        // --team-accept: aceita todo convite de TRIPULAÇÃO que receber
    bool teamInvite = false;        // --team-invite: a cada ~8 s convida o jogador mais perto (a até 8 m) para a tripulação do bot (precisa estar numa)
    int chat = 0;                   // --chat N: os N primeiros bots FALAM (frases de teste a cada ~5 s; aparecem em balao sobre a cabeca no app)
    int fighters = 0, weapon = 0;   // PvP: os N bots seguintes LUTAM (perseguem e socam o jogador vivo mais perto); --weapon 0..4 = o que levam na mao (dano muda)
    bool carry = false, repeat = false;                     // medico carrega antes de salvar; vitima morre de novo a cada ciclo
    double spread = 0;        // > 0: cada bot vai (teleporte de debug) para um ponto aleatório do MAPA TODO (±spread m; ilhas e mar)
    double joinRate = 200;    // bots que pedem para entrar por segundo (1000 bots de uma vez = rajada de Joins)
};

struct Pkt { double at; std::vector<uint8_t> d; };
struct PendAct { uint32_t seq; uint8_t kind; uint16_t a, b; double sentAt; double firstAt; };

enum class Act { Idle, Walk, Run, Strafe, Circle, JumpIdle, Pickup, Punch, Chop, Mine, Home };
static const char* actName(Act a) {
    switch (a) {
        case Act::Idle: return "PARADO";    case Act::Walk: return "ANDAR";     case Act::Run: return "CORRER";
        case Act::Strafe: return "LATERAL"; case Act::Circle: return "CIRCULO"; case Act::JumpIdle: return "PULAR";
        case Act::Pickup: return "COLETAR"; case Act::Punch: return "BATER";    case Act::Chop: return "CORTAR";
        case Act::Mine: return "MINERAR";   default: return "VOLTAR";
    }
}

struct Bot {
    enum class St { Joining, Connected, Full, Away } st = St::Joining;
    int idx = 0, fd = -1;
    uint32_t cookie = 0;   // cookie do handshake (MsgChallenge)
    int loginRes = -1, invSyncs = 0;   // CONTA (v42): resposta do login e quantos MsgInvSync chegaram
    sockaddr_in sv{};
    std::mt19937 rng;
    int id = -1;
    PlayerState pred;             // estado previsto localmente (igual ao cliente do jogo)
    float homeX = 0, homeZ = 0;   // ponto de spawn (centro do passeio)
    uint32_t seq = 1;
    std::deque<InputCmd> pending;
    std::vector<Pkt> outq, inq;   // filas de atraso simulado (saída / entrada)
    double lastJoin = -10, lastRecv = 0, lastSend = 0, lastTick = 0, awayUntil = 0, nextChurn = 0;
    uint32_t lastSrvTick = 0;
    float cycle = -1; int players = 0;
    // comportamento
    Act act = Act::Idle; double actUntil = 0; float mx = 0, my = 0, turn = 0, camYaw = 0; bool run = false, wantJump = false;
    uint8_t wantAct = 0, comboAct = 0;
    double comboAt = 0;
    double nextJump = 0;
    // estatísticas
    long snaps = 0, snapsWin = 0, rxB = 0, txB = 0, dropOut = 0, dropIn = 0, reconnects = 0, rejects = 0, joins = 0;
    double rttSum = 0, rttMin = 1e9, rttMax = 0, rttWin = 0; long rttN = 0, rttNWin = 0;
    double corrMax = 0, corrWinMax = 0, dist = 0;
    double rateT = 0, snapRate = 0, rxRate = 0, txRate = 0, rttShow = -1, corrShow = 0;
    long rxWin = 0, txWin = 0;
    bool everConnected = false;
    // morte / salvamento
    std::vector<PlayerState> all;           // ultimo estado de TODOS os jogadores da sala (para achar caidos)
    std::deque<PendAct> acts; uint32_t actSeq = 0;
    bool talker = false; double nextChat = 0; uint8_t chatId = 0; int chatSaid = 0;   // --chat
    double nextInvite = 0; uint32_t inviteId = 0; uint32_t lastOffer = 0; uint8_t lastNoticeRes = 255;            // --team-invite / --team-accept
    bool aimOn = false; uint16_t shotId = 1;   // --weapon 5 (BACAMARTE): mira e atira de longe
    bool fighter = false; double nextPunch = 0; float health = 100; long hitsDealt = 0, dmgTaken = 0, dmgDealt = 0;
    bool victim = false, medic = false, died = false, sawDead = false, respawnSent = false, afterDrop = false;
    double dieAt = -1, deadSince = 0, carryUntil = 0;
    long deaths = 0, revived = 0, respawns = 0, saves = 0, carries = 0, refused = 0;
    int target = -1; double targetSince = 0; uint32_t lastDmgId = 0;
    double joinAt = 0;                       // só começa a entrar a partir daqui (entrada escalonada)
    bool spreadDone = true; float spX = 0, spZ = 0; double spSent = 0;   // --spread: destino do teleporte
    int seenOthers = 0;                      // outros jogadores no último snapshot (área de interesse)
};

static double rnd(Bot& b, double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(b.rng); }

static void rawSend(Bot& b, const std::vector<uint8_t>& d) {
    sendto(b.fd, d.data(), d.size(), 0, (const sockaddr*)&b.sv, sizeof b.sv);
    b.txB += (long)d.size(); b.txWin += (long)d.size();
}

static void sendPkt(Bot& b, const Opts& o, const uint8_t* d, size_t n, double now) {
    if (o.loss > 0 && rnd(b, 0, 100) < o.loss) { ++b.dropOut; return; }
    double delay = std::max(0.0, (o.lag + (o.jitter > 0 ? rnd(b, -o.jitter, o.jitter) : 0)) / 1000.0);
    std::vector<uint8_t> v(d, d + n);
    if (delay <= 0) rawSend(b, v); else b.outq.push_back({now + delay, std::move(v)});
}

static void queueAct(Bot& b, uint8_t kind, uint16_t a, uint16_t bb) {   // acao confiavel (MsgAct), reenviada a cada 0.25 s ate o Ack
    b.acts.push_back({++b.actSeq, kind, a, bb, 0, 0});
}
static void pumpActs(Bot& b, const Opts& o, double now) {
    if (b.acts.empty()) return;
    PendAct& p = b.acts.front();
    if (p.firstAt == 0) p.firstAt = now;
    if (now - p.firstAt > 4.0) { b.acts.pop_front(); return; }
    if (now - p.sentAt < 0.25) return;
    p.sentAt = now;
    uint8_t buf[32]; Writer w(buf, sizeof buf);
    w.header(MsgAct); w.put<uint32_t>(p.seq); w.put<uint8_t>(p.kind); w.put<uint16_t>(p.a); w.put<uint16_t>(p.b);
    sendPkt(b, o, buf, w.size(), now);
}
static const PlayerState* nearestDown(const Bot& b, float sight) {   // jogador caido (morto, nao carregado) mais perto
    const PlayerState* best = nullptr; float bd = sight;
    for (const PlayerState& q : b.all) {
        if (q.id == b.id || !(q.status & StDead) || (q.status & StCarried)) continue;
        float dx = q.x - b.pred.x, dz = q.z - b.pred.z, d = std::sqrt(dx * dx + dz * dz);
        if (d < bd) { bd = d; best = &q; }
    }
    return best;
}
static float yawToward(const Bot& b, float tx, float tz) { return std::atan2(tx - b.pred.x, -(tz - b.pred.z)); }
static const char* stateLabel(const Bot& b, Act a) {
    if (b.fighter && !(b.pred.status & StDead)) return "LUTANDO";
    if (b.pred.status & StDead) return (b.pred.status & StCarried) ? "CARREGADO" : "MORTO";
    if (b.pred.status & StRescuing) return "SALVANDO";
    if (b.pred.status & StCarrying) return "CARREGANDO";
    return actName(a);
}

// Papel de VITIMA / MEDICO: devolve true se assumiu o controle do movimento neste tick.
static bool lifeLogic(Bot& b, const Opts& o, double now) {
    const bool dead = (b.pred.status & StDead) != 0;
    if (b.victim || b.died) {
        if (b.victim && !b.died && b.dieAt > 0 && now >= b.dieAt) { queueAct(b, ActDie, 0, 0); b.died = true; b.sawDead = false; b.respawnSent = false; b.deadSince = now; ++b.deaths; b.dieAt = -1; }
        if (b.died) {
            if (dead) b.sawDead = true;
            if (b.sawDead && !dead && !b.respawnSent) { ++b.revived; b.died = false; b.health = 30; b.dieAt = o.repeat ? now + o.dieAfter : -1; }   // alguem terminou o RCP
            else if (!b.respawnSent && now - b.deadSince > o.waitRespawn) {   // ninguem veio: renasce sozinha
                uint8_t buf[8]; Writer w(buf, sizeof buf); w.header(MsgRespawn); sendPkt(b, o, buf, w.size(), now); sendPkt(b, o, buf, w.size(), now);
                b.respawnSent = true; ++b.respawns; b.died = false; b.health = 100; b.dieAt = o.repeat ? now + o.dieAfter : -1;
            }
            b.mx = b.my = 0; b.run = false; b.wantAct = 0; b.act = Act::Idle; b.actUntil = now + 0.2;
            return true;
        }
    }
    if (dead) { b.mx = b.my = 0; b.run = false; b.wantAct = 0; b.act = Act::Idle; b.actUntil = now + 0.2; return true; }
    if (b.fighter) {   // PvP: persegue o jogador vivo mais perto e soca
        const PlayerState* t = nullptr; float bd = 40.0f;
        for (const PlayerState& q : b.all) {
            if (q.id == b.id || (q.status & (StDead | StCarried))) continue;
            float dx = q.x - b.pred.x, dz = q.z - b.pred.z, d = std::sqrt(dx * dx + dz * dz);
            if (d < bd) { bd = d; t = &q; }
        }
        b.aimOn = false;
        if (!t) return false;
        b.camYaw = yawToward(b, t->x, t->z);
        if (o.weapon == HeldGun) {   // BACAMARTE: fica a ~10 m, mira e atira (cabeça 35% das vezes, senão no peito) a cada 2,6 s
            b.aimOn = true; b.run = false; b.act = Act::Walk; b.actUntil = now + 0.2;
            b.mx = 0; b.my = bd > 14.0f ? 1.0f : (bd < 6.0f ? -1.0f : 0.0f);
            if (now >= b.nextPunch && std::fabs(wrapPi(b.camYaw - b.pred.yaw)) < 0.12f && bd < 40.0f) {
                b.nextPunch = now + 2.6;
                const bool head = rnd(b, 0, 1) < 0.35;
                float ox = b.pred.x, oy = b.pred.y + 1.45f, oz = b.pred.z, tx = t->x, ty = t->y + (head ? kHeadY : 1.05f), tz = t->z;
                float dx = tx - ox, dy = ty - oy, dz = tz - oz, l = std::sqrt(dx * dx + dy * dy + dz * dz);
                uint8_t pk[64]; Writer w(pk, sizeof pk);
                w.header(MsgShoot); w.put<uint16_t>(b.shotId++); w.put(ox); w.put(oy); w.put(oz); w.put(dx / l); w.put(dy / l); w.put(dz / l);
                w.put<float>((float)b.lastSrvTick); w.put<uint8_t>(1);
                sendPkt(b, o, pk, w.size(), now);
            }
            return true;
        }
        if (bd > 1.25f) { b.mx = 0; b.my = 1; b.run = bd > 6.0f; b.act = b.run ? Act::Run : Act::Walk; b.actUntil = now + 0.2; }
        else { b.mx = 0; b.my = 0.04f; b.run = false; b.act = Act::Punch; b.actUntil = now + 0.2;   // micro-passo para o corpo VIRAR de frente (o yaw só gira andando)
               if (now >= b.nextPunch) { b.wantAct = ActPunch; b.nextPunch = now + 1.2; } }
        return true;
    }
    if (!b.medic) return false;
    const uint8_t st = b.pred.status;
    if (st & StRescuing) { b.mx = b.my = 0; b.run = false; b.wantAct = 0; b.actUntil = now + 0.2; return true; }
    if (st & StCarrying) {   // carrega rumo ao spawn, solta e depois salva
        if (b.carryUntil == 0) { b.carryUntil = now + o.carryTime; ++b.carries; }
        b.camYaw = yawToward(b, b.homeX, b.homeZ); b.mx = 0; b.my = 1; b.run = false; b.act = Act::Walk; b.actUntil = now + 0.2;
        if (now >= b.carryUntil) { queueAct(b, ActDropCarry, 0, 0); b.carryUntil = 0; b.afterDrop = true; }
        return true;
    }
    const PlayerState* t = nearestDown(b, 80.0f);
    if (!t) { b.afterDrop = false; return false; }
    float dx = t->x - b.pred.x, dz = t->z - b.pred.z, d = std::sqrt(dx * dx + dz * dz);
    b.camYaw = yawToward(b, t->x, t->z);
    if (d > 1.8f) { b.mx = 0; b.my = 1; b.run = d > 8.0f; b.act = b.run ? Act::Run : Act::Walk; b.actUntil = now + 0.2; return true; }
    b.mx = b.my = 0; b.run = false; b.wantAct = 0; b.act = Act::Idle; b.actUntil = now + 0.2;
    if (b.acts.empty() && now - b.targetSince > 0.6) {
        b.targetSince = now;
        if (o.carry && !b.afterDrop) queueAct(b, ActCarry, t->id, 0); else { queueAct(b, ActRevive, t->id, 0); ++b.saves; }
    }
    return true;
}

static uint8_t randomPunch(Bot& b) {
    static const uint8_t kP[3] = {ActPunch, ActPunch2, ActPunch3};
    return kP[(int)rnd(b, 0, 2.999)];
}

static void chooseAction(Bot& b, const Opts& o, double now) {
    float dx = b.pred.x - b.homeX, dz = b.pred.z - b.homeZ;
    float r2 = dx * dx + dz * dz;
    b.mx = b.my = b.turn = 0; b.run = false; b.comboAct = 0;
    if (r2 > o.roam * o.roam) {  // longe demais do spawn: vira para o spawn e volta
        b.act = Act::Home; b.my = 1; b.run = rnd(b, 0, 1) < 0.5;
        b.camYaw = std::atan2(-dx, dz);  // frente = (sin yaw, -cos yaw) apontando p/ (homeX, homeZ)
        b.actUntil = now + rnd(b, 1.0, 2.2);
        return;
    }
    double r = rnd(b, 0, 100);
    if (r < 24) {
        // Coletar no chão (parado, abaixa e leva a mão ao bolso/cintura ao levantar)
        b.act = Act::Pickup;
        b.wantAct = ActPickup;
        b.actUntil = now + 1.75;
        return;
    } else if (r < 48) {
        // Bater (sorteia entre BATER 1/2/3 e frequentemente enfileira 1 segundo golpe)
        b.act = Act::Punch;
        b.wantAct = randomPunch(b);
        if (rnd(b, 0, 1) < 0.70) {
            b.comboAct = randomPunch(b);
            if (b.comboAct == b.wantAct) b.comboAct = (b.wantAct == ActPunch) ? ActPunch2 : ActPunch;
            b.comboAt = now + 0.32;
        }
        b.actUntil = now + (b.comboAct ? 2.0 : 1.15);
        return;
    } else if (r < 58) {
        b.act = Act::Chop;
        b.wantAct = ActChop;
        b.actUntil = now + 1.75;
        return;
    } else if (r < 68) {
        b.act = Act::Mine;
        b.wantAct = ActMine;
        b.actUntil = now + 1.65;
        return;
    } else if (r < 74) b.act = Act::Idle;
    else if (r < 84) { b.act = Act::Walk; b.my = (float)rnd(b, 0.7, 1.0); b.turn = (float)rnd(b, -0.8, 0.8); }
    else if (r < 92) { b.act = Act::Run; b.my = 1; b.run = true; b.turn = (float)rnd(b, -0.6, 0.6); }
    else if (r < 96) { b.act = Act::Circle; b.my = 1; b.run = rnd(b, 0, 1) < 0.4; b.turn = (float)(rnd(b, 0, 1) < 0.5 ? -1.1 : 1.1); }
    else b.act = Act::JumpIdle;
    b.actUntil = now + rnd(b, 1.0, 2.4);
}

static void behave(Bot& b, const Opts& o, double now, double dt) {
    if (lifeLogic(b, o, now)) { b.comboAct = 0; return; }
    if (now >= b.actUntil) chooseAction(b, o, now);
    if (b.comboAct && now >= b.comboAt) {
        b.wantAct = b.comboAct;
        b.comboAct = 0;
    }
    b.camYaw += b.turn * (float)dt;
    // pulos aleatórios apenas quando em modo PULAR ou caminhada/corrida
    if (b.act == Act::JumpIdle && now >= b.nextJump) {
        b.wantJump = true;
        b.nextJump = now + rnd(b, 0.9, 1.5);
    }
}

static void handle(Bot& b, const Opts& o, const uint8_t* d, size_t n, double arrival) {
    (void)o;
    Reader r(d, n);
    uint8_t type = r.header();
    if (!type) return;
    b.lastRecv = arrival;
    if (type == MsgWelcome) {
        int nid = r.get<uint16_t>(); float x = r.get<float>(), y = r.get<float>(), z = r.get<float>();
        if (!r.ok() || b.st == Bot::St::Connected) return;
        b.id = nid; b.pred = PlayerState{}; b.pred.id = (uint16_t)nid; b.pred.x = x; b.pred.y = y; b.pred.z = z;
        if (b.spreadDone) { b.homeX = x; b.homeZ = z; }
        b.st = Bot::St::Connected; b.everConnected = true; ++b.joins;
        b.pending.clear(); b.lastSrvTick = 0; b.actUntil = 0; b.nextJump = arrival + 1.0;
        b.acts.clear(); b.died = false; b.sawDead = false; b.respawnSent = false; b.carryUntil = 0; if (b.victim && b.dieAt < 0 && b.deaths == 0) b.dieAt = arrival + o.dieAfter;
        if (!o.noLogin) {   // CONTA (v42): cada bot entra como visitante local de teste ("local-bot<semente><idx>"; dados no servidor se ele tiver --data)
            char uid[48]; std::snprintf(uid, sizeof uid, "local-bot%u%04d", o.seed, b.idx);
            uint8_t pk[96]; Writer w(pk, sizeof pk); const size_t ul = std::strlen(uid);
            w.header(MsgLogin); w.put<uint8_t>(1); w.put<uint8_t>(LoginLocal); w.put<uint8_t>((uint8_t)ul); w.bytes(uid, ul); w.put<uint16_t>(0); w.put<uint8_t>(0);
            for (int k = 0; k < 2; ++k) sendPkt(b, o, pk, w.size(), arrival);
        }
    } else if (type == MsgInvSync) {   // inventário do servidor: só confirma (senão ele reenvia 1x/s)
        uint32_t rv = r.get<uint32_t>();
        if (r.ok()) { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgInvAck); w.put<uint32_t>(rv); sendPkt(b, o, pk, w.size(), arrival); ++b.invSyncs; }
    } else if (type == MsgTeamOffer) {   // TRIPULAÇÃO: convite recebido
        const uint32_t oid = r.get<uint32_t>(); const uint16_t from = r.get<uint16_t>();
        if (!r.ok() || oid == b.lastOffer) return;
        b.lastOffer = oid;
        std::printf("[bot %d] convite de tripulacao do jogador %d%s\n", b.idx, (int)from, o.teamAccept ? ": ACEITOU" : " (ignorado: use --team-accept)");
        if (o.teamAccept) { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgTeamOfferReply); w.put<uint32_t>(oid); w.put<uint8_t>(1); for (int k = 0; k < 2; ++k) sendPkt(b, o, pk, w.size(), arrival); }
    } else if (type == MsgTeamNotice) {
        const uint8_t kind = r.get<uint8_t>(), res = r.get<uint8_t>(); const uint16_t other = r.get<uint16_t>();
        if (r.ok() && (kind != TnInviteSent || res != b.lastNoticeRes)) std::printf("[bot %d] tripulacao: aviso %d resultado %d (jogador %d)\n", b.idx, (int)kind, (int)res, (int)other);
        if (kind == TnInviteSent) b.lastNoticeRes = res;
    } else if (type == MsgLoginResult) {
        r.get<uint8_t>(); const uint8_t st = r.get<uint8_t>();
        if (r.ok()) b.loginRes = st;
    } else if (type == MsgActAck) {
        uint32_t seq = r.get<uint32_t>(); uint8_t kind = r.get<uint8_t>(); uint8_t res = r.get<uint8_t>();
        if (!r.ok() || b.acts.empty() || b.acts.front().seq != seq) return;
        if (!res) { ++b.refused; if (kind == ActCarry || kind == ActRevive) b.afterDrop = false; }
        b.acts.pop_front();
    } else if (type == MsgDamage) {
        uint32_t did = r.get<uint32_t>(); uint8_t amount = r.get<uint8_t>(); uint16_t from = r.get<uint16_t>(); (void)from;
        if (!r.ok()) return;
        { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgDamageAck); w.put<uint32_t>(did); sendPkt(b, o, pk, w.size(), arrival); }
        if (b.lastDmgId == did) return;   // repetido
        b.lastDmgId = did; b.dmgTaken += amount;
        if (!(b.pred.status & StDead) && !b.died) {
            b.health -= amount;
            if (b.health <= 0) { queueAct(b, ActDie, 0, 0); b.died = true; b.sawDead = false; b.respawnSent = false; b.deadSince = arrival; ++b.deaths; }
        }
    } else if (type == MsgHitConfirm) {
        uint16_t target = r.get<uint16_t>(); uint8_t amount = r.get<uint8_t>(); (void)target;
        if (r.ok()) { ++b.hitsDealt; b.dmgDealt += amount; }
    } else if (type == MsgChallenge) {   // handshake: repete o Join com o cookie
        const uint32_t ck = r.get<uint32_t>();
        if (r.ok() && b.st == Bot::St::Joining) { b.cookie = ck; uint8_t p[kJoinPacketSize]; sendPkt(b, o, p, writeJoin(p, (uint8_t)(o.skin >= 0 ? o.skin % kSkinCount : b.idx % kSkinCount), ck), arrival); b.lastJoin = arrival; }
    } else if (type == MsgReject) {
        if (b.st == Bot::St::Joining) { b.st = Bot::St::Full; ++b.rejects; b.awayUntil = arrival + 2.0; }
    } else if (type == MsgSnapshot && b.st == Bot::St::Connected) {
        uint32_t tick = r.get<uint32_t>(); float cyc = r.get<float>();
        uint32_t echo = r.get<uint32_t>(); uint16_t hold = r.get<uint16_t>(); int count = r.get<uint16_t>();
        if (!r.ok() || count > kMaxPlayers || tick <= b.lastSrvTick) return;
        b.all.clear();
        PlayerState ps = r.state(); uint32_t ack = r.get<uint32_t>();   // EU (completo) + os outros da área de interesse (compactos)
        if (!r.ok() || ps.id != b.id) return;
        b.all.push_back(ps);
        for (int i = 0; i < count; ++i) b.all.push_back(r.stateRel(ps.x, ps.z));
        if (!r.ok()) return;
        {
            PlayerState before = b.pred;
            b.pred = ps;
            while (!b.pending.empty() && b.pending.front().seq <= ack) b.pending.pop_front();
            for (const InputCmd& c : b.pending) stepPlayer(b.pred, c);
            double ex = b.pred.x - before.x, ez = b.pred.z - before.z, e = std::sqrt(ex * ex + ez * ez);
            if (e < 20.0) { b.corrMax = std::max(b.corrMax, e); b.corrWinMax = std::max(b.corrWinMax, e); }   // > 20 m = teleporte (--spread / renascer), não é erro de predição
            if (!b.spreadDone && std::hypot(ps.x - b.spX, ps.z - b.spZ) < 30.0f) { b.spreadDone = true; b.homeX = ps.x; b.homeZ = ps.z; }
        }
        b.lastSrvTick = tick; b.cycle = cyc; b.players = count + 1; b.seenOthers = count; ++b.snaps; ++b.snapsWin;
        if (echo) {
            int32_t rtt = (int32_t)((uint32_t)((uint64_t)(arrival * 1000.0)) - echo) - (int32_t)hold;
            if (rtt >= 0 && rtt < 10000) {
                b.rttSum += rtt; ++b.rttN; b.rttWin += rtt; ++b.rttNWin;
                b.rttMin = std::min(b.rttMin, (double)rtt); b.rttMax = std::max(b.rttMax, (double)rtt);
            }
        }
    }
}

// Rodada a CADA ciclo do loop (não só no tick de 30 Hz): entrega o que o atraso simulado liberou e
// lê o socket assim que chega pacote, para o instante de chegada (e o RTT) ficar correto.
static void pumpBot(Bot& b, const Opts& o, double now) {
    // atraso simulado: entrega pacotes de saída vencidos
    for (size_t i = 0; i < b.outq.size();) {
        if (b.outq[i].at <= now) { rawSend(b, b.outq[i].d); b.outq.erase(b.outq.begin() + (long)i); } else ++i;
    }
    // recebe do socket (com perda/lag simulados) e processa o que venceu
    uint8_t buf[kMaxPacket];
    for (;;) {
        ssize_t n = recv(b.fd, buf, sizeof buf, 0);
        if (n < 0) break;
        b.rxB += n; b.rxWin += n;
        if (o.loss > 0 && rnd(b, 0, 100) < o.loss) { ++b.dropIn; continue; }
        double delay = std::max(0.0, (o.lag + (o.jitter > 0 ? rnd(b, -o.jitter, o.jitter) : 0)) / 1000.0);
        b.inq.push_back({now + delay, std::vector<uint8_t>(buf, buf + n)});
    }
    std::sort(b.inq.begin(), b.inq.end(), [](const Pkt& x, const Pkt& y) { return x.at < y.at; });
    while (!b.inq.empty() && b.inq.front().at <= now) {
        Pkt p = std::move(b.inq.front()); b.inq.erase(b.inq.begin());
        handle(b, o, p.d.data(), p.d.size(), now);
    }
}

// Rodada no tick de 30 Hz: máquina de estados da conexão, comportamento aleatório e envio do input.
static void tickBot(Bot& b, const Opts& o, double now) {
    // estados de conexão
    if (b.st == Bot::St::Away && now >= b.awayUntil) { b.st = Bot::St::Joining; b.lastJoin = -10; }
    if (b.st == Bot::St::Full && now >= b.awayUntil) { b.st = Bot::St::Joining; b.lastJoin = -10; }
    if (b.st == Bot::St::Joining && now >= b.joinAt && now - b.lastJoin > 0.5) {
        b.lastJoin = now; uint8_t p[kJoinPacketSize]; sendPkt(b, o, p, writeJoin(p, (uint8_t)(o.skin >= 0 ? o.skin % kSkinCount : b.idx % kSkinCount), b.cookie), now);
    }
    if (b.st == Bot::St::Connected) {
        if (now - b.lastRecv > kTimeoutSec) { ++b.reconnects; b.st = Bot::St::Joining; b.lastJoin = -10; b.lastRecv = now; return; }
        if (o.churn && now >= b.nextChurn) {  // sai da sala e volta depois
            uint8_t p[8]; Writer w(p, sizeof p); w.header(MsgLeave); sendPkt(b, o, p, w.size(), now);
            b.st = Bot::St::Away; b.awayUntil = now + rnd(b, 2.0, 5.0); b.nextChurn = b.awayUntil + rnd(b, 15.0, 40.0); return;
        }
        double dt = std::min(0.1, now - b.lastTick);
        if (dt <= 0.0) return;
        b.lastTick = now;
        if (!b.spreadDone && now - b.spSent > 0.6) {   // --spread: pede o teleporte (servidor --debug) até chegar
            b.spSent = now; uint8_t p[16]; Writer w(p, sizeof p); w.header(MsgDebugTeleport); w.put<float>(b.spX); w.put<float>(b.spZ); sendPkt(b, o, p, w.size(), now);
        }
        behave(b, o, now, dt);
        pumpActs(b, o, now);
        if (o.teamInvite && now >= b.nextInvite && b.all.size() > 1) {   // TRIPULAÇÃO: convida o jogador mais perto (a até 8 m)
            b.nextInvite = now + 8.0;
            int best = -1; float bd = kTeamInviteDist;
            for (size_t k = 1; k < b.all.size(); ++k) { const float d = std::hypot(b.all[k].x - b.pred.x, b.all[k].z - b.pred.z); if (d < bd) { bd = d; best = b.all[k].id; } }
            if (best >= 0) { uint8_t pk[16]; Writer w(pk, sizeof pk); w.header(MsgTeamInvite); w.put<uint32_t>(++b.inviteId); w.put<uint16_t>((uint16_t)best); sendPkt(b, o, pk, w.size(), now); }
        }
        if (b.talker && now >= b.nextChat) {   // CHAT: frase de teste (uma com texto bem longo para testar a quebra de linha)
            static const char* kSay[] = {"Ola, pessoal!", "Alguem viu o navio?", "Tem palmeira aqui perto, vamos cortar", "Cuidado com o bacamarte!",
                                         "Preciso de madeira e corda para fazer a machadinha antes de anoitecer, quem ajuda?", "Boa noite", "Tem diamante na ilha de pedra!", "Voc\xEA viu a ca\xE7" "ada? \xC9 \xF3timo, cora\xE7\xE3o!"};
            const char* t = kSay[(b.chatSaid + b.chatId * 3) % 8];
            b.nextChat = now + rnd(b, 4.0, 7.0);
            char msg[96]; std::snprintf(msg, sizeof msg, "%s", t); ++b.chatSaid;
            uint8_t pk[128]; Writer w(pk, sizeof pk); const int n = (int)std::strlen(msg);
            w.header(MsgChat); w.put<uint8_t>(++b.chatId); w.put<uint8_t>((uint8_t)n);
            for (int i = 0; i < n; ++i) w.put<uint8_t>((uint8_t)msg[i]);
            sendPkt(b, o, pk, w.size(), now);
        }
        float g = terrainPtr() ? terrainPtr()->heightAt(b.pred.x, b.pred.z) : 0.0f;
        bool grounded = b.pred.y <= g + 0.03f && b.pred.vy <= 0.0f;
        InputCmd c; c.moveX = o.still ? 0.0f : b.mx; c.moveY = o.still ? 0.0f : b.my; c.camYaw = b.camYaw; c.dt = (float)dt; c.held = (uint8_t)std::max(0, std::min((int)HeldCount - 1, o.weapon));
        c.aim = b.aimOn ? AimOn : 0;
        c.flags = (b.run ? InputRun : 0) |
                  ((b.wantJump && grounded) ? InputJump : 0) |
                  ((grounded ? (b.wantAct & 7u) : 0u) << InputActShift);
        if (o.still) c.flags = 0;   // --still: parado de verdade (sem pular, correr nem SOCAR quem está perto)
        if (c.flags & InputJump) b.wantJump = false;
        if (grounded && b.wantAct) b.wantAct = 0;
        c.seq = b.seq++;
        stepPlayer(b.pred, c);
        b.pending.push_back(c); if (b.pending.size() > 64) b.pending.pop_front();
        uint8_t p[kMaxPacket]; Writer w(p, sizeof p);
        w.header(MsgInput); w.put<uint32_t>(nowMs32());
        size_t cnt = std::min<size_t>(b.pending.size(), kMaxCmdsPerPacket);
        w.put<uint8_t>((uint8_t)cnt);
        for (size_t i = b.pending.size() - cnt; i < b.pending.size(); ++i) w.cmd(b.pending[i]);
        sendPkt(b, o, p, w.size(), now);
    }
    // taxas por janela de 1 s
    if (now - b.rateT >= 1.0) {
        double w = b.rateT > 0 ? now - b.rateT : 1.0;
        b.snapRate = b.snapsWin / w; b.rxRate = b.rxWin / w / 1024.0; b.txRate = b.txWin / w / 1024.0;
        b.rttShow = b.rttNWin ? b.rttWin / b.rttNWin : b.rttShow; b.corrShow = b.corrWinMax;
        b.snapsWin = 0; b.rxWin = b.txWin = 0; b.rttWin = 0; b.rttNWin = 0; b.corrWinMax = 0; b.rateT = now;
    }
}

static void usage() {
    std::printf(
        "Isolated BOT - testa o multiplayer com jogadores automaticos (acoes aleatorias)\n\n"
        "uso: build/bot [opcoes]\n"
        "  --host IP     servidor (padrao 127.0.0.1)        --port N    porta UDP (padrao %d)\n"
        "  --bots N      quantidade de bots (padrao 1; a sala comporta %d jogadores)\n"
        "  --time S      encerra apos S segundos (padrao: ate Ctrl+C)\n"
        "  --roam M      raio em metros do passeio (padrao 30)   --seed N   semente aleatoria\n"
        "  --loss P      %% de pacotes perdidos em cada sentido    --lag MS    atraso por sentido\n"
        "  --jitter MS   variacao do atraso (+-)                  --churn     bots saem e entram da sala sozinhos\n"
        "  --quiet       sem redesenho do terminal (1 linha por bot a cada 2 s; com mais de 24 bots: 1 linha de resumo)\n"
        "  --spread M    cada bot vai para um ponto aleatorio do MAPA TODO (+-M m; 60%% em ilhas, o resto no mar). Servidor com --debug\n"
        "  --join-rate N bots entrando por segundo (padrao 200)\n"
        "  SKIN: --skin N   todos os bots com a skin N (0..%d; padrao: cada bot uma diferente)\n"
        "  CHAT: --chat N   os N primeiros bots FALAM a cada ~5 s (balao de texto sobre a cabeca no app)\n"
        "  TRIPULACAO: --team-accept (aceita convites)  --team-invite (convida o jogador mais perto a cada 8 s; o bot precisa estar numa tripulacao)\n"
        "  PvP: --fighters N  os N bots (depois das vitimas/medicos) PERSEGUEM e SOCAM o jogador vivo mais perto (inclusive o celular); --weapon 0..5 = na mao (0 nada, 1 machadinha, 2 picareta, 3 tocha, 4 adaga, 5 BACAMARTE: mira e atira de ~10 m)\n"
        "  MORTE/SALVAR/CARREGAR:  --victims N  os N primeiros bots MORREM sozinhos (apos --die-after S, padrao 6) e ficam caidos ate serem salvos\n"
        "                          --medics N   os N bots seguintes SALVAM (RCP) ou, com --carry, CARREGAM por --carry-time S (padrao 6) e salvam depois;\n"
        "                                       tambem socorrem o celular/jogador real caido   --wait S  vitima renasce sozinha apos S s (padrao 25)   --repeat  vitimas morrem de novo\n"
        "exemplos:\n  build/bot --bots 3\n  build/bot --bots 1000 --spread 4900 --roam 40 --time 300   (MIL jogadores no mapa todo)\n  build/bot --bots 2 --victims 1 --medics 1 --carry --time 60   (um morre, o outro carrega e salva)\n  build/bot --bots 1 --medics 1   (socorre VOCE quando morrer no celular)\n  build/bot --host 192.168.0.62 --bots 4 --loss 5 --lag 60 --jitter 30 --churn --time 120\n", kDefaultPort, kMaxPlayers, kSkinCount - 1);
}

// Ponto aleatório do MAPA TODO para --spread: 60% em terra (uma ilha qualquer do oceano, ponto seco), o resto no mar (nadando)
static void spreadPoint(Bot& b, const Opts& o, float& x, float& z) {
    const Terrain& T = terrain();
    const double lim = std::min(o.spread, (double)Terrain::limit() - 20.0);
    if (rnd(b, 0, 1) < 0.6) {
        std::vector<int> slots;
        for (int sl = 0; sl < kIslandSlots; ++sl) if (T.world(sl)) { const World* w = T.world(sl); if (std::fabs(w->oxcm * 0.01f) < lim && std::fabs(w->ozcm * 0.01f) < lim) slots.push_back(sl); }
        if (!slots.empty()) for (int tries = 0; tries < 60; ++tries) {
            const World* w = T.world(slots[(size_t)rnd(b, 0, (double)slots.size() - 0.001)]);
            float px = w->oxcm * 0.01f + (float)rnd(b, -World::kHalf, World::kHalf), pz = w->ozcm * 0.01f + (float)rnd(b, -World::kHalf, World::kHalf);
            if (T.heightAt(px, pz) > 0.4f) { x = px; z = pz; return; }
        }
    }
    x = (float)rnd(b, -lim, lim); z = (float)rnd(b, -lim, lim);
}

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* { if (i + 1 >= argc) { std::fprintf(stderr, "faltou valor para %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--host") o.host = val(); else if (a == "--port") o.port = std::atoi(val());
        else if (a == "--bots") o.bots = std::max(1, std::min(kMaxPlayers - kFirstPublicId, std::atoi(val())));
        else if (a == "--time") o.time = std::atof(val()); else if (a == "--seed") o.seed = std::atoi(val());
        else if (a == "--loss") o.loss = std::atof(val()); else if (a == "--lag") o.lag = std::atof(val());
        else if (a == "--jitter") o.jitter = std::atof(val()); else if (a == "--roam") o.roam = std::atof(val());
        else if (a == "--churn") o.churn = true; else if (a == "--quiet") o.quiet = true;
        else if (a == "--victims") o.victims = std::atoi(val()); else if (a == "--medics") o.medics = std::atoi(val());
        else if (a == "--die-after") o.dieAfter = std::atof(val()); else if (a == "--wait") o.waitRespawn = std::atof(val());
        else if (a == "--no-login") o.noLogin = true;
        else if (a == "--skin") o.skin = std::atoi(val()); else if (a == "--chat") o.chat = std::atoi(val()); else if (a == "--team-accept") o.teamAccept = true; else if (a == "--still") o.still = true; else if (a == "--team-invite") o.teamInvite = true; else if (a == "--fighters") o.fighters = std::atoi(val()); else if (a == "--weapon") o.weapon = std::atoi(val());
        else if (a == "--carry-time") o.carryTime = std::atof(val()); else if (a == "--carry") o.carry = true; else if (a == "--repeat") o.repeat = true;
        else if (a == "--spread") o.spread = std::atof(val()); else if (a == "--join-rate") o.joinRate = std::max(1.0, std::atof(val()));
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "opcao desconhecida: %s (use --help)\n", a.c_str()); return 2; }
    }
    signal(SIGINT, [](int) { g_run = 0; });
    signal(SIGTERM, [](int) { g_run = 0; });
    if (o.seed == 0) o.seed = (int)(Clock::now().time_since_epoch().count() & 0x7fffffff);
    { rlimit rl; if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < (rlim_t)o.bots + 64) { rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, (rlim_t)o.bots + 256); setrlimit(RLIMIT_NOFILE, &rl); } }   // 1 socket por bot

    sockaddr_in sv{};
    sv.sin_family = AF_INET; sv.sin_port = htons((uint16_t)o.port);
    if (inet_pton(AF_INET, o.host.c_str(), &sv.sin_addr) != 1) { std::fprintf(stderr, "IP invalido: %s\n", o.host.c_str()); return 2; }

    // Mundo: baixa o terreno do servidor (TCP) — os bots precisam simular o MESMO relevo, senão a predição diverge.
    // As ilhas do OCEANO ABERTO são geradas aqui da mesma semente (como o servidor e o celular fazem): bots espalhados pisam no mesmo chão.
    static World world;
    static std::unique_ptr<World> isl[kIslandSlots];
    {
        std::vector<uint8_t> payload; uint32_t hash = 0;
        if (fetchWorld(o.host.c_str(), (uint16_t)o.port, 0, payload, hash, 8000) && World::fromBytes(payload.data(), payload.size(), world)) {
            setTerrain(&world);
            std::printf("mundo baixado do servidor: semente %u, hash %08x, %.0f KB (ilha %.0f m)\n", world.seed, world.hash, payload.size() / 1024.0, world.stats().extentM);
            const double ti = nowSec(); int ni = 0;
            for (int sl = 1; sl < kIslandSlots; ++sl) {
                int sx, sz; sectorOfSlot(sl, sx, sz);
                IslandSpec sp = islandSpec(world.seed, sx, sz);
                if (!sp.exists) continue;
                auto w = std::make_unique<World>();
                if (!World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, *w)) continue;
                isl[sl] = std::move(w); terrain().setIsland(sl, isl[sl].get()); ++ni;
            }
            std::printf("oceano aberto: %d ilhas geradas em %.1f s\n", ni, nowSec() - ti);
        } else {
            std::printf("servidor sem mundo (--flat?): bots usam chao plano\n");
        }
    }
    std::vector<Bot> bots((size_t)o.bots);
    double t0 = nowSec();
    const int ep = epoll_create1(0);
    for (int i = 0; i < o.bots; ++i) {
        Bot& b = bots[(size_t)i];
        b.idx = i; b.sv = sv; b.rng.seed((uint32_t)(o.seed * 7919 + i * 104729));
        b.fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (b.fd < 0) { std::perror("socket (limite de arquivos abertos?)"); return 2; }
        fcntl(b.fd, F_SETFL, fcntl(b.fd, F_GETFL, 0) | O_NONBLOCK);
        int tos = 0xB8; setsockopt(b.fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        int rcvBuf = 256 * 1024; setsockopt(b.fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof rcvBuf);
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.u32 = (uint32_t)i; epoll_ctl(ep, EPOLL_CTL_ADD, b.fd, &ev);
        b.lastRecv = t0; b.lastTick = t0;
        b.joinAt = t0 + i / o.joinRate;
        b.camYaw = (float)rnd(b, -kPi, kPi);
        b.nextChurn = t0 + rnd(b, 15.0, 40.0);
        b.talker = i < o.chat; b.nextChat = t0 + 2.0 + i * 0.7;
        b.victim = i < o.victims; b.medic = i >= o.victims && i < o.victims + o.medics;
        b.fighter = i >= o.victims + o.medics && i < o.victims + o.medics + o.fighters;
        if (o.spread > 0 && terrainPtr()) { spreadPoint(b, o, b.spX, b.spZ); b.spreadDone = false; b.homeX = b.spX; b.homeZ = b.spZ; }
    }
    const bool many = o.bots > 24;                 // muitos bots: uma linha de resumo em vez de uma por bot
    const bool tty = isatty(1) && !o.quiet && !many;
    const bool simNet = o.loss > 0 || o.lag > 0 || o.jitter > 0;
    std::printf("Isolated BOT  ->  %s:%d   bots=%d seed=%d  perda=%.0f%% lag=%.0fms jitter=%.0fms%s%s\n", o.host.c_str(), o.port, o.bots, o.seed,
                o.loss, o.lag, o.jitter, o.churn ? "  churn" : "", o.spread > 0 ? "  ESPALHADOS PELO MAPA" : "");
    if (tty) { std::printf("(Ctrl+C para sair)\n"); for (int i = 0; i <= o.bots; ++i) std::printf("\n"); }

    double nextDraw = 0, nextTick = t0, loopWorst = 0, loopWorstWin = 0;
    std::vector<epoll_event> evs(256);
    while (g_run) {
        double now = nowSec();
        if (o.time > 0 && now - t0 >= o.time) break;
        if (simNet) for (Bot& b : bots) pumpBot(b, o, now);   // perda/atraso simulados: as filas precisam andar mesmo sem pacote novo
        if (now >= nextTick) {
            const double tt = nowSec();
            nextTick += 1.0 / kTickHz; if (nextTick < now) nextTick = now + 1.0 / kTickHz;
            for (Bot& b : bots) tickBot(b, o, now);
            loopWorstWin = std::max(loopWorstWin, nowSec() - tt);
        }
        if (now >= nextDraw) {
            nextDraw = now + (tty ? 0.25 : 2.0);
            const Bot& f = bots[0];
            float hr = f.cycle >= 0 ? std::fmod(6.0f + f.cycle / kCycleSec * 24.0f, 24.0f) : 0;
            char hora[48]; if (f.cycle >= 0) std::snprintf(hora, sizeof hora, "%02d:%02d %s", (int)hr, (int)((hr - (int)hr) * 60), f.cycle < kDayLengthSec ? "DIA" : "NOITE"); else std::snprintf(hora, sizeof hora, "--");
            if (many) {   // resumo agregado
                int con = 0, spread = 0, rec = 0; double rtt = 0, rttMax = 0, corr = 0, snap = 0, rx = 0, tx = 0, others = 0; int nr = 0;
                for (const Bot& b : bots) {
                    if (b.st == Bot::St::Connected) { ++con; snap += b.snapRate; others += b.seenOthers; if (b.spreadDone) ++spread; }
                    if (b.rttShow >= 0) { rtt += b.rttShow; rttMax = std::max(rttMax, b.rttShow); ++nr; }
                    corr = std::max(corr, b.corrShow); rx += b.rxRate; tx += b.txRate; rec += (int)b.reconnects;
                }
                std::printf("t=%4.0fs  hora %s  conectados %d/%d (no lugar %d)  ping med %.0f max %.0f ms  snap %.1f/s por bot  vizinhos med %.1f  correcao max %.2f m  rx %.1f MB/s tx %.1f MB/s  reconexoes %d  tick dos bots %.1f ms\n",
                            now - t0, hora, con, o.bots, spread, nr ? rtt / nr : 0.0, rttMax, con ? snap / con : 0.0, con ? others / con : 0.0, corr, rx / 1024.0, tx / 1024.0, rec, loopWorstWin * 1000.0);
                loopWorst = std::max(loopWorst, loopWorstWin); loopWorstWin = 0;
            } else {
                if (tty) std::printf("\033[%dA", o.bots + 1);
                std::printf("%st=%4.0fs  hora do mundo %s  sala %d/%d%s\n", tty ? "\033[2K" : "", now - t0, hora, f.players, kMaxPlayers, tty ? "" : "");
                for (const Bot& b : bots) {
                    const char* st = b.st == Bot::St::Connected ? "\033[32mOK \033[0m" : b.st == Bot::St::Joining ? "\033[33mENT\033[0m"
                                     : b.st == Bot::St::Full ? "\033[31mCHEIA\033[0m" : "\033[36mSAIU\033[0m";
                    char ping[16]; if (b.rttShow >= 0) std::snprintf(ping, sizeof ping, "%3.0f", b.rttShow); else std::snprintf(ping, sizeof ping, " --");
                    std::printf("%sBOT%-2d %s id=%d %-10s pos(%6.1f %4.1f %6.1f) v=%3.1f ping=%sms snap=%2.0f/s corr=%.2fm rx=%4.1f tx=%4.1f KB/s%s\n",
                                tty ? "\033[2K" : "", b.idx, st, b.id, b.st == Bot::St::Connected ? stateLabel(b, b.act) : "-", b.pred.x, b.pred.y, b.pred.z,
                                b.pred.speed, ping, b.snapRate, b.corrShow, b.rxRate, b.txRate, b.reconnects ? "  (RECONEXOES)" : "");
                }
            }
            std::fflush(stdout);
        }
        double wait = std::max(0.0, std::min(nextTick, nextDraw) - nowSec());
        const int nev = epoll_wait(ep, evs.data(), (int)evs.size(), (int)std::min(wait * 1000.0, simNet ? 5.0 : 50.0));   // acorda assim que algum bot recebe pacote
        const double tr = nowSec();
        for (int k = 0; k < nev; ++k) { const uint32_t i = evs[(size_t)k].data.u32; if (i < bots.size()) pumpBot(bots[i], o, tr); }
    }

    // despede-se da sala e resume
    double now = nowSec();
    for (Bot& b : bots) if (b.st == Bot::St::Connected) { uint8_t p[8]; Writer w(p, sizeof p); w.header(MsgLeave); sendto(b.fd, p, w.size(), 0, (sockaddr*)&b.sv, sizeof b.sv); }
    std::printf("\n=== RESUMO (%.0f s) ===\n", now - t0);
    int bad = 0;
    if (many) {
        long snaps = 0, rec = 0, rej = 0, never = 0; double rmin = 1e9, rsum = 0, rmax = 0, corr = 0; long rn = 0;
        for (const Bot& b : bots) {
            snaps += b.snaps; rec += b.reconnects; rej += b.rejects; if (!b.everConnected) ++never;
            if (b.rttN) { rmin = std::min(rmin, b.rttMin); rmax = std::max(rmax, b.rttMax); rsum += b.rttSum; rn += b.rttN; }
            corr = std::max(corr, b.corrMax);
        }
        std::printf("%d bots: conectaram %ld, nunca conectaram %ld | snapshots %ld (%.1f/s por bot) | rtt min/med/max %.0f/%.0f/%.0f ms | correcao_max %.2f m | reconexoes %ld recusas %ld | pior tick dos bots %.1f ms\n",
                    o.bots, (long)o.bots - never, never, snaps, snaps / std::max(1.0, now - t0) / o.bots, rn ? rmin : 0, rn ? rsum / rn : 0, rmax, corr, rec, rej, loopWorst * 1000.0);
        bad = (int)never;
    } else
    for (const Bot& b : bots) {
        std::printf("BOT%-2d %s  entradas=%ld snapshots=%ld rtt(min/med/max)=%.0f/%.0f/%.0f ms  correcao_max=%.2f m  reconexoes=%ld recusas=%ld  perdidos(saida/entrada)=%ld/%ld\n",
                    b.idx, b.everConnected ? "conectou" : "NAO CONECTOU", b.joins, b.snaps, b.rttN ? b.rttMin : 0, b.rttN ? b.rttSum / b.rttN : 0, b.rttMax,
                    b.corrMax, b.reconnects, b.rejects, b.dropOut, b.dropIn);
        if (b.fighter) std::printf("      LUTADOR: golpes que acertaram=%ld (dano causado %ld)\n", b.hitsDealt, b.dmgDealt);
        if (b.dmgTaken) std::printf("      dano levado=%ld, vida restante=%.0f\n", b.dmgTaken, b.health);
        if (b.victim || b.medic)
            std::printf("      %s: mortes=%ld salvo-por-outro=%ld renasceu-sozinho=%ld | pedidos de salvar=%ld carregou=%ld recusados=%ld\n", b.victim ? "VITIMA" : "MEDICO", b.deaths, b.revived, b.respawns, b.saves, b.carries, b.refused);
        if (!b.everConnected) ++bad;
    }
    if (bad) std::printf("FALHA: %d bot(s) nunca conectaram (servidor no ar? IP/porta certos? sala cheia?)\n", bad);
    return bad ? 1 : 0;
}
