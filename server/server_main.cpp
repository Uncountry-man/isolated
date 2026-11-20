// Isolated - servidor dedicado (Linux). Autoritativo: o cliente envia INPUTS, nunca posições.
// Uso: ./server [porta] [hora_inicial_s] [--debug] [--seed N] [--world arquivo.e0w] [--new] [--flat] [--data DIR] [--jwk ARQ] [--firebase-project ID]
//   Mundo: carrega 'worlds/world.e0w' se existir; senão gera (semente --seed, padrão 143) e SALVA. --new recria. --flat = sem terreno.
//   --data DIR: PERSISTÊNCIA (contas dos jogadores: moedas, inventário, vida/fome, bandeira, skin, posição, recibos; estado do mundo:
//   itens soltos, construções, baús). Sem --data as contas vivem só na memória (somem ao reiniciar). Ver CLAUDE.md "PERSISTÊNCIA".
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include "../shared/protocol.h"
#include "../shared/world_sync.h"
#include "jwt.h"
#include "persist.h"
#include "teams.h"

using namespace e0net;
using Clock = std::chrono::steady_clock;

static double nowSec() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// Orçamento de tempo anti speed-hack: o cliente só pode simular tanto tempo quanto passou de verdade.
// A reserva precisa absorver picos de latência do Wi-Fi: pacotes atrasados chegam em rajada e o tempo
// "acumulado" não pode ser perdido (senão o servidor fica atrás do cliente => teleporte de volta).
// Custo: um speed-hack pode ganhar no máximo kCreditMax segundos de movimento de uma vez.
constexpr double kCreditStart = 0.5;
constexpr double kCreditMax = 1.0;

struct Client {
    bool active = false;
    sockaddr_in addr{};
    PlayerState st;
    uint32_t lastSeq = 0;
    double lastRecv = 0;
    double credit = kCreditStart;  // segundos de simulação permitidos (anti speed-hack)
    double creditT = 0;
    double welcomeAt = -1;     // último Welcome enviado (Join repetido só reenvia a cada 0,25 s)
    uint32_t echoMs = 0;       // último clientMs recebido (devolvido no snapshot p/ medir RTT)
    double echoAt = 0;
    uint8_t lastAck[40]; size_t lastAckLen = 0;
    float reach = kInterestRadius;   // até onde procurar vizinhos neste tick (multidão: a distância do 40º no tick anterior, com folga)
    std::vector<uint16_t> seen;   // quem foi mandado no último snapshot (histerese da área de interesse: quem já estava continua)   // último MsgActAck (reenviado se o cliente repetir o pedido)
    uint32_t lastActSeq = 0;   // última MsgAct processada (ações confiáveis do inventário: em ordem, sem repetir)
    // morte / salvamento / carregar (ver StatusBits): o servidor decide e replica em PlayerState.status/link
    bool dead = false;
    int carriedBy = -1, carrying = -1;   // quem me carrega / quem eu carrego (slot, -1 = ninguém)
    int reviveTarget = -1; float reviveT = 0; float reviveSide = 1.0f;   // quem estou salvando (RCP), há quantos segundos e de que lado do corpo (±1)
    // combate corpo a corpo
    double speedUntil = 0, shieldUntil = 0;   // poções: velocidade e invulnerabilidade
    double protectUntil = 0;   // invencível até este instante (nascer/logar/renascer)
    double hitAt = -1, nextHitOk = 0; uint8_t hitHeld = 0;
    double nextSummon = 0; int lastSummonId = -1; uint8_t lastSummonRes[16] = {}; size_t lastSummonLen = 0;   // chamar navio: cooldown e último resultado (cópia repetida recebe o mesmo)
    double nextChat = 0; int lastChatId = -1;   // chat: próxima mensagem aceita e último chatId (as cópias repetidas são ignoradas)
    double nextGunShot = 0; int lastGunShot = -1;   // bacamarte: próximo tiro aceito e último shotId (as cópias repetidas são ignoradas)
    struct Hist { uint32_t tick = 0; float x = 0, y = 0, z = 0; bool hittable = false; };
    Hist hist[64];                                  // posição a cada tick (~2 s): compensação de latência dos tiros
    // CONTA (v42): inventário/moedas AUTORITATIVOS. Sem login = conta efêmera (kit inicial, nunca vai ao disco).
    std::shared_ptr<persist::Account> acct;
    uint32_t invRev = 1, invAckRev = 0; double invSentAt = 0;
    uint32_t sessionId = 0;                 // muda a cada entrada (o slot pode ser reaproveitado enquanto um login carrega do disco)
    int loginReq = -1; uint8_t loginReply[48]; size_t loginReplyLen = 0; double nextLogin = 0; bool loginPending = false;
    double joinedAt = 0;
    double nextCannon = 0; int lastShot = -1; bool wantGun = false, gunner = false;   // canhoes: pediu / tem um dos 2 lugares   // canhão: recarga e último shotId aceito   // impacto agendado (tempo do servidor) e o que estava na mão quando o golpe começou
    double adminFlyLogAt = -1e9; bool adminFlying = false;
    double nextStruct = 0; uint16_t lastStructReq = 0; uint8_t lastStructRes = 0; bool lastStructValid = false;   // MELHORIAS DA ILHA: último pedido (cópias = mesma resposta)
    // TRIPULAÇÃO no jogo: consulta de bandeiras, convite enviado (último id + resposta) e convite RECEBIDO pendente
    double nextTeamQuery = 0, nextInvite = 0; uint32_t lastInviteId = 0; uint8_t lastInviteRes = 0;
    uint32_t offerId = 0, offerTeam = 0, offerFromSession = 0, offerDoneId = 0; int offerFrom = -1; double offerUntil = 0, offerSentAt = 0;   // ADMIN: voo registrado no log (no máx. 1x/min por jogador)
};

static Client g_clients[kMaxPlayers];
static int g_fd = -1;
static volatile sig_atomic_t g_run = 1;
static uint32_t g_tick = 0;
static World g_world;                       // terreno (imutável depois de carregado => seguro entre threads)
static bool g_hasWorld = false;
static std::shared_ptr<std::vector<uint8_t>> g_worldBytes;
static uint32_t g_worldHash = 0;
static int g_tcp = -1;                      // escuta TCP do download do mundo (mesma porta do UDP)
static std::atomic<int> g_serving{0};
static bool g_ownAllShips = false;  // --own-all-ships: toda conta é dona de todos os navios (testes antigos; produção NUNCA)
static bool g_debugTime = false;  // --debug: aceita MsgDebugTime (nunca habilitar em produção)
static double g_cycleStart = kStartTimeSec, g_t0 = 0;  // relógio do mundo: hora do ciclo na partida + tempo real
static float cycleNow(double now) { return (float)std::fmod(g_cycleStart + (now - g_t0), (double)kCycleSec); }

static bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
static void sendTo(const sockaddr_in& to, const uint8_t* d, size_t n) {
    sendto(g_fd, d, n, 0, (const sockaddr*)&to, sizeof to);
}
// endereço (ip:porta) -> slot: com 1000 jogadores a busca linear por pacote (30 mil/s) custaria caro
static std::unordered_map<uint64_t, int> g_byAddr;
static uint64_t addrKey(const sockaddr_in& a) { return ((uint64_t)a.sin_addr.s_addr << 16) | a.sin_port; }
static Client* findClient(const sockaddr_in& a) {
    auto it = g_byAddr.find(addrKey(a));
    if (it == g_byAddr.end()) return nullptr;
    Client& c = g_clients[it->second];
    return c.active && sameAddr(c.addr, a) ? &c : nullptr;
}
// ======================= CONTAS / PERSISTÊNCIA (v42; ver persist.h e CLAUDE.md "PERSISTÊNCIA NO SERVIDOR") =======================
// O servidor é a fonte da verdade de moedas e inventário. Cada jogador tem uma conta: "fb:<uid>" (idToken do Firebase verificado) ou
// "anon:<uid local>" (visitante sem token: NÃO verificado, dados num espaço separado: quem só informa um uid do Firebase nunca cai na conta dele).
// Sem login a conta é efêmera (kit inicial, não vai ao disco). Contas carregadas ficam num cache em memória; a gravação é assíncrona.
static persist::Store g_store;
static bool g_dataOn = false;                       // --data DIR
static std::string g_jwkPath;                       // --jwk (padrão <data>/firebase_jwk.json)
static std::string g_fbProject = "isolated-multiplayer";
static jwt::KeySet g_keys; static double g_keysCheckAt = -1;
static bool g_rejectUnverified = false;             // --reject-unverified-receipts: só credita recibo VERIFICADO (exige a Google Play Developer API)
static std::unordered_map<std::string, std::shared_ptr<persist::Account>> g_accounts;   // cache (chave -> conta)
struct LoginWait { int slot; uint32_t sessionId; uint8_t reqId; uint8_t status; std::string prevLocal; };
static std::unordered_map<std::string, std::vector<LoginWait>> g_loading;              // contas sendo lidas do disco
static std::unordered_map<std::string, std::string> g_migrating;                       // "anon:<local>" -> conta verificada que vai receber
static uint32_t g_nextSession = 1;
static int64_t unixNow() { return (int64_t)std::time(nullptr); }

static std::shared_ptr<persist::Account> newEphemeral() {
    auto a = std::make_shared<persist::Account>();
    a->persistent = false; e0net::invStarterKit(a->inv); a->created = unixNow();
    return a;
}
static void markDirty(persist::Account& a, bool critical) {
    if (!a.persistent) return;
    if (!a.dirty) a.dirtySince = nowSec();
    a.dirty = true; a.critical = a.critical || critical;
}
static void saveAccount(persist::Account& a, bool sync) {
    if (!a.persistent) return;
    a.updated = unixNow(); a.dirty = false; a.critical = false;
    if (g_dataOn) g_store.save(a, sync);
}
static InvSlot* invOf(Client& c) { if (!c.acct) c.acct = newEphemeral(); return c.acct->inv; }

// ======================= ADMINS (comandos de debug/cheat sem --debug) =======================
// Arquivo de texto (--admins ARQ; padrão <data>/admins.txt): um uid do Firebase por linha ("fb:" opcional), '#' = comentário.
// Admin = conta "fb:<uid>" (idToken do Firebase VERIFICADO no login) cujo uid está no arquivo. Visitante ("anon:local-...") e conta
// efêmera NUNCA são admin, mesmo que o uid informado esteja no arquivo. Sem arquivo = ninguém. O arquivo é relido quando muda
// (stat a cada 2 s: mtime/tamanho/inode). Com --debug todos podem tudo (servidor de desenvolvimento), como antes.
static std::string g_adminsPath;
static std::unordered_map<std::string, bool> g_admins;   // uid -> true
static double g_adminsCheckAt = -1;
static bool g_adminsExists = false; static long long g_adminsSig[4] = {-1, -1, -1, -1};
static long g_debugIgnored = 0; static double g_debugIgnLogAt = -1e9;
static std::string utcStamp() {
    char b[32]; const std::time_t t = std::time(nullptr); std::tm tm{}; gmtime_r(&t, &tm);
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm); return b;
}
static void reloadAdmins(double now, bool force) {
    if (g_adminsPath.empty()) return;
    if (!force && now < g_adminsCheckAt) return;
    g_adminsCheckAt = now + 2.0;
    struct stat sb{};
    const bool exists = ::stat(g_adminsPath.c_str(), &sb) == 0;
    const long long sig[4] = {exists ? (long long)sb.st_ino : -1, exists ? (long long)sb.st_size : -1, exists ? (long long)sb.st_mtim.tv_sec : -1, exists ? (long long)sb.st_mtim.tv_nsec : -1};
    if (!force && exists == g_adminsExists && std::memcmp(sig, g_adminsSig, sizeof sig) == 0) return;
    g_adminsExists = exists; std::memcpy(g_adminsSig, sig, sizeof sig);
    g_admins.clear();
    int bad = 0;
    if (FILE* f = exists ? std::fopen(g_adminsPath.c_str(), "r") : nullptr) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string s(line);
            const size_t h = s.find('#'); if (h != std::string::npos) s.erase(h);
            size_t a = 0, b = s.size();
            while (a < b && std::isspace((unsigned char)s[a])) ++a;
            while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
            s = s.substr(a, b - a);
            if (s.empty()) continue;
            if (s.rfind("fb:", 0) == 0) s.erase(0, 3);
            bool ok = s.size() >= 1 && s.size() <= 128;
            for (char ch : s) if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '-')) ok = false;
            if (ok) g_admins[s] = true; else ++bad;
        }
        std::fclose(f);
    }
    std::printf("[admin] %s lista de admins %s: %zu uid(s) em '%s'%s\n", utcStamp().c_str(), exists ? "carregada" : "AUSENTE (ninguem e admin)", g_admins.size(), g_adminsPath.c_str(),
                bad ? (" [" + std::to_string(bad) + " linha(s) invalida(s) ignorada(s)]").c_str() : "");
    std::fflush(stdout);
}
// Conta VERIFICADA (fb:) e na lista. A chave "fb:" só é criada em onLogin depois de jwt::verifyFirebase dar Ok.
static bool isAdmin(const Client& c) {
    if (!c.acct || !c.acct->persistent || !c.acct->verified || c.acct->key.rfind("fb:", 0) != 0) return false;
    return g_admins.count(c.acct->key.substr(3)) != 0;
}
// Pode usar o comando de debug? --debug = todos (dev). Senão só admin, e cada uso vai ao log. Recusados: contados, log no máx. 1x/min.
static bool debugAllowed(Client& c, double now, const char* what, const char* fmt = "", ...) {
    if (g_debugTime) return true;
    const int si = (int)(&c - g_clients);
    reloadAdmins(now, false);
    if (isAdmin(c)) {
        char det[200]; va_list ap; va_start(ap, fmt); std::vsnprintf(det, sizeof det, fmt, ap); va_end(ap);
        std::printf("[admin] %s %s (jogador %d) %s %s\n", utcStamp().c_str(), c.acct->key.c_str(), si, what, det); std::fflush(stdout);
        return true;
    }
    ++g_debugIgnored;
    if (now - g_debugIgnLogAt >= 60.0) {
        std::printf("[debug] %s %ld comando(s) de debug IGNORADO(S) (sem --debug e sem admin); ultimo: jogador %d (%s) %s\n", utcStamp().c_str(), g_debugIgnored, si,
                    c.acct && c.acct->persistent ? c.acct->key.c_str() : "sem login", what);
        std::fflush(stdout); g_debugIgnored = 0; g_debugIgnLogAt = now;
    }
    return false;
}
static uint8_t crewShips(const persist::Account& a);   // navios comprados pela TRIPULAÇÃO da conta (mais abaixo)
static void sendInvSync(Client& c) {
    if (!c.acct) return;
    uint8_t buf[160]; Writer w(buf, sizeof buf);
    w.header(MsgInvSync); w.inv(c.invRev, c.acct->coins, c.acct->inv, g_ownAllShips ? 15 : (uint8_t)(c.acct->ships | (crewShips(*c.acct) << 4)));   // bits 0-3: meus; 4-7: da tripulação
    if (w.ok()) sendTo(c.addr, buf, w.size());
    c.invSentAt = nowSec();
}
// inventário/moedas mudaram: nova revisão, manda já (e de novo 1x/s até o MsgInvAck), grava depois (moedas: logo)
static void invChanged(Client& c, bool coinsChanged = false) {
    ++c.invRev; if (c.invRev == 0) c.invRev = 1;
    if (c.acct) markDirty(*c.acct, coinsChanged);
    sendInvSync(c);
}
static void endSession(int i) {   // saiu / caiu / foi derrubado: guarda posição e grava a conta
    Client& c = g_clients[i];
    if (!c.acct) return;
    persist::Account& a = *c.acct;
    if (a.persistent && a.session == i) {
        if (g_hasWorld && !c.dead && c.carriedBy < 0 && !c.st.ship) { a.hasPos = true; a.px = c.st.x; a.py = c.st.y; a.pz = c.st.z; }
        else a.hasPos = false;
        if (c.dead) a.health = 0;
        a.session = -1; a.lastUsed = nowSec();
        saveAccount(a, true);
    }
    c.acct.reset();
}
static void dropClient(int i) { endSession(i); g_byAddr.erase(addrKey(g_clients[i].addr)); g_clients[i].active = false; g_clients[i].seen.clear(); }
static int g_activeN = 0;   // jogadores conectados (atualizado a cada tick)
// Spawn: espiral de Vogel em volta do ponto de nascimento (com 1000 jogadores, ~40 m de raio em vez de todos no mesmo círculo de 2,5 m)
static void spawnPoint(int i, float& x, float& z) {
    const float ang = (float)i * 2.3999632f, r = 2.5f + 1.15f * std::sqrt((float)i);
    x = g_world.spawnX() + r * std::cos(ang); z = g_world.spawnZ() + r * std::sin(ang);
}
// Eventos (tiro, canhão): só para quem está perto do ponto (ou é um dos envolvidos)
static void sendNear(const uint8_t* buf, size_t n, float x, float z, float radius, int must1 = -1, int must2 = -1) {
    const float r2 = radius * radius;
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Client& o = g_clients[i];
        if (!o.active) continue;
        const float dx = o.st.x - x, dz = o.st.z - z;
        if (dx * dx + dz * dz <= r2 || i == must1 || i == must2) sendto(g_fd, buf, n, 0, (const sockaddr*)&o.addr, sizeof o.addr);
    }
}


// ---- Navios (ver shared/ship.h): um de cada tipo. Nascem na água perto da praia; o comando debug MsgDebugShip leva um deles para perto de quem pediu. ----
static float g_shipRespawnSec = kShipRespawnTime;   // --ship-respawn S (testes)
static bool g_shipsAtStart = false;                 // --ships-at-start (testes antigos): todos nascem ao subir, renascem sozinhos e não há limpeza
static float g_shipCleanSec = kShipCleanupSec;      // período da LIMPEZA (--ship-cleanup S, testes)
static float g_shipCleanIn = kShipCleanupSec;       // s até a próxima LIMPEZA (some com os navios que ninguém usa)
static float g_shipRespawnIn[kMaxShips];             // > 0: afundou, renasce em tantos s
static bool spawnShipNear(int kind, float nx, float nz, float minR) {
    if (!g_hasWorld) return false;
    float x, z, yaw;
    ShipState& S = shipState(kind);
    const bool wasAlive = shipAfloat(S);
    if (!wasAlive) S.exists = false;   // afundando/afundado: não conta como obstáculo na busca
    if (!shipFindSpawn(kind, terrain(), nx, nz, minR, x, z, yaw)) return false;
    S.exists = true; S.x = x; S.z = z; S.yaw = yaw; S.speed = 0; S.steer = S.cmdSteer = 0; S.cmdThrottle = 0; ++S.gen;
    if (!wasAlive) S.helmer = -1;
    S.hp = (int16_t)shipDef(kind).maxHp; S.sinkT = 0.0f; g_shipRespawnIn[kind] = 0.0f;   // nasce (ou renasce) com a vida cheia
    std::printf("[navio] %s nasceu em (%.0f, %.0f) yaw %.0f graus\n", shipDef(kind).name, x, z, yaw * 57.2958f); std::fflush(stdout);
    return true;
}
static void placeShip(int kind, float x, float z, float yaw) {   // (re)posiciona o navio com a vida cheia (gen novo: o cliente teleporta o desenho)
    ShipState& S = shipState(kind);
    S.exists = true; S.x = x; S.z = z; S.yaw = yaw; S.speed = 0; S.steer = S.cmdSteer = 0; S.cmdThrottle = 0; ++S.gen; S.helmer = -1;
    S.hp = (int16_t)shipDef(kind).maxHp; S.sinkT = 0.0f; g_shipRespawnIn[kind] = 0.0f; S.owner = -1;
}
static void shipReleaseHelm(int slot) {
    for (int i = 0; i < kMaxShips; ++i) {
        ShipState& S = shipState(i);
        if (S.helmer == slot) { S.helmer = -1; S.cmdSteer = 0; S.cmdThrottle = 0; }
    }
}
// Põe o jogador no navio idx, no ponto do mundo (x, z) (ou no convés mais perto dele)
static void shipPlace(PlayerState& st, int idx, float x, float z) {
    const ShipDef& D = shipDef(idx);
    shipWorldToLocal(shipState(idx), x, z, st.sa, st.sb);
    if (shipFloor(D, st.sa, st.sb) < kNoFloor * 0.5f) shipClampDeck(D, st.sa, st.sb);
    st.ship = 1; st.shipIdx = (uint8_t)idx;
    shipCarry(st);
}

// ---- Estado dinâmico das ILHAS (a inicial = slot 0 + as do oceano aberto, ver terrain.h): coletáveis/objetos já pegos (bitset sobre world.objects; o arquivo do mundo guarda só o inicial) ----
struct IslandState {
    World* w = nullptr;                 // slot 0 aponta para g_world; os demais são donos de `own`
    std::unique_ptr<World> own;
    std::vector<uint8_t> taken;         // bitset dos objetos pegos/derrubados
    std::vector<uint16_t> takenBy;      // por objeto: jogador que pegou + 1 (reenvia o OK se o pacote se perdeu)
    std::vector<uint8_t> hits;          // golpes recebidos por objeto (palmeira/coqueiro/tora/pedra), em meios-golpes
    bool anyTaken = false;
    uint32_t resetUntilTick = 0;        // após a renovação da meia-noite: continua mandando o bitset zerado até aqui (clientes aprendem que renasceu)
};
static IslandState g_is[kIslandSlots];
static World* worldOfSlot(int s) { return s >= 0 && s < kIslandSlots ? g_is[s].w : nullptr; }
static void sendTakenSync(const sockaddr_in& to, int only = -1) {   // um MsgTakenSync por ilha com algo pego (a inicial sempre: reseta o que o cliente achava); only >= 0: só essa ilha
    if (!g_hasWorld) return;
    for (int sl = 0; sl < kIslandSlots; ++sl) {
        if (only >= 0 && sl != only) continue;
        IslandState& is = g_is[sl];
        if (!is.w || (sl != 0 && !is.anyTaken && g_tick >= is.resetUntilTick)) continue;
        uint32_t n = (uint32_t)std::min<size_t>(is.w->objects.size(), kMaxTakenBits);
        uint8_t buf[kMaxPacket];
        Writer w(buf, sizeof buf);
        w.header(MsgTakenSync);
        w.put<uint8_t>((uint8_t)sl);
        w.put<uint32_t>(n);
        for (uint32_t i = 0; i < (n + 7) / 8; ++i) w.put<uint8_t>(i < is.taken.size() ? is.taken[i] : 0);
        if (w.ok()) sendTo(to, buf, w.size());
    }
}
static void broadcastTakenSync(int slot, float x, float z);

// RENOVAÇÃO À MEIA-NOITE (cycleSec 1350): todo recurso pego/derrubado de todas as ilhas volta (coletáveis, árvores, pedras, toras, minério).
static void renewResources() {
    int n = 0;
    for (int sl = 0; sl < kIslandSlots; ++sl) {
        IslandState& is = g_is[sl];
        if (!is.w) continue;
        if (is.anyTaken) for (uint8_t b : is.taken) n += __builtin_popcount(b);
        std::fill(is.taken.begin(), is.taken.end(), 0);
        std::fill(is.takenBy.begin(), is.takenBy.end(), 0);
        std::fill(is.hits.begin(), is.hits.end(), 0);
        if (is.anyTaken) is.resetUntilTick = g_tick + 30 * 8;
        is.anyTaken = false;
        is.w->clearAllRemoved();
    }
    for (const auto& c : g_clients) if (c.active) sendTakenSync(c.addr);
    std::printf("[mundo] meia-noite: %d recursos renasceram\n", n); std::fflush(stdout);
}

// mudança no bitset de uma ilha: na hora para quem está perto; os outros recebem na sincronia de 1x/s
static void broadcastTakenSync(int slot, float x, float z) {
    for (const auto& c : g_clients)
        if (c.active && (c.st.x - x) * (c.st.x - x) + (c.st.z - z) * (c.st.z - z) < kEventRadius * kEventRadius) sendTakenSync(c.addr, slot);
}

// ---- Morto / salvar / carregar ----
static float distXZ(const Client& a, const Client& b) { return std::sqrt((a.st.x - b.st.x) * (a.st.x - b.st.x) + (a.st.z - b.st.z) * (a.st.z - b.st.z)); }
static void releaseCarry(int i) {   // larga quem carrego / sou largado; o caído fica no chão à frente de quem carregava
    Client& c = g_clients[i];
    if (c.carrying >= 0) {
        Client& t = g_clients[c.carrying];
        t.carriedBy = -1;
        float x = c.st.x + std::sin(c.st.yaw) * 0.9f, z = c.st.z - std::cos(c.st.yaw) * 0.9f;
        if (g_hasWorld) { terrain().pushOut(x, z, kPlayerRadius); t.st.y = std::max(terrain().heightAt(x, z), World::kSwimFloorH); } else t.st.y = 0;
        t.st.x = x; t.st.z = z; t.st.vy = 0; t.st.speed = 0;
        t.st.ship = 0;
        if (c.st.ship) { shipPlace(t.st, c.st.shipIdx, x, z); t.st.y = shipFloor(shipDef(c.st.shipIdx), t.st.sa, t.st.sb); }
        c.carrying = -1;
    }
    if (c.carriedBy >= 0) { g_clients[c.carriedBy].carrying = -1; c.carriedBy = -1; }
}
static double g_spawnProtect = kSpawnProtectSec;
static bool isProtected(int j, double now) { return g_clients[j].active && (now < g_clients[j].protectUntil || now < g_clients[j].shieldUntil); }
static void clearLife(int i) {   // saiu / renasceu / foi salvo: limpa o estado de morto e as ligações
    Client& c = g_clients[i];
    c.hitAt = -1;
    shipReleaseHelm(i); if (c.st.ship == 2) c.st.ship = 1;
    releaseCarry(i);
    c.dead = false; c.reviveTarget = -1; c.reviveT = 0;
}
static void updateRescue(float dt) {   // 1x por tick: avança o salvamento, prende o carregado ao ombro e monta status/link de cada jogador
    for (int i = 0; i < kMaxPlayers; ++i) {
        Client& c = g_clients[i];
        if (!c.active) continue;
        if (c.reviveTarget >= 0) {
            Client& t = g_clients[c.reviveTarget];
            bool ok = t.active && t.dead && t.carriedBy < 0 && !c.dead && c.carrying < 0 && distXZ(c, t) <= kRescueMaxDist + 0.6f && c.st.speed < 1.0f;
            if (!ok) { c.reviveTarget = -1; c.reviveT = 0; }
            else {
                // posição de socorro: de joelhos ao LADO do corpo (0.8 m), de frente para ele
                float sx = std::cos(t.st.yaw), sz = std::sin(t.st.yaw);
                float px = t.st.x + c.reviveSide * sx * 0.8f, pz = t.st.z + c.reviveSide * sz * 0.8f;
                if (g_hasWorld) terrain().pushOut(px, pz, kPlayerRadius);
                c.st.x = px; c.st.z = pz; c.st.y = t.st.y; c.st.vy = 0; c.st.speed = 0;
                if (t.st.ship) { shipReleaseHelm(i); shipPlace(c.st, t.st.shipIdx, px, pz); } else c.st.ship = 0;
                c.st.yaw = std::atan2(t.st.x - px, -(t.st.z - pz));
                c.reviveT += dt;
                if (c.reviveT >= kReviveTime) {
                    t.dead = false;
                    std::printf("[salvo] jogador %d salvou o jogador %d\n", i, c.reviveTarget); std::fflush(stdout);
                    c.reviveTarget = -1; c.reviveT = 0;
                }
            }
        }
        if (c.carriedBy >= 0) {
            Client& k = g_clients[c.carriedBy];
            if (!k.active || !c.dead || k.dead) releaseCarry(i);
            else { c.st.x = k.st.x; c.st.y = k.st.y; c.st.z = k.st.z; c.st.yaw = k.st.yaw; c.st.vy = 0; c.st.speed = 0; shipReleaseHelm(i); c.st.ship = k.st.ship ? 1 : 0; c.st.shipIdx = k.st.shipIdx; c.st.sa = k.st.sa; c.st.sb = k.st.sb; }
        }
    }
    const double tn = nowSec();
    for (int i = 0; i < kMaxPlayers; ++i) {
        Client& c = g_clients[i];
        if (!c.active) continue;
        c.st.status = (uint8_t)((c.dead ? StDead : 0) | (c.carriedBy >= 0 ? StCarried : 0) | (c.carrying >= 0 ? StCarrying : 0) | (c.reviveTarget >= 0 ? StRescuing : 0) | (c.gunner ? StGunner : 0) | ((tn < c.protectUntil || tn < c.shieldUntil) ? StProtected : 0) | (tn < c.speedUntil ? StSpeed : 0));
        c.st.link = (uint16_t)(c.carriedBy >= 0 ? c.carriedBy + 1 : (c.carrying >= 0 ? c.carrying + 1 : (c.reviveTarget >= 0 ? c.reviveTarget + 1 : 0)));
    }
}

// ---- Dano entre jogadores ----
struct Dmg { uint32_t id; int to; uint8_t amount; uint16_t from; double sentAt, firstAt; };
static std::vector<Dmg> g_dmgs;
static uint32_t g_nextDmgId = 1;
static void sendDmg(const Dmg& d) {
    uint8_t buf[16]; Writer w(buf, sizeof buf);
    w.header(MsgDamage); w.put<uint32_t>(d.id); w.put<uint8_t>(d.amount); w.put<uint16_t>(d.from);
    sendTo(g_clients[d.to].addr, buf, w.size());
}
// ---- Canhões do navio ----
static int damageShip(int i, int dmg);
static bool sameCrew(int a, int b);   // TRIPULAÇÃO: sem fogo amigo (mais abaixo)
struct Shell { double at; float x, y, z; int from, fromShip; };
static std::vector<Shell> g_shells;
static void onCannon(Client& c, Reader& r, double now) {
    uint8_t shot = r.get<uint8_t>(); float tx = r.get<float>(), tz = r.get<float>();
    const ShipState& S = shipOf(c.st); const ShipDef& D = shipDef(S.kind);
    if (!r.ok() || !finite(tx) || !finite(tz) || !shipAfloat(S) || !D.hasCannons || !c.active || c.dead || c.st.ship != 1 || c.carriedBy >= 0 || !c.gunner) return;
    if (c.lastShot == (int)shot) return;            // cópia repetida do mesmo disparo
    if (now < c.nextCannon) return;                 // recarregando
    float dx = tx - S.x, dz = tz - S.z, d = std::sqrt(dx * dx + dz * dz);
    if (d < 0.01f) return;
    if (d < D.rangeMin || d > D.rangeMax) { float k = std::max(D.rangeMin, std::min(D.rangeMax, d)) / d; tx = S.x + dx * k; tz = S.z + dz * k; d = std::max(D.rangeMin, std::min(D.rangeMax, d)); }
    c.lastShot = (int)shot; c.nextCannon = now + kCannonReload;
    float a, b; shipWorldToLocal(S, tx, tz, a, b);
    const float side = b >= 0 ? 1.0f : -1.0f;
    float sx, sz; shipLocalToWorld(S, std::max(-D.muzzleA, std::min(D.muzzleA, a * 0.1f)), side * D.muzzleB, sx, sz);   // sai pelo costado do lado do alvo
    const float sy = D.deckY + 1.1f;
    const Terrain* W = terrainPtr();
    float ty = W ? std::max(0.0f, W->heightAt(tx, tz)) : 0.0f;
    const float flight = cannonFlightTime(d);
    g_shells.push_back({now + flight, tx, ty, tz, (int)(&c - g_clients), (int)c.st.shipIdx});
    uint8_t buf[48]; Writer w(buf, sizeof buf);
    w.header(MsgCannonFire); w.put<uint16_t>((uint16_t)(&c - g_clients));
    w.put<float>(sx); w.put<float>(sy); w.put<float>(sz); w.put<float>(tx); w.put<float>(ty); w.put<float>(tz); w.put<float>(flight);
    sendNear(buf, w.size(), (sx + tx) * 0.5f, (sz + tz) * 0.5f, kEventRadius + d * 0.5f, (int)(&c - g_clients));
    std::printf("[canhao] jogador %d disparou: alvo (%.0f, %.0f) a %.0f m, voo %.1f s\n", (int)(&c - g_clients), tx, tz, d, flight); std::fflush(stdout);
}
static void updateShells(double now) {
    for (size_t k = 0; k < g_shells.size();) {
        if (now < g_shells[k].at) { ++k; continue; }
        const Shell sh = g_shells[k]; g_shells.erase(g_shells.begin() + (long)k);
        int hitShip = -1, best = 0;   // casco atingido (o do próprio atirador não conta: fogo amigo no mesmo navio é ignorado)
        for (int i = 0; i < kMaxShips; ++i) {
            if (i == sh.fromShip) continue;
            int d = shipShellDamage(shipState(i), sh.x, sh.z);
            if (d > best) { best = d; hitShip = i; }
        }
        const int hpLeft = hitShip >= 0 ? damageShip(hitShip, best) : 0;
        uint8_t buf[32]; Writer w(buf, sizeof buf);
        w.header(MsgCannonHit); w.put<float>(sh.x); w.put<float>(sh.y); w.put<float>(sh.z); w.put<uint8_t>(hitShip >= 0 ? 3 : (sh.y > 0.3f ? 1 : 0));
        w.put<uint8_t>(hitShip >= 0 ? (uint8_t)hitShip : 255); w.put<int16_t>((int16_t)hpLeft); w.put<uint16_t>((uint16_t)sh.from);
        sendNear(buf, w.size(), sh.x, sh.z, kEventRadius, sh.from);
        for (int j = 0; j < kMaxPlayers; ++j) {   // estrago: jogadores vivos fora de navios, perto do impacto
            Client& t = g_clients[j];
            if (!t.active || t.dead || t.carriedBy >= 0 || t.st.ship != 0 || isProtected(j, now)) continue;
            float dx = t.st.x - sh.x, dz = t.st.z - sh.z, d = std::sqrt(dx * dx + dz * dz);
            if (d > kCannonRadius || std::fabs(t.st.y - sh.y) > 5.0f) continue;
            if (sh.from >= 0 && sh.from != j && sameCrew(sh.from, j)) continue;   // TRIPULAÇÃO: sem fogo amigo
            const int dmg = std::max(1, (int)(kCannonDamage * (1.0f - 0.45f * d / kCannonRadius)));
            g_dmgs.push_back({g_nextDmgId++, j, (uint8_t)dmg, (uint16_t)sh.from, 0, now});
            if (g_nextDmgId == 0) g_nextDmgId = 1;
            if (sh.from != j && g_clients[sh.from].active) {
                uint8_t b2[16]; Writer w2(b2, sizeof b2);
                w2.header(MsgHitConfirm); w2.put<uint16_t>((uint16_t)j); w2.put<uint8_t>((uint8_t)dmg);
                sendTo(g_clients[sh.from].addr, b2, w2.size());
            }
            std::printf("[canhao] bala do jogador %d acertou o jogador %d (-%d)\n", sh.from, j, dmg); std::fflush(stdout);
        }
    }
}
// ---- BACAMARTE (tiro de chumbo): o servidor decide tudo. Volta os outros para onde o atirador os via e testa cada chumbo ----
static bool histAt(const Client& t, float tick, float& x, float& y, float& z, bool& hittable) {
    const uint32_t a = (uint32_t)std::floor(tick), b = a + 1;
    const Client::Hist& ha = t.hist[a % 64]; const Client::Hist& hb = t.hist[b % 64];
    if (ha.tick != a) return false;
    if (hb.tick != b) { x = ha.x; y = ha.y; z = ha.z; hittable = ha.hittable; return true; }
    const float f = tick - (float)a;
    x = ha.x + (hb.x - ha.x) * f; y = ha.y + (hb.y - ha.y) * f; z = ha.z + (hb.z - ha.z) * f; hittable = ha.hittable && hb.hittable;
    return true;
}
// CHAMAR NAVIO (tela NAVEGAÇÃO): leva o navio `kind` para o mar aberto à frente do jogador. Recusa: sem mundo/navio, jogador morto/carregando/a bordo, navio em uso
// (alguém a bordo), afundando/afundado (renasce sozinho), cooldown de kSummonCooldown s, ou sem mar aberto em até kSummonMaxDist m à frente.
static bool islePierSpot(const Client& c, int kind, float& x, float& z, float& yaw);   // MELHORIAS DA ILHA (mais abaixo)
static void onSummon(Client& c, Reader& r, double now) {
    const uint8_t id = r.get<uint8_t>(), kind = r.get<uint8_t>();
    if (!r.ok() || kind >= kMaxShips) return;
    const int si = (int)(&c - g_clients);
    auto reply = [&](uint8_t res, uint16_t dist) {
        Writer w(c.lastSummonRes, sizeof c.lastSummonRes);
        w.header(MsgSummonResult); w.put<uint8_t>(id); w.put<uint8_t>(kind); w.put<uint8_t>(res); w.put<uint16_t>(dist);
        c.lastSummonLen = w.size();
        sendto(g_fd, c.lastSummonRes, c.lastSummonLen, 0, (const sockaddr*)&c.addr, sizeof c.addr);
    };
    if ((int)id == c.lastSummonId) { if (c.lastSummonLen) sendto(g_fd, c.lastSummonRes, c.lastSummonLen, 0, (const sockaddr*)&c.addr, sizeof c.addr); return; }   // cópia repetida: mesma resposta
    c.lastSummonId = (int)id;
    if (!g_hasWorld) { reply(SumNoShip, 0); return; }
    if (c.dead || c.carriedBy >= 0 || c.carrying >= 0) { reply(SumBusy, 0); return; }
    if (c.st.ship) { reply(SumAboard, 0); return; }
    const bool crewShip = c.acct && ((crewShips(*c.acct) >> kind) & 1u) && !((c.acct->ships >> kind) & 1u);   // da TRIPULAÇÃO (e não comprado por ele): sem cooldown
    if (!g_ownAllShips && !(c.acct && (((c.acct->ships | crewShips(*c.acct)) >> kind) & 1u))) { reply(SumNotOwned, 0); return; }   // a conta começa sem navio: precisa comprar
    ShipState& S = shipState(kind);
    if (S.exists && !shipAfloat(S)) { reply(SumSinking, 0); return; }
    if (S.exists && S.owner >= 0 && S.owner != si && S.owner < kMaxPlayers && g_clients[S.owner].active) { reply(SumInUse, 0); return; }   // o navio é de outro jogador
    // navio que não existe (nunca chamado, sumiu na limpeza ou já afundou): o chamado o cria à frente do jogador
    for (const auto& o : g_clients) if (o.active && o.st.ship && o.st.shipIdx == kind) { reply(SumInUse, 0); return; }
    if (!crewShip && now < c.nextSummon) { reply(SumCooldown, (uint16_t)std::min(65000.0, std::ceil(c.nextSummon - now))); return; }
    float x, z, yaw;
    const bool atPier = islePierSpot(c, kind, x, z, yaw);   // na ilha da tripulação com muralha: atraca na frente do CAIS
    if (!atPier && !shipFindAhead(kind, terrain(), c.st.x, c.st.z, c.st.yaw, x, z, yaw)) { reply(SumNoWater, 0); return; }
    shipReleaseHelm(si);
    placeShip(kind, x, z, yaw);
    S.owner = si;
    if (!crewShip) c.nextSummon = now + kSummonCooldown;
    const uint16_t dist = (uint16_t)std::min(65000.0f, std::hypot(x - c.st.x, z - c.st.z));
    reply(SumOk, dist);
    std::printf("[navio] jogador %d chamou o %s: agora em (%.0f, %.0f), a %d m %s\n", si, shipDef(kind).name, x, z, (int)dist, atPier ? "(ATRACADO NO CAIS)" : "a frente"); std::fflush(stdout);
}

// CHAT: limpa o texto, limita a 1 por kChatInterval s e difunde a quem está perto (inclusive quem falou: é a confirmação de entrega).
static void onChat(Client& c, Reader& r, double now) {
    const uint8_t chatId = r.get<uint8_t>(); const uint8_t len = r.get<uint8_t>();
    if (!r.ok() || len > 200) return;
    char raw[200];
    for (int i = 0; i < len; ++i) raw[i] = (char)r.get<uint8_t>();
    if (!r.ok()) return;
    if ((int)chatId == c.lastChatId) return;                  // cópia repetida da mesma mensagem
    char txt[kChatMaxLen + 1];
    const int n = chatSanitize(raw, len, txt);
    if (n == 0) return;
    if (now < c.nextChat) return;                             // rápido demais: descarta (o cliente não recebe eco)
    c.lastChatId = (int)chatId; c.nextChat = now + kChatInterval;
    const int si = (int)(&c - g_clients);
    uint8_t buf[16 + kChatMaxLen]; Writer w(buf, sizeof buf);
    w.header(MsgChatMsg); w.put<uint16_t>((uint16_t)si); w.put<uint8_t>((uint8_t)n);
    for (int i = 0; i < n; ++i) w.put<uint8_t>((uint8_t)txt[i]);
    sendNear(buf, w.size(), c.st.x, c.st.z, kChatRadius, si);
    std::printf("[chat] jogador %d: %s\n", si, txt); std::fflush(stdout);
}

static uint32_t g_gunRng = 0x2545F491u;
static float gunRnd() { g_gunRng ^= g_gunRng << 13; g_gunRng ^= g_gunRng >> 17; g_gunRng ^= g_gunRng << 5; return (float)(g_gunRng & 0xFFFFFF) / 16777216.0f; }
// ---- ÁREA DE INTERESSE: grade espacial dos jogadores (refeita a cada tick) ----
constexpr float kGridCell = 16.0f;
constexpr int kGridN = 640;                        // 640 x 16 m = 10,24 km (cobre o oceano de ±4,95 km)
constexpr float kGridOrigin = -kGridCell * kGridN * 0.5f;
static int g_gridHead[kGridN * kGridN];
static int g_gridNext[kMaxPlayers];
static int gridCoord(float v) { int c = (int)std::floor((v - kGridOrigin) / kGridCell); return c < 0 ? 0 : (c >= kGridN ? kGridN - 1 : c); }
static bool g_gridBuilt = false;
static void buildGrid() {
    g_gridBuilt = true;
    std::fill(g_gridHead, g_gridHead + kGridN * kGridN, -1);
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Client& c = g_clients[i];
        if (!c.active) continue;
        const int cell = gridCoord(c.st.z) * kGridN + gridCoord(c.st.x);
        g_gridNext[i] = g_gridHead[cell]; g_gridHead[cell] = i;
    }
}
static void onShoot(Client& c, Reader& r, double now) {
    const uint16_t shotId = r.get<uint16_t>();
    float ox = r.get<float>(), oy = r.get<float>(), oz = r.get<float>(), dx = r.get<float>(), dy = r.get<float>(), dz = r.get<float>();
    const float viewTick = r.get<float>(); const uint8_t flags = r.get<uint8_t>();
    const int si = (int)(&c - g_clients);
    if (!r.ok() || !finite(ox) || !finite(oy) || !finite(oz) || !finite(dx) || !finite(dy) || !finite(dz) || !finite(viewTick)) return;
    if ((int)shotId == c.lastGunShot) return;                                   // cópia repetida do mesmo tiro
    if (c.dead || c.carriedBy >= 0 || c.carrying >= 0 || c.st.held != HeldGun) return;
    if (now < c.nextGunShot) { std::printf("[tiro] jogador %d: tiro cedo demais (recarregando)\n", si); std::fflush(stdout); return; }
    c.protectUntil = 0;
    const float ex = ox - c.st.x, ey = oy - (c.st.y + 1.4f), ez = oz - c.st.z;
    if (ex * ex + ey * ey + ez * ez > kGunMaxOriginDist * kGunMaxOriginDist) { std::printf("[tiro] jogador %d: origem longe demais do corpo\n", si); std::fflush(stdout); return; }
    const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dl < 0.5f) return;
    dx /= dl; dy /= dl; dz /= dl;
    c.lastGunShot = (int)shotId; c.nextGunShot = now + kGunMinInterval;
    const float vt = std::max((float)g_tick - 20.0f, std::min((float)g_tick, viewTick));   // compensação de latência: até ~0,66 s para trás
    // base do cone
    float rx = -dz, rz = dx, rl = std::sqrt(rx * rx + rz * rz);
    if (rl < 1e-4f) { rx = 1; rz = 0; rl = 1; }
    rx /= rl; rz /= rl;
    const float ux = -dy * rz, uy = rz * dx - rx * dz, uz = dy * rx;   // up = right x d (ortonormal: right é horizontal e perpendicular a d)
    const float spread = (flags & 1) ? kGunSpreadAim : kGunSpreadHip;
    // Candidatos pela GRADE ESPACIAL (só as células em volta do segmento do tiro, com folga p/ a compensação de latência e a grade
    // de até 1 tick atrás) em vez de varrer os 1000 jogadores para cada chumbo.
    struct Tgt { float x, y, z; int j; int bodyDmg; bool head; float headDist; };
    static std::vector<Tgt> tg; tg.clear();
    if (!g_gridBuilt) buildGrid();
    {
        const float M = 25.0f + kGridCell, ex2 = ox + dx * kGunRange, ez2 = oz + dz * kGunRange;
        const int x0 = gridCoord(std::min(ox, ex2) - M), x1 = gridCoord(std::max(ox, ex2) + M), z0 = gridCoord(std::min(oz, ez2) - M), z1 = gridCoord(std::max(oz, ez2) + M);
        for (int gz = z0; gz <= z1; ++gz)
            for (int gx = x0; gx <= x1; ++gx)
                for (int j = g_gridHead[gz * kGridN + gx]; j >= 0; j = g_gridNext[j]) {
                    const Client& t = g_clients[j];
                    if (j == si || !t.active || t.dead || t.carriedBy >= 0) continue;
                    if ((t.st.x - ox) * (t.st.x - ox) + (t.st.z - oz) * (t.st.z - oz) > (kGunRange + 25.0f) * (kGunRange + 25.0f)) continue;   // longe demais (com folga p/ a compensação de latência)
                    if (sameCrew(si, j)) continue;   // TRIPULAÇÃO: o chumbo passa pelo tripulante (sem fogo amigo)
                    Tgt g{t.st.x, t.st.y, t.st.z, j, 0, false, 0.0f};
                    bool hit = true;
                    if (!histAt(t, vt, g.x, g.y, g.z, hit)) { g.x = t.st.x; g.y = t.st.y; g.z = t.st.z; }
                    if (hit) tg.push_back(g);
                }
    }
    uint8_t buf[512]; Writer w(buf, sizeof buf);
    w.header(MsgShotFx); w.put<uint16_t>((uint16_t)si); w.put<uint16_t>(shotId); w.put(ox); w.put(oy); w.put(oz); w.put<uint8_t>((uint8_t)kGunPellets);
    for (int k = 0; k < kGunPellets; ++k) {
        float px = dx, py = dy, pz = dz;
        if (k > 0 || !(flags & 1)) {   // mirando, o 1º chumbo vai exato no centro da mira (tiro na cabeça possível de longe)
            const float a = gunRnd() * 6.2831853f, rr = spread * std::sqrt(gunRnd());
            const float cr = std::cos(a) * rr, cu = std::sin(a) * rr;
            px += rx * cr + ux * cu; py += uy * cu; pz += rz * cr + uz * cu;
            const float l = std::sqrt(px * px + py * py + pz * pz); px /= l; py /= l; pz /= l;
        }
        uint8_t kind; float tEnd = gunTraceWorld(ox, oy, oz, px, py, pz, kGunRange, kind);
        int who = -1; bool isHead = false;
        for (int q = 0; q < (int)tg.size(); ++q) {
            const Tgt& g = tg[q];
            const float cx = g.x - ox, cy = g.y + 1.0f - oy, cz = g.z - oz, along = cx * px + cy * py + cz * pz;
            if (along < -1.0f || along > tEnd + 1.0f) continue;
            const float qx = cx - px * along, qy = cy - py * along, qz = cz - pz * along;
            if (qx * qx + qy * qy + qz * qz > 2.2f * 2.2f) continue;   // longe do raio
            const float th = rayHitSphere(ox, oy, oz, px, py, pz, g.x, g.y + kHeadY, g.z, kHeadR);
            const float tb = rayHitCapsule(ox, oy, oz, px, py, pz, g.x, g.y + kBodyY0, g.y + kBodyY1, g.z, kBodyR);
            if (th >= 0.0f && th <= tEnd && (tb < 0.0f || th <= tb + 0.05f)) { tEnd = th; who = q; isHead = true; }
            else if (tb >= 0.0f && tb <= tEnd) { tEnd = tb; who = q; isHead = false; }
        }
        if (who >= 0) {
            kind = isHead ? ShotHead : ShotBody;
            if (isHead) { tg[who].head = true; tg[who].headDist = std::max(tg[who].headDist, tEnd); } else tg[who].bodyDmg += gunBodyDamage(tEnd);
        }
        w.put(ox + px * tEnd); w.put(oy + py * tEnd); w.put(oz + pz * tEnd); w.put<uint8_t>(kind); w.put<uint16_t>(who >= 0 ? (uint16_t)tg[who].j : kNoPlayer);
    }
    sendNear(buf, w.size(), ox, oz, kEventRadius, si);
    int nHit = 0;
    for (const Tgt& g : tg) {
        const int j = g.j;
        if (!g.head && g.bodyDmg <= 0) continue;
        if (isProtected(j, now)) continue;
        const uint8_t dmg = g.head ? gunHeadDamage(g.headDist) : (uint8_t)std::min(254, g.bodyDmg);
        g_dmgs.push_back({g_nextDmgId++, j, dmg, (uint16_t)si, 0, now});
        if (g_nextDmgId == 0) g_nextDmgId = 1;
        uint8_t b2[16]; Writer w2(b2, sizeof b2);
        w2.header(MsgHitConfirm); w2.put<uint16_t>((uint16_t)j); w2.put<uint8_t>(dmg);
        sendTo(c.addr, b2, w2.size());
        std::printf("[tiro] jogador %d acertou o jogador %d %s (-%d)\n", si, j, g.head ? "NA CABECA" : "no corpo", (int)dmg); std::fflush(stdout);
        ++nHit;
    }
    if (!nHit) { std::printf("[tiro] jogador %d disparou (%s), sem acertar ninguem\n", si, (flags & 1) ? "mirando" : "do quadril"); std::fflush(stdout); }
}

static void updateCombat(double now) {
    updateShells(now);
    for (int i = 0; i < kMaxPlayers; ++i) {
        Client& a = g_clients[i];
        if (!a.active || a.hitAt < 0 || now < a.hitAt) continue;
        a.hitAt = -1;
        if (a.dead || a.carriedBy >= 0) continue;
        float fx = std::sin(a.st.yaw), fz = -std::cos(a.st.yaw);
        int best = -1; float bd = 1e9f;
        for (int j = 0; j < kMaxPlayers; ++j) {
            const Client& t = g_clients[j];
            if (j == i || !t.active || t.dead || t.carriedBy >= 0) continue;
            float dx = t.st.x - a.st.x, dz = t.st.z - a.st.z, d = std::sqrt(dx * dx + dz * dz);
            if (d > kMeleeReach || std::fabs(t.st.y - a.st.y) > 1.3f) continue;
            if (d > 0.05f && (fx * dx + fz * dz) / d < kMeleeCos) continue;   // fora do cone à frente
            if (sameCrew(i, j)) continue;   // TRIPULAÇÃO: sem fogo amigo
            if (d < bd) { bd = d; best = j; }
        }
        if (best < 0 || isProtected(best, now)) continue;
        a.protectUntil = 0;   // atacar tira a proteção
        const int dmg = meleeDamage(a.hitHeld);
        g_dmgs.push_back({g_nextDmgId++, best, (uint8_t)dmg, (uint16_t)i, 0, now});
        if (g_nextDmgId == 0) g_nextDmgId = 1;
        uint8_t buf[16]; Writer w(buf, sizeof buf);
        w.header(MsgHitConfirm); w.put<uint16_t>((uint16_t)best); w.put<uint8_t>((uint8_t)dmg);
        sendTo(a.addr, buf, w.size());
        std::printf("[dano] jogador %d acertou o jogador %d (-%d, na mao: %d)\n", i, best, dmg, (int)a.hitHeld); std::fflush(stdout);
    }
    for (size_t k = 0; k < g_dmgs.size();) {   // reenvia até o Ack (a cada 0.4 s, no máx. 8 s)
        Dmg& d = g_dmgs[k];
        if (!g_clients[d.to].active || now - d.firstAt > 8.0) { g_dmgs.erase(g_dmgs.begin() + (long)k); continue; }
        if (now - d.sentAt >= 0.4) { d.sentAt = now; sendDmg(d); }
        ++k;
    }
}

// Pedido de coleta: valida índice, tipo, se já foi pego e a distância do jogador (autoridade do servidor).
static void onCollect(Client& c, uint32_t gid) {
    const int sl = gidSlot(gid); const uint32_t idx = gidIdx(gid);
    World* W = worldOfSlot(sl);
    if (!g_hasWorld || !W || idx >= W->objects.size() || idx >= kMaxTakenBits) return;
    IslandState& is = g_is[sl];
    const WorldObject& o = W->objects[idx];
    if (!World::isCollectible(o.type)) return;
    if (is.taken.size() * 8 <= idx) is.taken.resize(idx / 8 + 1, 0);
    if (is.takenBy.size() <= idx) is.takenBy.resize(W->objects.size(), 0);
    const uint16_t slot = (uint16_t)((&c - g_clients) + 1);
    auto sendOk = [&]() {
        uint8_t buf[32];
        Writer w(buf, sizeof buf);
        w.header(MsgCollectOk);
        w.put<uint32_t>(gid);
        w.put<uint8_t>(o.type);
        sendTo(c.addr, buf, w.size());
    };
    if (is.taken[idx / 8] & (1u << (idx & 7))) {   // já pego: se foi ESTE jogador (OK perdido), reenvia; senão só atualiza o bitset dele
        if (is.takenBy[idx] == slot) sendOk(); else sendTakenSync(c.addr);
        return;
    }
    float dx = o.xcm * 0.01f - c.st.x, dz = o.zcm * 0.01f - c.st.z, dy = o.ycm * 0.01f - c.st.y;
    if (dx * dx + dz * dz > kCollectMaxDist * kCollectMaxDist || std::fabs(dy) > kCollectMaxDy) { sendTakenSync(c.addr); return; }
    const int item = o.type == ObjStick ? item::Stick : (o.type == ObjPebble ? item::Pebble : item::Cloth);
    if (!invFits(invOf(c), item, 1)) { sendTakenSync(c.addr); sendInvSync(c); return; }   // inventário (do servidor) cheio: o item fica no chão
    invAdd(invOf(c), item, 1); invChanged(c);
    is.taken[idx / 8] |= (uint8_t)(1u << (idx & 7));
    is.anyTaken = true;
    is.takenBy[idx] = slot;
    sendOk();
    broadcastTakenSync(sl, c.st.x, c.st.z);
    std::printf("[coleta] jogador %d pegou %s (obj %u, ilha %d)\n", (int)(&c - g_clients), o.type == ObjStick ? "graveto" : (o.type == ObjCloth ? "tecido" : "pedrinha"), idx, sl);
    std::fflush(stdout);
}

// ---- Itens soltos no chão (descartados / dropados pelos jogadores) + entregas entre jogadores ----
struct DropItem { uint16_t id; uint8_t item; uint16_t n; float x, y, z; };
static std::vector<DropItem> g_drops;
static uint16_t g_nextDropId = 1;
static void sendDropSync(const sockaddr_in& to) {
    uint8_t buf[kMaxPacket];
    Writer w(buf, sizeof buf);
    w.header(MsgDropSync);
    w.put<uint16_t>((uint16_t)g_drops.size());
    for (const DropItem& d : g_drops) { w.put<uint16_t>(d.id); w.put<uint8_t>(d.item); w.put<uint16_t>(d.n); w.put<float>(d.x); w.put<float>(d.y); w.put<float>(d.z); }
    if (w.ok()) sendTo(to, buf, w.size());
}
static float g_dropX = 0, g_dropZ = 0;   // onde mudou o chão (os itens soltos de perto chegam na hora; os outros na sincronia de 1x/s)
static void broadcastDropSync() { for (const auto& c : g_clients) if (c.active && (c.st.x - g_dropX) * (c.st.x - g_dropX) + (c.st.z - g_dropZ) * (c.st.z - g_dropZ) < kEventRadius * kEventRadius) sendDropSync(c.addr); }

// ---- Baús dos navios (conteúdo só na memória do servidor: reiniciar o servidor esvazia) ----
static Chest g_chests[kMaxShips];
static void sendChestSync(const sockaddr_in& to, int ship) {
    uint8_t buf[128];
    Writer w(buf, sizeof buf);
    w.header(MsgChestSync); w.chest(ship, g_chests[ship]);
    if (w.ok()) sendTo(to, buf, w.size());
}
static void broadcastChest(int ship) {   // quem está a bordo daquele navio ou perto dele
    const ShipState& S = shipState(ship);
    for (const auto& c : g_clients)
        if (c.active && ((c.st.ship && c.st.shipIdx == ship) || (c.st.x - S.x) * (c.st.x - S.x) + (c.st.z - S.z) * (c.st.z - S.z) < kEventRadius * kEventRadius)) sendChestSync(c.addr, ship);
}

// ---- Vida dos navios: dano e NAUFRÁGIO ----
// Afundar: o navio perde leme/velocidade e desce por kShipSinkTime s (o desenho aderna e afunda); quem está a bordo vai para a água ao lado do casco
// (sem dano), leme e canhões ficam livres e o que estava no BAÚ se perde. Depois de sumir, renasce em g_shipRespawnSec s com vida cheia.
static void startSink(int i) {
    ShipState& S = shipState(i);
    if (!shipAfloat(S)) return;
    S.hp = 0; S.sinkT = 1e-3f; S.helmer = -1; S.cmdSteer = 0; S.cmdThrottle = 0;
    for (auto& c : g_clients) if (c.active && c.st.ship && c.st.shipIdx == i) { shipEject(c.st); c.gunner = false; }
    if (chestCount(g_chests[i])) { Chest e; e.ver = (uint16_t)(g_chests[i].ver + 1); g_chests[i] = e; broadcastChest(i); }
    std::printf("[navio] %s AFUNDANDO\n", shipDef(i).name); std::fflush(stdout);
}
static int damageShip(int i, int dmg) {   // devolve a vida que ficou
    ShipState& S = shipState(i);
    if (!shipAfloat(S) || dmg == 0) return S.hp;
    S.hp = (int16_t)std::max(0, std::min(shipDef(i).maxHp, (int)S.hp - dmg));
    std::printf("[navio] %s levou %d de dano: vida %d/%d\n", shipDef(i).name, dmg, (int)S.hp, shipDef(i).maxHp); std::fflush(stdout);
    if (S.hp <= 0) startSink(i);
    return S.hp;
}

static uint32_t g_rng = 0x9E3779B9u;
static int rnd(int lo, int hi) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return lo + (int)(g_rng % (uint32_t)(hi - lo + 1)); }
// Solta um item no chão; junta com um já existente do mesmo tipo muito perto (evita encher o chão de pilhas de 1).
static void spawnDrop(uint8_t item, uint16_t n, float x, float z, float refY, bool merge) {
    if (merge) for (DropItem& d : g_drops) {
        float dx = d.x - x, dz = d.z - z;
        if (d.item == item && dx * dx + dz * dz < 0.8f * 0.8f && (int)d.n + n <= 99) { d.n = (uint16_t)(d.n + n); return; }
    }
    if (g_drops.size() >= (size_t)kMaxDrops) g_drops.erase(g_drops.begin());
    float y = g_hasWorld ? std::max(supportHeightAt(x, z, refY + 0.5f), refY - 1.0f) : 0.0f;
    if (g_hasWorld && std::fabs(y - refY) > 1.0f) y = refY;
    g_drops.push_back({g_nextDropId++, item, n, x, y, z});
    if (g_nextDropId == 0) g_nextDropId = 1;
}

struct Gift { uint32_t id; int to; uint16_t from; uint8_t item; uint16_t n; double sentAt; };
static std::vector<Gift> g_gifts;
static uint32_t g_nextGiftId = 1;
static void sendGift(const Gift& g) {
    uint8_t buf[32];
    Writer w(buf, sizeof buf);
    w.header(MsgGift);
    w.put<uint32_t>(g.id); w.put<uint16_t>(g.from); w.put<uint8_t>(g.item); w.put<uint16_t>(g.n);
    sendTo(g_clients[g.to].addr, buf, w.size());
}

// Ação confiável do inventário: processa em ordem (seq == último+1); repetição só reenvia o Ack.
static uint8_t teamBuyLicense(persist::Account& A, const char* who);   // TRIPULAÇÃO (mais abaixo)
static void onAct(Client& c, Reader& r) {
    uint32_t seq = r.get<uint32_t>(); uint8_t kind = r.get<uint8_t>(); uint16_t a = r.get<uint16_t>(), b = r.get<uint16_t>();
    if (!r.ok()) return;
    const int me = (int)(&c - g_clients);
    auto ack = [&](uint8_t result, uint8_t item, uint16_t n, uint16_t other) {
        uint8_t buf[40];
        Writer w(buf, sizeof buf);
        w.header(MsgActAck);
        w.put<uint32_t>(seq); w.put<uint8_t>(kind); w.put<uint8_t>(result); w.put<uint8_t>(item); w.put<uint16_t>(n); w.put<uint16_t>(other);
        w.put<uint32_t>(c.invRev);   // v42: revisão do inventário depois da ação (o cliente só adota MsgInvSync >= esta)
        std::memcpy(c.lastAck, buf, w.size()); c.lastAckLen = w.size();
        sendTo(c.addr, buf, w.size());
    };
    if (seq <= c.lastActSeq) { if (seq == c.lastActSeq && c.lastAckLen) sendTo(c.addr, c.lastAck, c.lastAckLen); return; }   // repetido (Ack perdido): reenvia o mesmo Ack
    // o cliente só tem 1 ação em voo por vez (em ordem), então qualquer seq maior é nova (tolera reconexão/ações abandonadas)
    c.lastActSeq = seq;
    InvSlot* inv = invOf(c);
    if (kind == ActDrop) {
        const uint8_t item = (uint8_t)a; const uint16_t n = b; const int slot = (int)(a >> 8) - 1;
        if (item == 0 || n == 0 || isSwimmingPos(c.st.x, c.st.y, c.st.z) || !invTake(inv, slot, item, n)) { ack(0, item, n, kNoPlayer); return; }   // não tem o item: recusa (o cliente readota o inventário do servidor)
        invChanged(c);
        if (g_drops.size() >= (size_t)kMaxDrops) g_drops.erase(g_drops.begin());   // cheio: o mais antigo some
        float x = c.st.x + std::sin(c.st.yaw) * kDropForward, z = c.st.z - std::cos(c.st.yaw) * kDropForward;
        if (g_hasWorld) g_world.pushOut(x, z, 0.15f);
        float y = g_hasWorld ? std::max(supportHeightAt(x, z, c.st.y + 0.5f), c.st.y - 1.0f) : 0.0f;
        if (g_hasWorld && std::fabs(y - c.st.y) > 1.0f) y = c.st.y;
        g_drops.push_back({g_nextDropId++, item, n, x, y, z});
        if (g_nextDropId == 0) g_nextDropId = 1;
        ack(1, item, n, kNoPlayer);
        g_dropX = c.st.x; g_dropZ = c.st.z; broadcastDropSync();
        std::printf("[drop] jogador %d soltou item %u x%u\n", me, (unsigned)item, (unsigned)n); std::fflush(stdout);
    } else if (kind == ActPickupDrop) {
        for (size_t i = 0; i < g_drops.size(); ++i) {
            if (g_drops[i].id != a) continue;
            float dx = g_drops[i].x - c.st.x, dz = g_drops[i].z - c.st.z;
            if (dx * dx + dz * dz > kCollectMaxDist * kCollectMaxDist || std::fabs(g_drops[i].y - c.st.y) > kCollectMaxDy) break;
            const int room = invRoomFor(inv, g_drops[i].item, g_drops[i].n);   // só o que cabe (peso/espaço); o resto fica no chão
            if (room <= 0) { ack(0, g_drops[i].item, 0, 1); return; }          // other 1 = inventário cheio
            invAdd(inv, g_drops[i].item, room); invChanged(c);
            ack(1, g_drops[i].item, (uint16_t)room, kNoPlayer);
            if (room < g_drops[i].n) g_drops[i].n = (uint16_t)(g_drops[i].n - room);
            else g_drops.erase(g_drops.begin() + (long)i);
            g_dropX = c.st.x; g_dropZ = c.st.z; broadcastDropSync();
            return;
        }
        ack(0, 0, 0, kNoPlayer); sendDropSync(c.addr);
    } else if (kind == ActGive || kind == ActGiveTo) {   // ENTREGAR (o mais perto) ou DAR para UM jogador (menu de segurar no jogador)
        uint8_t item; const uint16_t n = b; int slot, forced = -1;
        if (kind == ActGive) { item = (uint8_t)a; slot = (int)(a >> 8) - 1; }
        else { slot = (int)(a & 31); forced = (int)(a >> 5); item = slot < kInvSlots ? inv[slot].id : 0; }
        int best = -1; float bd = kGiveMaxDist * kGiveMaxDist;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (i == me || !g_clients[i].active || (forced >= 0 && i != forced)) continue;
            float dx = g_clients[i].st.x - c.st.x, dz = g_clients[i].st.z - c.st.z;
            float d = dx * dx + dz * dz;
            if (d <= bd && std::fabs(g_clients[i].st.y - c.st.y) < 3.0f) { bd = d; best = i; }
        }
        if (best < 0 || item == 0 || n == 0 || !invTake(inv, slot, item, n)) { ack(0, item, n, kNoPlayer); return; }
        invChanged(c);
        {   // quem recebe: entra no inventário (do servidor) dele já; o que não couber cai aos pés dele
            Client& t = g_clients[best]; InvSlot* ti = invOf(t);
            const int room = invRoomFor(ti, item, n);
            if (room > 0) { invAdd(ti, item, room); invChanged(t); }
            if (room < n) { spawnDrop(item, (uint16_t)(n - room), t.st.x, t.st.z, t.st.y, false); g_dropX = t.st.x; g_dropZ = t.st.z; broadcastDropSync(); }
        }
        Gift g{g_nextGiftId++, best, (uint16_t)me, item, n, 0};
        g_gifts.push_back(g);
        ack(1, item, n, (uint16_t)best);
        std::printf("[entrega] jogador %d -> jogador %d item %u x%u\n", me, best, (unsigned)item, (unsigned)n); std::fflush(stdout);
    } else if (kind == ActChestPut || kind == ActChestTake) {
        uint8_t item = 0; uint16_t n = 0; int ship = 0; uint16_t bb = b; bool okInv = true;
        if (kind == ActChestPut) {   // precisa ter o item (no slot indicado ou em qualquer lugar)
            const int slot = (int)(b >> 8) - 1, q = b & 0xFF, it = a & 0xFF;
            okInv = q > 0 && (slot >= 0 && slot < kInvSlots && inv[slot].id == it ? inv[slot].n >= q : invCount(inv, it) >= q);
        } else {                     // pegar: só o que cabe na mochila (peso/espaço)
            const int sh = a >> 8, sl = a & 0xFF;
            if (sh >= 0 && sh < kMaxShips && sl < kChestSlots && g_chests[sh].s[sl].id) bb = (uint16_t)invRoomFor(inv, g_chests[sh].s[sl].id, std::min<int>(b, g_chests[sh].s[sl].n)); else bb = 0;
            okInv = bb > 0;
        }
        const uint8_t res = (okInv && c.active && !c.dead && c.carriedBy < 0) ? chestAct(g_chests, c.st, kind, a, bb, item, n, &ship) : 0;
        if (res && kind == ActChestPut) { invTake(inv, (int)(b >> 8) - 1, item, n); invChanged(c); }
        else if (res) { const int left = invAddPreferHot(inv, item, n); if (left > 0) { uint8_t i2 = 0; (void)i2; chestPut(g_chests[ship], item, left); n = (uint16_t)(n - left); } invChanged(c); }
        ack(res, item, n, kNoPlayer);
        if (res) {
            broadcastChest(ship);
            std::printf("[bau] jogador %d %s item %u x%u (navio %d)\n", me, kind == ActChestPut ? "guardou" : "pegou", (unsigned)item, (unsigned)n, ship); std::fflush(stdout);
        } else if (ship >= 0 && ship < kMaxShips) sendChestSync(c.addr, ship);   // recusado: o cliente reve o conteúdo certo
    } else if (kind == ActHit) {
        const int sl = (int)b; const uint32_t idx = a;
        World* W = worldOfSlot(sl);
        if (!g_hasWorld || !W || idx >= W->objects.size() || idx >= kMaxTakenBits) { ack(0, 0, 0, 3); return; }
        IslandState& is = g_is[sl];
        const WorldObject& o = W->objects[idx];
        int need = 0; uint8_t tool = 0;
        switch (o.type) {
            case ObjCoconut: need = 3; tool = HeldHatchet; break;
            case ObjPalm: need = 4; tool = HeldHatchet; break;
            case ObjLog: need = 3; tool = HeldHatchet; break;
            case ObjRock: need = W->oreOf(idx) == 2 ? 8 : (W->oreOf(idx) == 1 ? 6 : (W->oreOf(idx) == 3 ? 5 : 4)); tool = HeldPickaxe; break;
            default: break;
        }
        if (!need) { ack(0, 0, 0, 3); return; }
        if (is.taken.size() * 8 <= idx) is.taken.resize(idx / 8 + 1, 0);
        if (is.takenBy.size() <= idx) is.takenBy.resize(W->objects.size(), 0);
        if (is.hits.size() < W->objects.size()) is.hits.resize(W->objects.size(), 0);
        if (is.taken[idx / 8] & (1u << (idx & 7))) { ack(0, 0, 0, 3); sendTakenSync(c.addr); return; }
        const bool dagger = c.st.held == HeldDagger && tool == HeldHatchet;   // a adaga de pedra também corta madeira, só que na METADE da velocidade (cada golpe vale 1 de 2)
        if (c.st.held != tool && !dagger) { ack(0, 0, 0, 2); return; }
        World::Collider cl = World::colliderOf(o);
        float cx = o.xcm * 0.01f, cz = o.zcm * 0.01f;
        if (cl.kind == 2) { float t = (c.st.x - cx) * cl.dx + (c.st.z - cz) * cl.dz; t = std::max(-cl.b, std::min(cl.b, t)); cx += cl.dx * t; cz += cl.dz * t; }
        float dd = std::sqrt((cx - c.st.x) * (cx - c.st.x) + (cz - c.st.z) * (cz - c.st.z)) - cl.a - kPlayerRadius;
        if (dd > kHitMaxReach || std::fabs(o.ycm * 0.01f - c.st.y) > 3.0f) { ack(0, 0, 0, 1); return; }
        is.hits[idx] = (uint8_t)(is.hits[idx] + (dagger ? 1 : 2));                      // contagem em meios-golpes
        const bool last = is.hits[idx] >= need * 2;
        const bool full = last || (is.hits[idx] % 2) == 0;                              // adaga: só solta material a cada 2 golpes
        // material deste golpe (sorteado): cai à frente do jogador, com pequena dispersão
        auto spill = [&](uint8_t item, int n) {
            if (n <= 0 || !full) return;
            float fx = std::sin(c.st.yaw), fz = -std::cos(c.st.yaw);
            float j1 = rnd(-40, 40) * 0.01f, j2 = rnd(-30, 30) * 0.01f;
            float x = c.st.x + fx * (kDropForward + j2) - fz * j1, z = c.st.z + fz * (kDropForward + j2) + fx * j1;
            terrain().pushOut(x, z, 0.15f);
            spawnDrop(item, (uint16_t)n, x, z, c.st.y, true);
        };
        if (o.type == ObjCoconut) spill(5, rnd(1, 2) + (last ? 1 : 0));               // COCO
        else if (o.type == ObjPalm) { if (last || rnd(0, 99) < 60) spill(3, 1); spill(6, rnd(1, 2)); }   // MADEIRA (tora) + FOLHA
        else if (o.type == ObjLog) spill(3, last ? 2 : 1);                             // MADEIRA
        else {                                                                         // PEDRINHA (+ minério nas pedras especiais)
            const uint8_t ore = W->oreOf(idx);   // 1 ouro (item 9), 2 diamante (item 10), 3 ferro (item 15)
            if (ore == 0) spill(2, rnd(1, 3) + (last ? 1 : 0));
            else {
                spill(2, rnd(0, 2));
                if (ore == 3) { if (last || rnd(0, 99) < 60) spill(15, last ? rnd(1, 3) : rnd(1, 2)); }
                else if (ore == 1) { if (last || rnd(0, 99) < 55) spill(9, last ? rnd(1, 2) : 1); }
                else { if (last) spill(10, rnd(1, 2)); else if (rnd(0, 99) < 30) spill(10, 1); }
            }
        }
        g_dropX = c.st.x; g_dropZ = c.st.z; broadcastDropSync();
        if (last) {
            is.taken[idx / 8] |= (uint8_t)(1u << (idx & 7));
            is.anyTaken = true;
            is.takenBy[idx] = (uint16_t)(me + 1);
            W->setRemoved(idx);
            broadcastTakenSync(sl, c.st.x, c.st.z);
        }
        ack(last ? 2 : 1, 0, 0, 0);
        std::printf("[golpe] jogador %d no objeto %u (ilha %d, tipo %u) golpe %d/%d (meios)%s\n", me, idx, sl, (unsigned)o.type, (int)is.hits[idx], need * 2, last ? " - ACABOU" : ""); std::fflush(stdout);
    } else if (kind == ActDie) {
        if (!c.dead) {
            releaseCarry(me); c.reviveTarget = -1; c.reviveT = 0; c.dead = true;
            e0net::invDeathLoss(inv); invChanged(c);   // morrer: perde TODOS os itens menos a adaga (as moedas ficam)
            if (c.acct) c.acct->health = 0;
            std::printf("[morte] jogador %d morreu\n", me); std::fflush(stdout);
        }
        ack(1, 0, 0, 0);
    } else if (kind == ActRevive || kind == ActCarry) {
        const int t = (int)a;
        bool ok = t >= 0 && t < kMaxPlayers && t != me && g_clients[t].active && g_clients[t].dead && g_clients[t].carriedBy < 0 && !c.dead && c.carrying < 0 && c.reviveTarget < 0
                  && distXZ(c, g_clients[t]) <= kRescueMaxDist + 0.4f;
        if (ok && kind == ActRevive) { c.reviveTarget = t; c.reviveT = 0; { const Client& tc = g_clients[t]; float sx = std::cos(tc.st.yaw), sz = std::sin(tc.st.yaw); c.reviveSide = ((c.st.x - tc.st.x) * sx + (c.st.z - tc.st.z) * sz) >= 0 ? 1.0f : -1.0f; } std::printf("[salvar] jogador %d comecou a salvar o jogador %d\n", me, t); }
        else if (ok) { g_clients[t].carriedBy = me; c.carrying = t; std::printf("[carregar] jogador %d carrega o jogador %d\n", me, t); }
        std::fflush(stdout);
        ack(ok ? 1 : 0, 0, 0, 0);
    } else if (kind == ActDropCarry) {
        bool ok = c.carrying >= 0;
        if (ok && g_hasWorld && !c.st.ship) {   // não se larga o corpo na água (nem de dentro dela): o corpo afundaria/boiaria
            float dx = c.st.x + std::sin(c.st.yaw) * 0.9f, dz = c.st.z - std::cos(c.st.yaw) * 0.9f;
            if (terrain().heightAt(c.st.x, c.st.z) < 0.0f || terrain().heightAt(dx, dz) < 0.0f) ok = false;
        }
        if (ok) releaseCarry(me);
        ack(ok ? 1 : 0, 0, 0, 0);
    } else if (kind == ActPotion) {
        const double nw = nowSec();
        bool ok = !c.dead && c.carriedBy < 0 && (a == PotionSpeed || a == PotionShield) && invTake(inv, (int)b - 1, a == PotionSpeed ? item::PotSpeed : item::PotShield, 1);
        if (ok) invChanged(c);
        if (ok && a == PotionSpeed) c.speedUntil = nw + kSpeedPotionSec;
        else if (ok && a == PotionShield) c.shieldUntil = nw + kShieldPotionSec;
        else ok = false;
        if (ok) { std::printf("[pocao] jogador %d usou pocao %d\n", me, (int)a); std::fflush(stdout); }
        ack(ok ? 1 : 0, 0, 0, 0);
    } else if (kind == ActCraft) {   // fabricar: confere e gasta os ingredientes AQUI (o cliente só mostra a barra de progresso)
        const int r = c.dead ? 1 : invCraft(inv, (int)a);
        int nr; const Recipe* R = recipeTable(nr);
        if (r == 0) invChanged(c);
        ack(r == 0 ? 1 : 0, (int)a < nr ? (uint8_t)R[a].out : 0, (int)a < nr ? (uint16_t)R[a].outN : 0, (uint16_t)r);
    } else if (kind == ActUse) {     // comer/beber/gastar 1 (coco, poções de cura, munição ao recarregar)
        const bool okItem = itemConsumable(b) || b == item::Ammo;
        const bool ok = okItem && !c.dead && invTake(inv, (int)a - 1, b, 1);
        if (ok) invChanged(c);
        ack(ok ? 1 : 0, (uint8_t)b, 1, 0);
    } else if (kind == ActInvMove || kind == ActInvSplit || kind == ActInvSort) {   // arrumar a mochila (mesmas regras do cliente)
        const int from = a & 0xFF, to = a >> 8;
        bool ok = true;
        if (kind == ActInvMove) invMove(inv, from, to); else if (kind == ActInvSplit) ok = invSplit(inv, from, to); else invSort(inv);
        invChanged(c);
        ack(ok ? 1 : 0, 0, 0, 0);
    } else if (kind == ActAdReward) {  // anúncio premiado: +kAdRewardCoins com limite de taxa por conta (não dá para provar que o anúncio passou sem o SSV do AdMob)
        persist::Account& A = *c.acct;
        const int64_t t = unixNow(); const int32_t day = (int32_t)(t / 86400);
        if (A.adDay != day) { A.adDay = day; A.adCount = 0; }
        const bool ok = A.adCount < kAdRewardPerDay && (double)(t - A.adLast) >= kAdRewardMinGap;
        if (ok) { A.adCount++; A.adLast = t; A.coins += kAdRewardCoins; invChanged(c, true); std::printf("[moedas] jogador %d +%d (anuncio %d/%d hoje)\n", me, kAdRewardCoins, (int)A.adCount, kAdRewardPerDay); std::fflush(stdout); }
        ack(ok ? 1 : 0, 0, ok ? (uint16_t)kAdRewardCoins : 0, ok ? 0 : (A.adCount >= kAdRewardPerDay ? 2 : 1));
    } else if (kind == ActBuyShip) {   // comprar um navio com moedas: a conta passa a ser dona dele (para sempre)
        persist::Account& A = *c.acct;
        const int sk = (int)a;
        uint16_t why = 0;
        if (sk < 0 || sk >= kMaxShips) why = 1;
        else if ((A.ships >> sk) & 1u) why = 2;                     // já tem
        else if (c.dead) why = 4;
        else if (A.coins < kShipPrice[sk]) why = 3;                 // sem moedas
        if (!why) {
            A.coins -= kShipPrice[sk]; A.ships |= (uint8_t)(1u << sk); invChanged(c, true);
            std::printf("[navio] jogador %d (%s) COMPROU o %s por %d moedas (saldo %d)\n", me, A.persistent ? A.key.c_str() : "sem login", shipDef(sk).name, kShipPrice[sk], (int)A.coins); std::fflush(stdout);
        }
        ack(why ? 0 : 1, (uint8_t)std::max(0, std::min(255, sk)), 0, why);
    } else if (kind == ActBuyTeamLicense) {   // LICENÇA DE TRIPULAÇÃO pela LOJA (a mesma regra da tela da tripulação no menu)
        char who[32]; std::snprintf(who, sizeof who, "jogador %d", me);
        const uint8_t res = teamBuyLicense(*c.acct, who);
        ack(res == TeamOk ? 1 : 0, 0, 0, res);
    } else ack(0, 0, 0, kNoPlayer);
}
static void resendGifts(double now) {
    for (size_t i = 0; i < g_gifts.size();) {
        Gift& g = g_gifts[i];
        if (!g_clients[g.to].active) { g_gifts.erase(g_gifts.begin() + (long)i); continue; }   // destinatário saiu
        if (now - g.sentAt >= 0.4) { g.sentAt = now; sendGift(g); }
        ++i;
    }
}

static void sendIslandOwners(const sockaddr_in& to);   // DOMINAÇÃO DE ILHAS (mais abaixo)
static void sendAllStructs(const sockaddr_in& to);     // MELHORIAS DA ILHA (mais abaixo)
static void sendWelcome(const Client& c, int idx) {
    uint8_t buf[64];
    Writer w(buf, sizeof buf);
    w.header(MsgWelcome);
    w.put<uint16_t>((uint16_t)idx);
    w.put(c.st.x); w.put(c.st.y); w.put(c.st.z);
    w.put<uint32_t>(g_worldHash);
    sendTo(c.addr, buf, w.size());
    sendTakenSync(c.addr);
    sendIslandOwners(c.addr);
    sendAllStructs(c.addr);
}

// ---- HANDSHAKE COM COOKIE (anti amplificação / anti IP falso) ----
// Join sem cookie válido => MsgChallenge (9 B, menor que o Join de kJoinPacketSize B). Só quem RECEBE o cookie no seu endereço entra.
// O cookie é um hash do endereço + segredo aleatório + janela de 10 s (vale a janela atual e a anterior): o servidor não guarda nada.
static uint64_t g_cookieSecret = 0;
static double g_joinTokens = 400.0, g_joinTokT = 0;          // entradas novas: até 400/s (rajada 400; os bots entram a 200/s)
static double g_chalTokens = 4000.0, g_chalTokT = 0;         // desafios: até 4000/s
static uint32_t joinCookie(const sockaddr_in& a, uint32_t win) {
    uint64_t x = g_cookieSecret ^ ((uint64_t)a.sin_addr.s_addr << 24) ^ ((uint64_t)a.sin_port << 8) ^ ((uint64_t)win * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    const uint32_t c = (uint32_t)x;
    return c ? c : 1u;
}
static bool takeToken(double& tok, double& t, double rate, double now) {
    tok = std::min(rate, tok + (now - t) * rate); t = now;
    if (tok < 1.0) return false;
    tok -= 1.0; return true;
}

static void onJoin(const sockaddr_in& from, double now, uint8_t skin, uint32_t cookie, size_t len) {
    if (len < (size_t)kJoinPacketSize) return;   // Join curto: sem resposta nenhuma (não serve de refletor)
    if (Client* c = findClient(from)) {  // Join repetido (Welcome perdido): reenvia, no máx. ~4x/s
        if (now - c->welcomeAt < 0.25) return;
        c->welcomeAt = now;
        c->st.skin = skin;
        sendWelcome(*c, (int)(c - g_clients));
        return;
    }
    const uint32_t win = (uint32_t)(now / 10.0);
    if (cookie == 0 || (cookie != joinCookie(from, win) && cookie != joinCookie(from, win - 1))) {
        if (!takeToken(g_chalTokens, g_chalTokT, 4000.0, now)) return;
        uint8_t buf[16]; Writer w(buf, sizeof buf); w.header(MsgChallenge); w.put<uint32_t>(joinCookie(from, win));
        sendTo(from, buf, w.size());
        return;
    }
    if (!takeToken(g_joinTokens, g_joinTokT, 400.0, now)) return;   // muitas entradas no mesmo segundo: o cliente repete o Join em 0,5 s
    for (int i = kFirstPublicId; i < kMaxPlayers; ++i) {
        if (g_clients[i].active) continue;
        Client& c = g_clients[i];
        c = Client{};
        c.active = true; c.addr = from; c.lastRecv = now; c.creditT = now; c.protectUntil = now + g_spawnProtect; c.welcomeAt = now;
        c.acct = newEphemeral(); c.sessionId = g_nextSession++; c.joinedAt = now; c.invRev = 1; c.invAckRev = 0;   // sem login: conta efêmera com o kit inicial
        g_byAddr[addrKey(from)] = i;
        c.st.id = (uint16_t)i; c.st.skin = skin;
        if (g_hasWorld) {  // em volta do ponto de nascimento da ilha (espiral: 1000 jogadores não nascem um em cima do outro)
            spawnPoint(i, c.st.x, c.st.z);
            terrain().pushOut(c.st.x, c.st.z, kPlayerRadius);
            c.st.y = std::max(terrain().heightAt(c.st.x, c.st.z), World::kSwimFloorH);
        } else {
            c.st.x = (float)((i % 4) * 3 - 4);  // mundo plano: spawn em fileira
            c.st.z = (float)(-(i / 4) * 3);
        }
        sendWelcome(c, i);
        sendDropSync(c.addr);
        for (int si = 0; si < kMaxShips; ++si) sendChestSync(c.addr, si);
        sendInvSync(c);
        std::printf("[+] jogador %d entrou (%s:%d) skin %d\n", i, inet_ntoa(from.sin_addr), ntohs(from.sin_port), (int)skin);
        std::fflush(stdout);
        return;
    }
    uint8_t buf[16];
    Writer w(buf, sizeof buf);
    w.header(MsgReject);
    w.put<uint8_t>(1);
    sendTo(from, buf, w.size());
}

static void onInput(Client& c, Reader& r, double now) {
    uint32_t clientMs = r.get<uint32_t>();
    uint8_t count = r.get<uint8_t>();
    if (!r.ok() || count > kMaxCmdsPerPacket) return;
    c.echoMs = clientMs; c.echoAt = now;
    c.credit = std::min(c.credit + (now - c.creditT), kCreditMax);
    c.creditT = now;
    for (int i = 0; i < count; ++i) {
        InputCmd cmd = r.cmd();
        if (!r.ok()) return;
        if (cmd.seq <= c.lastSeq) continue;  // já processado / duplicado
        if (!finite(cmd.moveX) || !finite(cmd.moveY) || !finite(cmd.camYaw) ||
            !finite(cmd.dt) || cmd.dt < 0) { c.lastSeq = cmd.seq; continue; }
        float dt = std::min(cmd.dt, 0.1f);
        dt = (float)std::min<double>(dt, c.credit);  // não simula mais tempo do que passou de verdade
        c.credit -= dt;
        cmd.dt = dt;
        if (cmd.flags & (InputFly | InputFlyFast | InputFlyDown)) {   // VOAR (aba GOD): só em servidor --debug ou para ADMIN
            if (g_debugTime) {}
            else if (isAdmin(c)) {
                if (!c.adminFlying && now - c.adminFlyLogAt >= 60.0) { debugAllowed(c, now, "VOAR", "(voo ligado)"); c.adminFlyLogAt = now; }
                c.adminFlying = true;
            } else { if (!c.adminFlying) debugAllowed(c, now, "VOAR"); c.adminFlying = true; cmd.flags &= (uint8_t)~(InputFly | InputFlyFast | InputFlyDown); }
        } else c.adminFlying = false;
        if (c.carrying >= 0 || c.dead) cmd.shipEv &= (uint8_t)(ShipEvBoard | ShipEvLeave);   // carregando (ou morto): sem leme, acelerador nem canhão
        const uint8_t actBefore = c.st.action;
        stepPlayer(c.st, cmd);
        c.lastSeq = cmd.seq;
        c.wantGun = (cmd.shipEv & ShipGunWant) != 0;
        if (((c.st.action >> 3) != (actBefore >> 3)) && !c.dead && c.carriedBy < 0 && c.carrying < 0 && now >= c.nextHitOk) {   // golpe novo (bater/cortar/minerar; coletar não conta)
            const uint8_t k = c.st.action & 7u;
            if (k != 0 && k != ActPickup) {
                c.hitHeld = c.st.held; c.nextHitOk = now + 0.35;
                c.hitAt = now + (k == ActMine ? (c.st.held == HeldPickaxe ? 0.85 : 0.55) : (k == ActChop ? 0.50 : 0.30));
            }
        }
    }
}

// ---- LOGIN ----
static void kickSession(int s, const char* why) {   // a mesma conta entrou em outro aparelho: derruba a sessão antiga (ela NÃO reconecta sozinha: MsgReject 2)
    Client& o = g_clients[s];
    uint8_t buf[16]; Writer w(buf, sizeof buf); w.header(MsgReject); w.put<uint8_t>(2);
    sendTo(o.addr, buf, w.size()); sendTo(o.addr, buf, w.size());
    std::printf("[conta] jogador %d derrubado: %s\n", s, why); std::fflush(stdout);
    clearLife(s); dropClient(s);
}
static void sendLoginReply(Client& c, uint8_t reqId, uint8_t status, uint8_t flags) {
    Writer w(c.loginReply, sizeof c.loginReply);
    const persist::Account* a = c.acct.get();
    w.header(MsgLoginResult); w.put<uint8_t>(reqId); w.put<uint8_t>(status); w.put<uint8_t>(flags);
    w.put<uint8_t>(a ? a->health : 100); w.put<uint8_t>(a ? a->hunger : 100); w.put<uint8_t>(a ? a->skin : 0);
    uint8_t fl[kFlagBytes] = {}; if (a && a->hasFlag) std::memcpy(fl, a->flag, sizeof fl);
    w.bytes(fl, sizeof fl);
    c.loginReplyLen = w.size(); c.loginReq = reqId;
    sendTo(c.addr, c.loginReply, c.loginReplyLen);
}
// liga a conta (já em memória) à sessão do jogador `slot`
static void attachAccount(int slot, const std::shared_ptr<persist::Account>& acc, uint8_t reqId, uint8_t status, uint8_t flags) {
    Client& c = g_clients[slot];
    if (acc->session >= 0 && acc->session != slot && acc->session < kMaxPlayers && g_clients[acc->session].active && g_clients[acc->session].acct == acc)
        kickSession(acc->session, "a mesma conta entrou de novo (outro aparelho/reconexao)");
    if (c.acct && c.acct != acc) endSession(slot);
    c.acct = acc; acc->session = slot; acc->lastUsed = nowSec(); acc->skin = c.st.skin;
    if (acc->health == 0) { acc->health = 100; acc->hunger = 100; acc->hasPos = false; }   // saiu morto: volta vivo no ponto de nascimento (os itens já tinham ido)
    if (acc->hasPos && g_hasWorld && !c.dead && !c.st.ship && c.carriedBy < 0 && nowSec() - c.joinedAt < 30.0 && finite(acc->px) && finite(acc->pz)) {   // última posição segura
        const float lim = Terrain::limit();
        float x = std::max(-lim, std::min(lim, acc->px)), z = std::max(-lim, std::min(lim, acc->pz));
        terrain().pushOut(x, z, kPlayerRadius);
        c.st.x = x; c.st.z = z; c.st.y = std::max(terrain().heightAt(x, z), World::kSwimFloorH); c.st.vy = 0; c.st.speed = 0;
        flags |= LoginRestoredPos;
    }
    if (acc->hasFlag) flags |= LoginHasFlag;
    c.loginPending = false;
    invChanged(c);
    sendLoginReply(c, reqId, status, flags);
    std::printf("[conta] jogador %d entrou na conta %s (%s) | moedas %d, %d itens\n", slot, acc->key.c_str(), acc->verified ? "verificada" : "NAO verificada", acc->coins,
                [&] { int k = 0; for (const auto& s : acc->inv) k += s.n; return k; }());
    std::fflush(stdout);
}
static void migrateLocal(const std::shared_ptr<persist::Account>& dst, persist::Account& src) {   // visitante local -> conta do Firebase (1x, só numa conta nova)
    if (!src.migratedTo.empty() || !dst->fresh) return;
    std::memcpy(dst->inv, src.inv, sizeof dst->inv);
    dst->coins += src.coins; dst->hasFlag = src.hasFlag; std::memcpy(dst->flag, src.flag, sizeof dst->flag);
    for (const auto& p : src.purchases) dst->purchases.push_back(p);
    dst->migratedFrom = src.key; dst->fresh = false;
    src.migratedTo = dst->key; src.coins = 0; invClear(src.inv); src.purchases.clear();
    markDirty(src, true); saveAccount(src, true);
    markDirty(*dst, true); saveAccount(*dst, true);
    std::printf("[conta] MIGRADA %s -> %s (%d moedas)\n", src.key.c_str(), dst->key.c_str(), dst->coins); std::fflush(stdout);
    if (dst->session >= 0 && g_clients[dst->session].active && g_clients[dst->session].acct == dst) invChanged(g_clients[dst->session], true);
}
static std::unordered_map<std::string, bool> g_teamFresh;   // contas NOVAS criadas por um pedido da TRIPULAÇÃO (ainda podem receber a migração no 1º login)
static void maybeMigrate(const std::shared_ptr<persist::Account>& acc, const std::string& prevLocal) {
    if (!acc->fresh || !acc->verified || prevLocal.empty()) return;   // migração 1x do visitante local
    const std::string src = "anon:" + prevLocal;
    auto s2 = g_accounts.find(src);
    if (s2 != g_accounts.end()) { if (s2->second->session < 0) migrateLocal(acc, *s2->second); }
    else if (g_dataOn) { g_migrating[src] = acc->key; g_store.requestLoad(src); }
}
static void finishLoad(persist::Store::Loaded& L) {
    auto it = g_loading.find(L.key);
    std::vector<LoginWait> waits; if (it != g_loading.end()) { waits = std::move(it->second); g_loading.erase(it); }
    auto mit = g_migrating.find(L.key);
    if (mit != g_migrating.end()) {   // leitura do visitante local para migrar
        auto dst = g_accounts.find(mit->second);
        if (L.found && dst != g_accounts.end()) { g_accounts[L.key] = L.acc; migrateLocal(dst->second, *L.acc); }
        g_migrating.erase(mit);
        if (waits.empty()) return;
    }
    std::shared_ptr<persist::Account> acc;
    auto cached = g_accounts.find(L.key);
    if (cached != g_accounts.end()) acc = cached->second;
    else if (L.corrupt) {   // arquivo corrompido: não cria conta nova por cima (os dados ficam guardados para recuperar à mão)
        for (const LoginWait& w : waits) if (w.slot >= 0 && g_clients[w.slot].active && g_clients[w.slot].sessionId == w.sessionId) sendLoginReply(g_clients[w.slot], w.reqId, LoginNoKeys, 0);
        return;
    } else if (L.found) acc = L.acc;
    else {
        acc = std::make_shared<persist::Account>();
        acc->key = L.key; acc->verified = L.key.rfind("fb:", 0) == 0; acc->created = unixNow(); acc->fresh = true;
        e0net::invStarterKit(acc->inv);
        markDirty(*acc, true);
    }
    g_accounts[L.key] = acc;
    for (const LoginWait& w : waits) {
        if (w.slot < 0) continue;   // leitura pedida pela TRIPULAÇÃO (serviço TCP do menu): o pedido é atendido em updateTeams
        if (!g_clients[w.slot].active || g_clients[w.slot].sessionId != w.sessionId) continue;
        attachAccount(w.slot, acc, w.reqId, w.status, (uint8_t)(acc->fresh ? LoginNewAccount : 0));
        maybeMigrate(acc, w.prevLocal);
    }
    if (acc->dirty) saveAccount(*acc, true);
}
static bool uidOk(const std::string& u) {
    if (u.size() < 6 || u.size() > (size_t)kMaxUidLen) return false;
    for (char ch : u) if (!(std::isalnum((unsigned char)ch) || ch == '-' || ch == '_')) return false;
    return true;
}
static void onLogin(Client& c, Reader& r, double now) {
    const uint8_t reqId = r.get<uint8_t>(), kind = r.get<uint8_t>();
    std::string uid, tok, prev;
    { const uint8_t n = r.get<uint8_t>(); for (int i = 0; i < n && r.ok(); ++i) uid += (char)r.get<uint8_t>(); }
    { const uint16_t n = r.get<uint16_t>(); if (n > kAccountMaxLen) return; for (int i = 0; i < n && r.ok(); ++i) tok += (char)r.get<uint8_t>(); }
    { const uint8_t n = r.get<uint8_t>(); for (int i = 0; i < n && r.ok(); ++i) prev += (char)r.get<uint8_t>(); }
    if (!r.ok()) return;
    if ((int)reqId == c.loginReq && c.loginReplyLen) { sendTo(c.addr, c.loginReply, c.loginReplyLen); return; }   // cópia: mesma resposta
    if (c.loginPending) return;                                          // já carregando do disco
    if (now < c.nextLogin) return;
    c.nextLogin = now + 0.5;
    const int si = (int)(&c - g_clients);
    std::string key; uint8_t status;
    if (kind == LoginFirebase) {
        if (g_keysCheckAt < 0 || now >= g_keysCheckAt) { g_keysCheckAt = now + 30.0; if (!g_jwkPath.empty()) g_keys.refresh(g_jwkPath); }
        jwt::Claims cl;
        const jwt::Result vr = jwt::verifyFirebase(tok, g_fbProject, g_keys, (double)unixNow(), cl);
        if (vr != jwt::Result::Ok) {
            const uint8_t st = vr == jwt::Result::Expired ? LoginExpired : (vr == jwt::Result::NoKeys || vr == jwt::Result::UnknownKid ? LoginNoKeys : LoginInvalid);
            std::printf("[conta] jogador %d: idToken recusado (%s)\n", si, jwt::resultName(vr)); std::fflush(stdout);
            if (vr == jwt::Result::UnknownKid) g_keysCheckAt = now;   // o Google pode ter girado as chaves: relê o arquivo no próximo pedido
            sendLoginReply(c, reqId, st, 0); c.loginReplyLen = 0; c.loginReq = -1;   // erro: o cliente pode tentar de novo (token novo)
            return;
        }
        key = "fb:" + cl.sub; status = LoginOk;
        if (!prev.empty() && !(prev.rfind("local-", 0) == 0 && uidOk(prev))) prev.clear();
    } else if (kind == LoginLocal) {
        if (uid.rfind("local-", 0) != 0 || !uidOk(uid)) { sendLoginReply(c, reqId, LoginBadUid, 0); return; }
        key = "anon:" + uid; status = LoginGuest; prev.clear();
    } else return;
    auto it = g_accounts.find(key);
    if (it != g_accounts.end()) {
        attachAccount(si, it->second, reqId, status, 0);
        if (g_teamFresh.erase(key)) maybeMigrate(it->second, prev);   // conta criada agora há pouco pela TRIPULAÇÃO (menu), ainda sem sessão
        return;
    }
    c.loginPending = true;
    g_loading[key].push_back({si, c.sessionId, reqId, status, prev});
    if (g_loading[key].size() == 1) {
        if (g_dataOn) g_store.requestLoad(key);
        else { persist::Store::Loaded L; L.key = key; finishLoad(L); }   // sem disco: conta nova (só em memória)
    }
}
// RECIBO da Play: credita moedas 1 vez por token (índice global). PONTO DE VERIFICAÇÃO: verifyPurchase (hoje sem credenciais = NÃO verificado).
enum class PurchaseCheck { Verified, Unverified, Invalid };
static PurchaseCheck verifyPurchase(const std::string& sku, const std::string& token) {
    // Para valer de verdade: chamar a Google Play Developer API (purchases.products.get, pacote com.rungo.isolatedMT, sku, token) com uma CONTA DE
    // SERVIÇO (JSON com chave privada) e conferir purchaseState == 0 e consumptionState. O servidor é C++ estático sem TLS: o caminho previsto é um
    // ajudante na VPS (script com curl + token OAuth da conta de serviço) que este servidor consulta. Nenhuma credencial foi criada.
    (void)sku;
    if (token.size() < 10 || token.size() > 1000) return PurchaseCheck::Invalid;
    if (token.rfind("test-", 0) == 0) return g_debugTime ? PurchaseCheck::Unverified : PurchaseCheck::Invalid;   // compra SIMULADA do modo teste: só em servidor --debug
    return PurchaseCheck::Unverified;
}
static void onAccount(Client& c, Reader& r) {
    const uint8_t kind = r.get<uint8_t>(); const uint16_t len = r.get<uint16_t>();
    if (!r.ok() || len > kAccountMaxLen || kind != AccPurchase) return;   // AccIdToken (v41) foi substituído pelo MsgLogin
    std::string v; v.reserve(len);
    for (int i = 0; i < len; ++i) v.push_back((char)r.get<uint8_t>());
    if (!r.ok()) return;
    const int si = (int)(&c - g_clients);
    const uint32_t h = fnv1a32(v.data(), v.size());
    auto reply = [&](uint8_t st, int32_t coins) {
        uint8_t buf[24]; Writer w(buf, sizeof buf);
        w.header(MsgAccountResult); w.put<uint8_t>(kind); w.put<uint8_t>(st); w.put<int32_t>(coins); w.put<uint32_t>(h);
        sendTo(c.addr, buf, w.size());
    };
    if (!c.acct || !c.acct->persistent) { reply(PurchNotLogged, 0); return; }   // sem conta ainda: o cliente manda de novo depois do login
    const size_t bar = v.find('|');
    const std::string sku = bar == std::string::npos ? std::string() : v.substr(0, bar), token = bar == std::string::npos ? std::string() : v.substr(bar + 1);
    const int coins = coinsForSku(sku.c_str());
    const PurchaseCheck pc = coins > 0 ? verifyPurchase(sku, token) : PurchaseCheck::Invalid;
    if (pc == PurchaseCheck::Invalid || (pc == PurchaseCheck::Unverified && g_rejectUnverified)) {
        std::printf("[compra] jogador %d: recibo RECUSADO (%s)\n", si, sku.c_str()); std::fflush(stdout); reply(PurchRejected, 0); return;
    }
    const std::string th = jwt::sha256Hex(token);
    if (const std::string* owner = g_store.receiptOwner(th)) { std::printf("[compra] jogador %d: recibo repetido (%s, ja creditado em %s)\n", si, sku.c_str(), owner->c_str()); std::fflush(stdout); reply(PurchDuplicate, 0); return; }
    persist::Account& A = *c.acct;
    persist::Purchase p; p.sku = sku; p.token = token; p.coins = coins; p.at = unixNow(); p.verified = pc == PurchaseCheck::Verified ? 1 : 0;
    A.purchases.push_back(p); A.coins += coins;
    g_store.appendReceipt(th, A.key, sku + " " + std::to_string(coins) + " " + std::to_string(p.at) + (p.verified ? " verificado" : " NAO_VERIFICADO"));
    invChanged(c, true); saveAccount(A, true);
    std::printf("[compra] jogador %d (%s): +%d moedas (%s, %s)\n", si, A.key.c_str(), coins, sku.c_str(), p.verified ? "verificado" : "NAO VERIFICADO: sem Google Play Developer API"); std::fflush(stdout);
    reply(PurchCredited, coins);
}
// 1x/s: grava contas sujas (moedas na hora; o resto após 10 s), reenvia o inventário não confirmado, descarrega contas paradas do cache
// ===================== TRIPULAÇÃO (serviço TCP do menu; ver shared/team.h e server/teams.h) =====================
// A thread do TCP só lê o pedido e espera a resposta; TODO o estado (contas, tripulações) é mexido aqui, na thread do jogo (updateTeams).
static teams::Registry g_teams;
static void teamChatNotify(teams::Team& t, const std::string& author, const std::string& text, const std::string& authorKey);
static std::unordered_map<std::string, std::string> g_teamNames;   // conta -> último nome de jogador visto (tela da tripulação); quem entra por convite usa este
static double g_teamsSaveAt = 0, g_teamsPruneAt = 0;
struct TeamJob {
    std::vector<uint8_t> req, resp;
    bool done = false;
    double at = 0;
    // depois de autenticado:
    bool authed = false; std::string key, kicked; e0net::TeamRequest R;
};
static std::mutex g_teamM; static std::condition_variable g_teamCv;
static std::deque<std::shared_ptr<TeamJob>> g_teamIn;     // thread TCP -> jogo
static std::vector<std::shared_ptr<TeamJob>> g_teamWait;  // esperando a conta ser lida do disco
static std::unordered_map<std::string, double> g_teamChatAt;
static long g_teamReqs = 0;

static void teamsSave(bool force) {
    if (!g_teams.dirty || !g_dataOn) { g_teams.dirty = false; return; }
    const double now = nowSec();
    if (!force && now < g_teamsSaveAt) return;
    g_teamsSaveAt = now + 2.0; g_teams.dirty = false;
    g_store.saveFile("teams.e0t", g_teams.serialize());
}
static void teamsLoad() {
    std::vector<uint8_t> d;
    const std::string p = g_store.dir() + "/teams.e0t";
    if (!persist::readFile(p, d)) { std::printf("Tripulacoes: nenhuma ainda (%s)\n", p.c_str()); return; }
    if (!g_teams.deserialize(d)) {
        const std::string bak = p + ".corrompido-" + std::to_string((long)std::time(nullptr));
        persist::writeAtomic(bak, d, true);
        std::fprintf(stderr, "[tripulacao] ERRO: %s invalido (copia em %s); comecando sem tripulacoes\n", p.c_str(), bak.c_str());
        return;
    }
    std::printf("Tripulacoes: %zu carregadas\n", g_teams.byId.size());
}
static void teamSystemMsg(teams::Team& t, const std::string& text) {
    teams::Msg m; m.id = t.nextMsg++; m.at = unixNow(); m.text = text.substr(0, 120);
    t.msgs.push_back(m); teams::Registry::prune(t, m.at); g_teams.dirty = true;
    teamChatNotify(t, "", m.text, "");
}
static std::string teamNewCode() {
    const char* A = e0net::teamCodeAlphabet(); const size_t n = std::strlen(A);
    static uint64_t st = 0x9E3779B97F4A7C15ull ^ (uint64_t)std::time(nullptr) ^ ((uint64_t)getpid() << 32);
    for (;;) {
        std::string c;
        for (int i = 0; i < e0net::kTeamCodeLen; ++i) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; c += A[st % n]; }
        if (!g_teams.byCode.count(c)) return c;
    }
}
static uint8_t crewShips(const persist::Account& a) { const teams::Team* t = a.persistent ? g_teams.ofKey(a.key) : nullptr; return t ? (uint8_t)(t->ships & 15u) : 0; }
static Client* teamOnlineClient(const persist::Account& a) {
    if (a.session < 0 || a.session >= kMaxPlayers) return nullptr;
    Client& c = g_clients[a.session];
    return c.active && c.acct.get() == &a ? &c : nullptr;
}
// Compra a LICENÇA (tela da TRIPULAÇÃO ou LOJA do jogo): gasta kTeamLicenseCost moedas. Devolve TeamResult.
static uint8_t teamBuyLicense(persist::Account& A, const char* who) {
    if (A.teamLicense) return TeamErrHasLicense;
    if (g_teams.ofKey(A.key)) return TeamErrInTeam;
    if (A.coins < kTeamLicenseCost) return TeamErrCoins;
    A.coins -= kTeamLicenseCost; A.teamLicense = 1;
    markDirty(A, true); if (A.persistent) saveAccount(A, true);
    if (Client* c = teamOnlineClient(A)) invChanged(*c, true);
    std::printf("[tripulacao] %s (%s) COMPROU a licenca por %d moedas (saldo %d)\n", who, A.persistent ? A.key.c_str() : "sem login", kTeamLicenseCost, (int)A.coins); std::fflush(stdout);
    return TeamOk;
}
static void teamInfoChangedKey(const std::string& key);
static bool teamKeyOnline(const std::string& key);
static void queuePush(const std::string& token, const std::string& title, const std::string& line, double now);
static void teamRemoveMember(teams::Team& t, int idx, const char* why) {
    const teams::Member m = t.members[(size_t)idx];
    t.members.erase(t.members.begin() + idx);
    g_teams.byKey.erase(m.key);
    if (t.members.empty()) { std::printf("[tripulacao] '%s' (%u) acabou (ninguem ficou)\n", t.name.c_str(), t.id); g_teams.remove(t.id); g_teams.dirty = true; return; }
    if (t.ownerKey == m.key) {   // o CAPITÃO abandonou: a tripulação é DESFEITA (todos saem; o código deixa de valer)
        const std::string name = t.name; const uint32_t id = t.id;
        teamChatNotify(t, "", "A TRIPULA\xC7\xC3" "O FOI DESFEITA: O CAPIT\xC3" "O " + m.name + " ABANDONOU", "");
        std::vector<std::string> keys;
        const double now = nowSec();
        for (const teams::Member& o : t.members) { keys.push_back(o.key); if (!teamKeyOnline(o.key)) queuePush(o.pushToken, name, "A tripula\xE7\xE3o foi desfeita: o capit\xE3o abandonou.", now); }
        std::printf("[tripulacao] '%s' (%u) DESFEITA: o capitao %s abandonou (%zu membro(s) liberado(s))\n", name.c_str(), id, m.key.c_str(), keys.size()); std::fflush(stdout);
        g_teams.remove(id); g_teams.dirty = true;
        for (const std::string& k : keys) teamInfoChangedKey(k);
        return;
    }
    teamSystemMsg(t, m.name + " " + why);
    g_teams.dirty = true;
}
static std::vector<uint8_t> teamReply(uint8_t result, const std::string& key, const persist::Account* A, uint32_t since) {
    e0net::TeamView v;
    if (A) { v.coins = A->coins; v.license = A->teamLicense != 0; }
    if (teams::Team* t = key.empty() ? nullptr : g_teams.ofKey(key)) {
        const int64_t now = unixNow();
        if (teams::Registry::prune(*t, now)) g_teams.dirty = true;
        v.inTeam = true; v.treasury = (int32_t)std::min<int64_t>(t->treasury, INT32_MAX); v.slotLevel = t->slotLevel; v.ships = t->ships; v.owner = t->ownerKey == key; v.teamId = t->id; v.name = t->name; v.code = t->code; std::memcpy(v.flag, t->flag, sizeof v.flag);
        for (const teams::Member& m : t->members) {
            e0net::TeamMember tm; tm.name = m.name; tm.owner = m.key == t->ownerKey; tm.me = m.key == key;
            auto ai = g_accounts.find(m.key); Client* oc = ai != g_accounts.end() ? teamOnlineClient(*ai->second) : nullptr; tm.online = oc != nullptr; tm.slot = oc ? (int)(oc - g_clients) : -1;
            v.members.push_back(tm);
        }
        std::stable_sort(v.members.begin(), v.members.end(), [](const e0net::TeamMember& a, const e0net::TeamMember& b) { return a.owner > b.owner; });
        v.lastMsgId = t->nextMsg - 1;
        size_t first = 0;
        while (first < t->msgs.size() && t->msgs[first].id <= since) ++first;
        if (t->msgs.size() - first > (size_t)kTeamMsgSend) first = t->msgs.size() - kTeamMsgSend;
        for (size_t i = first; i < t->msgs.size(); ++i) {
            const teams::Msg& m = t->msgs[i];
            e0net::TeamMsg tm; tm.id = m.id; tm.age = (uint32_t)std::max<int64_t>(0, now - m.at); tm.author = m.author; tm.text = m.text; tm.mine = !m.authorKey.empty() && m.authorKey == key;
            v.msgs.push_back(tm);
        }
    }
    e0net::TBufW w; w.put<uint8_t>(result); v.write(w);
    return w.b;
}
static void teamFinish(TeamJob& j, std::vector<uint8_t> resp) {
    std::lock_guard<std::mutex> l(g_teamM);
    j.resp = std::move(resp); j.done = true;
    g_teamCv.notify_all();
}
static void teamInfoChanged(int id);
static bool teamKeyOnline(const std::string& key) { auto it = g_accounts.find(key); return it != g_accounts.end() && teamOnlineClient(*it->second) != nullptr; }
static void teamInfoChangedKey(const std::string& key) {   // conta jogando agora: a TAG dela troca a bandeira na hora
    auto it = g_accounts.find(key);
    if (it != g_accounts.end()) if (Client* c = teamOnlineClient(*it->second)) { teamInfoChanged((int)(c - g_clients)); sendInvSync(*c); }   // a TAG troca e os navios da tripulação (mascara `ships`) chegam
}
// atende um pedido com a conta já na memória
static void teamHandle(TeamJob& j, const std::shared_ptr<persist::Account>& acc) {
    persist::Account& A = *acc;
    const e0net::TeamRequest& R = j.R;
    const double now = nowSec(); const int64_t unow = unixNow();
    A.lastUsed = now;
    if (A.fresh) g_teamFresh[A.key] = true;
    std::string pname = teamCleanText(R.playerName, kPlayerNameMax);
    if (pname.empty()) pname = "PIRATA";
    g_teamNames[j.key] = pname;
    teams::Team* t = g_teams.ofKey(j.key);
    if (t) { const int mi = t->memberOf(j.key); if (mi >= 0 && t->members[(size_t)mi].name != pname) { t->members[(size_t)mi].name = pname; g_teams.dirty = true; } }
    uint8_t res = TeamOk;
    char who[160]; std::snprintf(who, sizeof who, "%s '%s'", j.key.c_str(), pname.c_str());
    switch (R.op) {
        case TeamGet: break;
        case TeamBuyLicense: res = teamBuyLicense(A, who); break;
        case TeamCreate: {
            const std::string nm = teamCleanText(R.text, kTeamNameMax);
            if (t) res = TeamErrInTeam;
            else if (!A.teamLicense) res = TeamErrNoLicense;
            else if ((int)nm.size() < kTeamNameMin || (int)R.text.size() > 40) res = TeamErrName;
            else if (g_teams.nameTaken(nm, 0)) res = TeamErrNameTaken;
            else {
                auto nt = std::make_shared<teams::Team>();
                nt->id = g_teams.nextId++; nt->name = nm; nt->code = teamNewCode(); nt->ownerKey = j.key; nt->created = unow;
                std::memcpy(nt->flag, R.flag, sizeof nt->flag);
                nt->members.push_back({j.key, pname, unow, A.pushToken});
                g_teams.add(nt);
                teamSystemMsg(*nt, pname + " FUNDOU A TRIPULA\xC7\xC3" "O " + nm);
                A.teamLicense = 0; markDirty(A, true); if (A.persistent) saveAccount(A, true);
                g_teams.dirty = true;
                std::printf("[tripulacao] %s CRIOU '%s' (id %u, codigo %s)\n", who, nm.c_str(), nt->id, nt->code.c_str()); std::fflush(stdout);
            }
            break;
        }
        case TeamJoin: {
            teams::Team* jt = g_teams.ofCode(teamCleanCode(R.text));
            if (t) res = TeamErrInTeam;
            else if (!jt) res = TeamErrCode;
            else if ((int)jt->members.size() >= jt->capacity()) res = TeamErrFull;
            else {
                jt->members.push_back({j.key, pname, unow, A.pushToken}); g_teams.byKey[j.key] = jt->id;
                teamSystemMsg(*jt, pname + " ENTROU NA TRIPULA\xC7\xC3" "O");
                std::printf("[tripulacao] %s ENTROU em '%s' (%u)\n", who, jt->name.c_str(), jt->id); std::fflush(stdout);
            }
            break;
        }
        case TeamLeave:
            if (!t) res = TeamErrNoTeam;
            else { std::printf("[tripulacao] %s SAIU de '%s' (%u)\n", who, t->name.c_str(), t->id); std::fflush(stdout); teamRemoveMember(*t, t->memberOf(j.key), "SAIU"); }
            break;
        case TeamRename: {
            const std::string nm = teamCleanText(R.text, kTeamNameMax);
            if (!t) res = TeamErrNoTeam;
            else if (t->ownerKey != j.key) res = TeamErrNotOwner;
            else if ((int)nm.size() < kTeamNameMin || (int)R.text.size() > 40) res = TeamErrName;
            else if (g_teams.nameTaken(nm, t->id)) res = TeamErrNameTaken;
            else if (nm != t->name) { t->name = nm; teamSystemMsg(*t, "NOVO NOME: " + nm); }
            break;
        }
        case TeamSetFlag:
            if (!t) res = TeamErrNoTeam;
            else if (t->ownerKey != j.key) res = TeamErrNotOwner;
            else if (std::memcmp(t->flag, R.flag, sizeof t->flag) != 0) { std::memcpy(t->flag, R.flag, sizeof t->flag); teamSystemMsg(*t, pname + " TROCOU A BANDEIRA"); }
            break;
        case TeamChat: {
            const std::string tx = teamCleanText(R.text, kTeamMsgMax);
            if (!t) res = TeamErrNoTeam;
            else if (tx.empty()) res = TeamErrText;
            else if (now - g_teamChatAt[j.key] < kTeamChatGap) res = TeamErrSlow;
            else {
                g_teamChatAt[j.key] = now;
                teams::Msg m; m.id = t->nextMsg++; m.at = unow; m.authorKey = j.key; m.author = pname; m.text = tx;
                t->msgs.push_back(m); teams::Registry::prune(*t, unow); g_teams.dirty = true;
                teamChatNotify(*t, pname, tx, j.key);
                std::printf("[tripulacao] '%s' %s: %s\n", t->name.c_str(), pname.c_str(), tx.c_str()); std::fflush(stdout);
            }
            break;
        }
        case TeamKick: {
            // índice = posição na lista ENVIADA (capitão primeiro, depois a ordem de entrada)
            if (!t) { res = TeamErrNoTeam; break; }
            if (t->ownerKey != j.key) { res = TeamErrNotOwner; break; }
            std::vector<int> order; for (size_t i = 0; i < t->members.size(); ++i) if (t->members[i].key == t->ownerKey) order.push_back((int)i);
            for (size_t i = 0; i < t->members.size(); ++i) if (t->members[i].key != t->ownerKey) order.push_back((int)i);
            if (R.index >= order.size() || t->members[(size_t)order[R.index]].key == j.key) { res = TeamErrBadReq; break; }
            if (!R.text.empty() && teamCleanText(R.text, kPlayerNameMax) != t->members[(size_t)order[R.index]].name) { res = TeamErrBadReq; break; }   // a lista mudou: não expulsa a pessoa errada
            std::printf("[tripulacao] %s EXPULSOU %s de '%s'\n", who, t->members[(size_t)order[R.index]].key.c_str(), t->name.c_str()); std::fflush(stdout);
            j.kicked = t->members[(size_t)order[R.index]].key;
            teamRemoveMember(*t, order[R.index], "FOI EXPULSO");
            break;
        }
        case TeamPromote: {   // o capitão passa o cargo (índice = posição na lista enviada)
            if (!t) { res = TeamErrNoTeam; break; }
            if (t->ownerKey != j.key) { res = TeamErrNotOwner; break; }
            std::vector<int> order; for (size_t i = 0; i < t->members.size(); ++i) if (t->members[i].key == t->ownerKey) order.push_back((int)i);
            for (size_t i = 0; i < t->members.size(); ++i) if (t->members[i].key != t->ownerKey) order.push_back((int)i);
            if (R.index >= order.size() || t->members[(size_t)order[R.index]].key == j.key) { res = TeamErrBadReq; break; }
            if (!R.text.empty() && teamCleanText(R.text, kPlayerNameMax) != t->members[(size_t)order[R.index]].name) { res = TeamErrBadReq; break; }
            const teams::Member nm = t->members[(size_t)order[R.index]];
            t->ownerKey = nm.key; g_teams.dirty = true;
            teamSystemMsg(*t, nm.name + " AGORA \xC9 O CAPIT\xC3" "O");
            std::printf("[tripulacao] %s passou o cargo de capitao de '%s' para %s\n", who, t->name.c_str(), nm.key.c_str()); std::fflush(stdout);
            break;
        }
        case TeamDeposit: {   // qualquer membro põe moedas dele no cofre
            if (!t) { res = TeamErrNoTeam; break; }
            const int64_t amt = R.amount;
            if (amt < 1 || amt > 1000000) { res = TeamErrAmount; break; }
            if (A.coins < amt) { res = TeamErrCoins; break; }
            A.coins -= (int32_t)amt; t->treasury += amt; markDirty(A, true); if (A.persistent) saveAccount(A, true);
            if (Client* oc = teamOnlineClient(A)) invChanged(*oc, true);
            teamSystemMsg(*t, pname + " DEPOSITOU " + std::to_string(amt) + " MOEDAS NO COFRE");
            std::printf("[tripulacao] %s depositou %lld em '%s' (cofre %lld)\n", who, (long long)amt, t->name.c_str(), (long long)t->treasury); std::fflush(stdout);
            g_teams.dirty = true; break;
        }
        case TeamWithdraw: {   // só o capitão tira do cofre (vai para a conta dele)
            if (!t) { res = TeamErrNoTeam; break; }
            if (t->ownerKey != j.key) { res = TeamErrNotOwner; break; }
            const int64_t amt = R.amount;
            if (amt < 1 || amt > 1000000) { res = TeamErrAmount; break; }
            if (t->treasury < amt) { res = TeamErrTreasury; break; }
            t->treasury -= amt; A.coins += (int32_t)amt; markDirty(A, true); if (A.persistent) saveAccount(A, true);
            if (Client* oc = teamOnlineClient(A)) invChanged(*oc, true);
            teamSystemMsg(*t, pname + " SACOU " + std::to_string(amt) + " MOEDAS DO COFRE");
            std::printf("[tripulacao] %s sacou %lld de '%s' (cofre %lld)\n", who, (long long)amt, t->name.c_str(), (long long)t->treasury); std::fflush(stdout);
            g_teams.dirty = true; break;
        }
        case TeamBuy: {   // LOJA da tripulação: o capitão paga com o COFRE
            if (!t) { res = TeamErrNoTeam; break; }
            if (t->ownerKey != j.key) { res = TeamErrNotOwner; break; }
            int price = 0; std::string what;
            if (R.index == 0) {
                if (t->slotLevel >= kTeamSlotLevels) { res = TeamErrMaxed; break; }
                price = kTeamSlotPrice[t->slotLevel]; what = "+" + std::to_string(kTeamSlotStep) + " VAGAS";
            } else if (R.index >= 1 && R.index <= kTeamShipKinds) {
                const int k = R.index - 1;
                if ((t->ships >> k) & 1u) { res = TeamErrOwned; break; }
                price = kTeamShipPrice[k]; what = std::string("O NAVIO ") + shipDef(k).name;
            } else { res = TeamErrBadReq; break; }
            if (t->treasury < price) { res = TeamErrTreasury; break; }
            t->treasury -= price;
            if (R.index == 0) t->slotLevel++; else t->ships |= (uint8_t)(1u << (R.index - 1));
            teamSystemMsg(*t, pname + " COMPROU " + what + " (" + std::to_string(price) + ")");
            std::printf("[tripulacao] %s comprou %s para '%s' por %d (cofre %lld)\n", who, what.c_str(), t->name.c_str(), price, (long long)t->treasury); std::fflush(stdout);
            g_teams.dirty = true;
            for (const teams::Member& m : t->members) teamInfoChangedKey(m.key);   // navios novos chegam já ao jogo de quem está online
            break;
        }
        case TeamPushToken: {   // token FCM do aparelho (notificações com o jogo fechado)
            const std::string& tk = R.text;
            bool ok = tk.size() <= 250;
            for (char ch : tk) if (!(std::isalnum((unsigned char)ch) || ch == ':' || ch == '-' || ch == '_')) ok = false;
            if (!ok) { res = TeamErrBadReq; break; }
            if (A.pushToken != tk) { A.pushToken = tk; markDirty(A, false); }
            if (t) { const int mi = t->memberOf(j.key); if (mi >= 0 && t->members[(size_t)mi].pushToken != tk) { t->members[(size_t)mi].pushToken = tk; g_teams.dirty = true; } }
            break;
        }
        default: res = TeamErrBadReq; break;
    }
    if (res == TeamOk) {
        if (R.op == TeamCreate || R.op == TeamJoin || R.op == TeamLeave) teamInfoChangedKey(j.key);
        if (R.op == TeamKick && !j.kicked.empty()) teamInfoChangedKey(j.kicked);
        if (R.op == TeamSetFlag) if (teams::Team* t2 = g_teams.ofKey(j.key)) for (const teams::Member& m : t2->members) teamInfoChangedKey(m.key);
    }
    teamFinish(j, teamReply(res, j.key, &A, R.since));
}
// thread do TCP: pedido "E0TQ" (os 4 bytes do magic já foram lidos)
static void serveTeamConnection(int fd) {
    e0net::teamTimeouts(fd, 8000);
    uint8_t lb[4];
    if (!e0net::teamRecvAll(fd, lb, 4)) { close(fd); return; }
    uint32_t len; std::memcpy(&len, lb, 4);
    if (len < 4 || len > kTeamMaxReq) { close(fd); return; }
    auto j = std::make_shared<TeamJob>(); j->req.resize(len); j->at = nowSec();
    if (!e0net::teamRecvAll(fd, j->req.data(), len)) { close(fd); return; }
    {
        std::unique_lock<std::mutex> l(g_teamM);
        if (g_teamIn.size() > 256) { l.unlock(); close(fd); return; }
        g_teamIn.push_back(j);
        g_teamCv.wait_for(l, std::chrono::seconds(9), [&] { return j->done || !g_run; });
        if (!j->done) { l.unlock(); close(fd); return; }
    }
    e0net::TBufW out; out.raw("E0TR", 4); out.put<uint32_t>((uint32_t)j->resp.size()); out.raw(j->resp.data(), j->resp.size());
    e0net::teamSendAll(fd, out.b.data(), out.b.size());
    close(fd);
}
// 1x por volta do laço principal: pedidos novos + os que esperavam a conta
static void updateTeams(double now) {
    std::deque<std::shared_ptr<TeamJob>> in;
    { std::lock_guard<std::mutex> l(g_teamM); in.swap(g_teamIn); }
    for (auto& jp : in) {
        TeamJob& j = *jp; ++g_teamReqs;
        e0net::TBufR r(j.req.data(), j.req.size());
        if (!j.R.read(r)) { teamFinish(j, teamReply(TeamErrBadReq, "", nullptr, 0)); continue; }
        if (j.R.authKind == LoginFirebase) {
            if (g_keysCheckAt < 0 || now >= g_keysCheckAt) { g_keysCheckAt = now + 30.0; if (!g_jwkPath.empty()) g_keys.refresh(g_jwkPath); }
            jwt::Claims cl;
            const jwt::Result vr = jwt::verifyFirebase(j.R.token, g_fbProject, g_keys, (double)unixNow(), cl);
            if (vr != jwt::Result::Ok) {
                if (vr == jwt::Result::UnknownKid) g_keysCheckAt = now;
                teamFinish(j, teamReply(vr == jwt::Result::Expired ? TeamErrExpired : TeamErrAuth, "", nullptr, 0)); continue;
            }
            j.key = "fb:" + cl.sub;
        } else if (j.R.authKind == LoginLocal) {
            if (j.R.uid.rfind("local-", 0) != 0 || !uidOk(j.R.uid)) { teamFinish(j, teamReply(TeamErrAuth, "", nullptr, 0)); continue; }
            j.key = "anon:" + j.R.uid;
        } else { teamFinish(j, teamReply(TeamErrAuth, "", nullptr, 0)); continue; }
        j.authed = true;
        if (!g_dataOn && (j.R.op == TeamCreate || j.R.op == TeamJoin)) { teamFinish(j, teamReply(TeamErrNoData, j.key, nullptr, 0)); continue; }
        auto it = g_accounts.find(j.key);
        if (it != g_accounts.end()) { teamHandle(j, it->second); continue; }
        if (!g_loading.count(j.key)) {   // pede a leitura da conta (slot -1: ninguém a "veste"; só fica no cache)
            g_loading[j.key].push_back({-1, 0, 0, 0, ""});
            if (g_dataOn) g_store.requestLoad(j.key); else { persist::Store::Loaded L; L.key = j.key; finishLoad(L); }
        }
        g_teamWait.push_back(jp);
    }
    for (size_t i = 0; i < g_teamWait.size();) {
        TeamJob& j = *g_teamWait[i];
        auto it = g_accounts.find(j.key);
        if (it != g_accounts.end()) teamHandle(j, it->second);
        else if (!g_loading.count(j.key) || now - j.at > 7.0) teamFinish(j, teamReply(g_loading.count(j.key) ? TeamErrBusy : TeamErrAuth, "", nullptr, 0));   // corrompida / demorou demais
        else { ++i; continue; }
        g_teamWait.erase(g_teamWait.begin() + (long)i);
    }
    if (now >= g_teamsPruneAt) {   // mensagens de mais de 24 h somem mesmo sem ninguém abrir a tela
        g_teamsPruneAt = now + 60.0;
        const int64_t u = unixNow();
        for (auto& kv : g_teams.byId) if (teams::Registry::prune(*kv.second, u)) g_teams.dirty = true;
    }
    teamsSave(false);
}
// ---- TRIPULAÇÃO NO JOGO (UDP): bandeira na TAG (MsgTeamQuery/MsgTeamInfo) e CONVITES (MsgTeamInvite -> MsgTeamOffer -> MsgTeamOfferReply) ----
static teams::Team* teamOfSlot(int s) {
    if (s < 0 || s >= kMaxPlayers || !g_clients[s].active || !g_clients[s].acct || !g_clients[s].acct->persistent) return nullptr;
    return g_teams.ofKey(g_clients[s].acct->key);
}
static void writeTeamInfo(Writer& w, int id) {
    teams::Team* t = teamOfSlot(id);
    w.put<uint16_t>((uint16_t)id); w.put<uint32_t>(t ? t->id : 0u);
    uint8_t z[kFlagBytes] = {}; w.bytes(t ? t->flag : z, kFlagBytes);
}
static void sendTeamInfoOf(int id, const sockaddr_in& to) {
    uint8_t b[64]; Writer w(b, sizeof b); w.header(MsgTeamInfo); w.put<uint8_t>(1); writeTeamInfo(w, id); sendTo(to, b, w.size());
}
static void teamInfoChanged(int id) {   // mudou a tripulação de alguém: avisa quem está perto (a TAG troca a bandeira na hora)
    if (id < 0 || id >= kMaxPlayers || !g_clients[id].active) return;
    uint8_t b[64]; Writer w(b, sizeof b); w.header(MsgTeamInfo); w.put<uint8_t>(1); writeTeamInfo(w, id);
    sendNear(b, w.size(), g_clients[id].st.x, g_clients[id].st.z, kInterestRadius, id);
}
static void onTeamQuery(Client& c, Reader& r, double now) {
    const uint8_t n = r.get<uint8_t>();
    if (!r.ok() || n == 0 || n > kTeamQueryMax) return;
    uint16_t ids[kTeamQueryMax]; int k = 0;
    for (int i = 0; i < n; ++i) { const uint16_t id = r.get<uint16_t>(); if (id < kMaxPlayers && g_clients[id].active) ids[k++] = id; }
    if (!r.ok() || now < c.nextTeamQuery) return;
    c.nextTeamQuery = now + 0.25;
    uint8_t b[16 + kTeamQueryMax * (6 + kFlagBytes)]; Writer w(b, sizeof b);
    w.header(MsgTeamInfo); w.put<uint8_t>((uint8_t)k);
    for (int i = 0; i < k; ++i) writeTeamInfo(w, ids[i]);
    sendTo(c.addr, b, w.size());
}
static void sendTeamNotice(Client& to, uint8_t kind, uint8_t result, int other, const std::string& name) {
    uint8_t b[64]; Writer w(b, sizeof b);
    const std::string nm = name.substr(0, 40);
    w.header(MsgTeamNotice); w.put<uint8_t>(kind); w.put<uint8_t>(result); w.put<uint16_t>((uint16_t)(other < 0 ? kNoPlayer : other)); w.put<uint8_t>((uint8_t)nm.size());
    for (char ch : nm) w.put<uint8_t>((uint8_t)ch);
    sendTo(to.addr, b, w.size());
}
static void sendOffer(Client& to) {
    teams::Team* t = nullptr; { auto it = g_teams.byId.find(to.offerTeam); if (it != g_teams.byId.end()) t = it->second.get(); }
    if (!t) return;
    uint8_t b[96]; Writer w(b, sizeof b);
    w.header(MsgTeamOffer); w.put<uint32_t>(to.offerId); w.put<uint16_t>((uint16_t)to.offerFrom); w.put<uint8_t>((uint8_t)std::min<size_t>(t->members.size(), 255));
    w.bytes(t->flag, kFlagBytes); w.put<uint8_t>((uint8_t)std::min<size_t>(t->name.size(), 40));
    for (size_t i = 0; i < t->name.size() && i < 40; ++i) w.put<uint8_t>((uint8_t)t->name[i]);
    sendTo(to.addr, b, w.size());
    to.offerSentAt = nowSec();
}
static uint32_t g_nextOffer = 1;
static void onTeamInvite(Client& c, Reader& r, double now) {
    const uint32_t inviteId = r.get<uint32_t>(); const uint16_t target = r.get<uint16_t>();
    if (!r.ok()) return;
    const int me = (int)(&c - g_clients);
    if (inviteId == c.lastInviteId) { sendTeamNotice(c, TnInviteSent, c.lastInviteRes, target, ""); return; }   // cópia: mesma resposta
    uint8_t res = TeamOk;
    teams::Team* t = teamOfSlot(me);
    Client* tc = target < kMaxPlayers && g_clients[target].active ? &g_clients[target] : nullptr;
    if (!c.acct || !c.acct->persistent) res = TeamErrNoLogin;
    else if (!t) res = TeamErrNoTeam;
    else if (now < c.nextInvite) res = TeamErrSlow;
    else if (!tc || (int)target == me || tc->dead || !tc->acct || !tc->acct->persistent) res = TeamErrTarget;
    else if (g_teams.ofKey(tc->acct->key)) res = TeamErrTargetTeam;
    else if ((int)t->members.size() >= t->capacity()) res = TeamErrFull;
    else if (std::hypot(tc->st.x - c.st.x, tc->st.z - c.st.z) > kTeamInviteDist + 2.0f) res = TeamErrFar;
    c.lastInviteId = inviteId; c.lastInviteRes = res;
    if (res == TeamOk) {
        c.nextInvite = now + kTeamInviteGap;
        tc->offerId = g_nextOffer++; tc->offerTeam = t->id; tc->offerFrom = me; tc->offerFromSession = c.sessionId; tc->offerUntil = now + kTeamOfferTtl;
        sendOffer(*tc);
        std::printf("[tripulacao] jogador %d convidou o jogador %d para '%s'\n", me, (int)target, t->name.c_str()); std::fflush(stdout);
    }
    sendTeamNotice(c, TnInviteSent, res, target, t ? t->name : "");
}
static void onTeamOfferReply(Client& c, Reader& r, double now) {
    const uint32_t offerId = r.get<uint32_t>(); const uint8_t accept = r.get<uint8_t>();
    if (!r.ok()) return;
    const int me = (int)(&c - g_clients);
    if (offerId == c.offerDoneId) return;          // cópia repetida (já tratada)
    if (offerId != c.offerId || now > c.offerUntil) { if (accept) sendTeamNotice(c, TnOfferGone, TeamErrCode, -1, ""); return; }
    c.offerDoneId = offerId; c.offerId = 0;
    teams::Team* t = nullptr; { auto it = g_teams.byId.find(c.offerTeam); if (it != g_teams.byId.end()) t = it->second.get(); }
    Client* from = c.offerFrom >= 0 && g_clients[c.offerFrom].active && g_clients[c.offerFrom].sessionId == c.offerFromSession ? &g_clients[c.offerFrom] : nullptr;
    if (!accept) {
        if (from) sendTeamNotice(*from, TnDeclined, TeamOk, me, t ? t->name : "");
        return;
    }
    uint8_t res = TeamOk;
    if (!t) res = TeamErrCode;
    else if (!c.acct || !c.acct->persistent) res = TeamErrNoLogin;
    else if (g_teams.ofKey(c.acct->key)) res = TeamErrInTeam;
    else if ((int)t->members.size() >= t->capacity()) res = TeamErrFull;
    if (res == TeamOk) {
        auto nm = g_teamNames.find(c.acct->key);
        char def[24]; std::snprintf(def, sizeof def, "PIRATA %03d", me);
        const std::string pname = nm != g_teamNames.end() ? nm->second : std::string(def);
        t->members.push_back({c.acct->key, pname, unixNow(), c.acct->pushToken}); g_teams.byKey[c.acct->key] = t->id;
        teamSystemMsg(*t, pname + " ENTROU NA TRIPULA\xC7\xC3" "O (CONVITE)");
        std::printf("[tripulacao] jogador %d (%s) ACEITOU o convite e entrou em '%s'\n", me, c.acct->key.c_str(), t->name.c_str()); std::fflush(stdout);
        teamInfoChanged(me); sendTeamInfoOf(me, c.addr);
        if (from) sendTeamNotice(*from, TnAccepted, TeamOk, me, t->name);
    }
    sendTeamNotice(c, TnJoined, res, c.offerFrom, t ? t->name : "");
}
static void updateTeamOffers(double now) {   // 1x por tick: reenvia convites pendentes 1x/s
    for (int i = (int)(g_tick % 15); i < kMaxPlayers; i += 15) {
        Client& c = g_clients[i];
        if (!c.active || !c.offerId) continue;
        if (now > c.offerUntil) { c.offerId = 0; continue; }
        if (now - c.offerSentAt >= 1.0) sendOffer(c);
    }
}
// ---- COOP: fogo amigo desligado, posição dos tripulantes, chat da tripulação no jogo e PUSH (Firebase Cloud Messaging) ----
static bool sameCrew(int a, int b) {   // os dois numa mesma tripulação (sem fogo amigo)
    teams::Team* ta = teamOfSlot(a);
    return ta && ta == teamOfSlot(b);
}
// PUSH: o servidor não fala HTTPS; manda as notificações por um AJUDANTE (--fcm-cmd, ex. /opt/isolated/bin/fcm_send.py) que recebe 1 JSON por linha
// no stdin {"token","title","body","action"} e chama a API HTTP v1 do FCM com a conta de serviço. Sem --fcm-cmd: sem push (o resto funciona).
static std::string g_fcmCmd;
struct PushJob { std::string token, title, body; };
static std::mutex g_pushM; static std::condition_variable g_pushCv; static std::deque<PushJob> g_pushQ; static bool g_pushStop = false;
static long g_pushSent = 0;
static std::string latin1Json(const std::string& s) {   // Latin-1 -> UTF-8 com escape de JSON
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += ' ';
        else if (c < 0x80) o += (char)c;
        else { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}
static void pushThread() {
    for (;;) {
        std::deque<PushJob> jobs;
        {
            std::unique_lock<std::mutex> l(g_pushM);
            g_pushCv.wait(l, [] { return g_pushStop || !g_pushQ.empty(); });
            if (g_pushQ.empty()) return;
            jobs.swap(g_pushQ);
        }
        FILE* p = popen(g_fcmCmd.c_str(), "w");
        if (!p) { std::fprintf(stderr, "[push] ERRO: nao consegui rodar '%s'\n", g_fcmCmd.c_str()); continue; }
        for (const PushJob& j : jobs)
            std::fprintf(p, "{\"token\":\"%s\",\"title\":\"%s\",\"body\":\"%s\",\"action\":\"tripulacao\"}\n", latin1Json(j.token).c_str(), latin1Json(j.title).c_str(), latin1Json(j.body).c_str());
        const int rc = pclose(p);
        g_pushSent += (long)jobs.size();
        std::printf("[push] %zu notificacao(oes) entregue(s) ao ajudante (saida %d)\n", jobs.size(), rc); std::fflush(stdout);
    }
}
struct PushPend { std::string title, body, last; int count = 0; double sendAt = 0; };
static std::unordered_map<std::string, PushPend> g_pushPend;   // token -> notificação esperando (junta várias mensagens numa só)
static std::unordered_map<std::string, double> g_pushLast;     // token -> última enviada
constexpr double kPushGap = 30.0;                              // s entre notificações do mesmo aparelho
static void queuePush(const std::string& token, const std::string& title, const std::string& line, double now) {
    if (g_fcmCmd.empty() || token.empty() || token.size() > 300) return;
    PushPend& p = g_pushPend[token];
    p.title = title; p.last = line; ++p.count;
    if (p.count == 1) { auto it = g_pushLast.find(token); p.sendAt = std::max(now + 2.0, it == g_pushLast.end() ? 0.0 : it->second + kPushGap); }
}
static void flushPushes(double now) {
    if (g_pushPend.empty()) return;
    std::vector<PushJob> out;
    for (auto it = g_pushPend.begin(); it != g_pushPend.end();) {
        if (now < it->second.sendAt) { ++it; continue; }
        PushJob j; j.token = it->first; j.title = it->second.title;
        if (it->second.count > 1) j.body = std::to_string(it->second.count) + " novas mensagens. " + it->second.last; else j.body = it->second.last;
        out.push_back(j); g_pushLast[it->first] = now;
        it = g_pushPend.erase(it);
    }
    if (out.empty()) return;
    { std::lock_guard<std::mutex> l(g_pushM); for (auto& j : out) g_pushQ.push_back(std::move(j)); }
    g_pushCv.notify_one();
}
// mensagem nova no chat da tripulação: quem está NO JOGO recebe na hora (UDP); quem não está recebe PUSH (se o aparelho mandou o token)
static void teamChatNotify(teams::Team& t, const std::string& author, const std::string& text, const std::string& authorKey) {
    const double now = nowSec();
    uint8_t b[256]; Writer w(b, sizeof b);
    const std::string a = author.substr(0, 40), tx = text.substr(0, 120);
    w.header(MsgTeamChatPush); w.put<uint8_t>(author.empty() ? 1 : 0);
    w.put<uint8_t>((uint8_t)a.size()); for (char c : a) w.put<uint8_t>((uint8_t)c);
    w.put<uint8_t>((uint8_t)tx.size()); for (char c : tx) w.put<uint8_t>((uint8_t)c);
    for (const teams::Member& m : t.members) {
        if (m.key == authorKey) continue;
        auto ai = g_accounts.find(m.key);
        Client* c = ai != g_accounts.end() ? teamOnlineClient(*ai->second) : nullptr;
        if (c) { sendTo(c->addr, b, w.size()); continue; }
        if (!author.empty()) queuePush(m.pushToken, t.name, author + ": " + text, now);   // mensagens do sistema não viram push
    }
}
// 1x/s: cada tripulante online recebe onde estão os OUTROS tripulantes online (qualquer distância: minimapa e mapa)
static void updateTeamPositions() {
    for (auto& kv : g_teams.byId) {
        const teams::Team& t = *kv.second;
        int on[kTeamMaxMembers + 1]; int n = 0;
        for (const teams::Member& m : t.members) {
            auto ai = g_accounts.find(m.key);
            if (ai == g_accounts.end()) continue;
            if (Client* c = teamOnlineClient(*ai->second)) if (n < kTeamMaxMembers) on[n++] = (int)(c - g_clients);
        }
        if (n < 2) continue;
        for (int i = 0; i < n; ++i) {
            uint8_t b[16 + kTeamMaxMembers * 10]; Writer w(b, sizeof b);
            w.header(MsgTeamPos); w.put<uint8_t>((uint8_t)(n - 1));
            for (int k = 0; k < n; ++k) if (k != i) { const Client& o = g_clients[on[k]]; w.put<uint16_t>((uint16_t)on[k]); w.put<float>(o.st.x); w.put<float>(o.st.z); }
            sendTo(g_clients[on[i]].addr, b, w.size());
        }
    }
}
// ===================== DOMINAÇÃO DE ILHAS (shared/conquest.h) =====================
// O MASTRO de cada ilha do oceano: tripulantes a pé na área sobem a bandeira da tripulação (ou arriam a de outra). Dona salva em teams.e0t.
struct IsleCap {
    bool has = false; float x = 0, y = 0, z = 0;   // mastro (base)
    uint32_t pole = 0; float prog = 0;             // bandeira no mastro e altura 0..1
    uint8_t state = CapIdle; int nAtt = 0, nDef = 0; uint32_t att = 0;
    double shieldUntil = 0, warnAt = -1e9, alarmAt = -1e9;
};
static IsleCap g_cap[kIslandSlots];
static float g_capScale = 1.0f;          // --cap-time-scale X (testes): multiplica os tempos de hastear/arriar/voltar/proteção
static int g_payoutSec = kIslandPayoutSec;   // --payout-sec S (testes): intervalo da renda
static bool g_ownersDirty = false;
static double g_payoutCheckAt = 0;
static std::string isleName(int slot) { const World* w = worldOfSlot(slot); return islandLabel(slot, w ? w->kind : (uint8_t)IslandBig); }
static const teams::Team* teamById(uint32_t id) { auto it = g_teams.byId.find(id); return it == g_teams.byId.end() ? nullptr : it->second.get(); }
static void capInit() {
    int n = 0;
    for (int s = 1; s < kIslandSlots; ++s) {
        const World* w = worldOfSlot(s);
        IsleCap& C = g_cap[s]; C = IsleCap();
        int32_t xc, zc;
        if (!w || !conquestPoint(*w, xc, zc)) continue;
        C.has = true; C.x = xc * 0.01f; C.z = zc * 0.01f; C.y = w->heightAt(C.x, C.z);
        C.pole = g_teams.isleOwner[s]; C.prog = C.pole ? 1.0f : 0.0f;
        ++n;
    }
    int owned = 0; for (int s = 0; s < kIslandSlots; ++s) if (g_teams.isleOwner[s]) ++owned;
    std::printf("Dominacao de ilhas: %d mastros, %d ilha(s) dominada(s)\n", n, owned);
}
static size_t writeOwners(uint8_t* b, size_t cap) {
    Writer w(b, cap); w.header(MsgIslandOwners);
    int n = 0; for (int s = 1; s < kIslandSlots; ++s) if (g_teams.isleOwner[s] && teamById(g_teams.isleOwner[s])) ++n;
    w.put<uint8_t>((uint8_t)n);
    for (int s = 1; s < kIslandSlots; ++s) {
        const teams::Team* t = g_teams.isleOwner[s] ? teamById(g_teams.isleOwner[s]) : nullptr;
        if (!t) continue;
        w.put<uint8_t>((uint8_t)s); w.put<uint32_t>(t->id); w.bytes(t->flag, kFlagBytes);
    }
    return w.ok() ? w.size() : 0;
}
static void sendIslandOwners(const sockaddr_in& to) { uint8_t b[1600]; const size_t n = writeOwners(b, sizeof b); if (n) sendTo(to, b, n); }
static void sendIslandEvent(const sockaddr_in& to, uint8_t kind, int slot, int32_t value, const std::string& name) {
    uint8_t b[96]; Writer w(b, sizeof b); const std::string nm = name.substr(0, 40);
    w.header(MsgIslandEvent); w.put<uint8_t>(kind); w.put<uint8_t>((uint8_t)slot); w.put<int32_t>(value);
    w.put<uint8_t>((uint8_t)nm.size()); for (char c : nm) w.put<uint8_t>((uint8_t)c);
    sendTo(to, b, w.size());
}
// tripulantes da tripulação: online no jogo recebem o evento; os de fora (com token) recebem push
static void teamIslandNotify(const teams::Team& t, uint8_t kind, int slot, int32_t value, const std::string& name, const std::string& push, double now) {
    for (const teams::Member& m : t.members) {
        auto ai = g_accounts.find(m.key);
        Client* c = ai != g_accounts.end() ? teamOnlineClient(*ai->second) : nullptr;
        if (c) sendIslandEvent(c->addr, kind, slot, value, name);
        else if (!push.empty()) queuePush(m.pushToken, t.name, push, now);
    }
}
static void capCapture(int slot, uint32_t A, double now) {
    IsleCap& C = g_cap[slot];
    const uint32_t old = g_teams.isleOwner[slot];
    teams::Team* tA = g_teams.byId.count(A) ? g_teams.byId[A].get() : nullptr;
    if (!tA) return;
    g_teams.isleOwner[slot] = A; g_teams.isleSince[slot] = unixNow(); g_teams.dirty = true;
    C.pole = A; C.prog = 1.0f; C.shieldUntil = now + kCapShieldSec * g_capScale; C.state = CapShielded;
    const World* w = worldOfSlot(slot);
    const std::string nm = isleName(slot);
    const int inc = islandIncome(w ? w->kind : (uint8_t)IslandBig);
    teamSystemMsg(*tA, "DOMINAMOS " + nm + "! RENDE " + std::to_string(inc) + " MOEDAS A CADA 20 MIN");
    if (old && g_teams.byId.count(old)) {
        teams::Team& tO = *g_teams.byId[old];
        teamSystemMsg(tO, "PERDEMOS " + nm + " PARA " + tA->name);
        teamIslandNotify(tO, IeLost, slot, 0, tA->name, "Perdemos " + nm + " para " + tA->name, now);
    }
    for (int i = 0; i < kMaxPlayers; ++i) if (g_clients[i].active) sendIslandEvent(g_clients[i].addr, IeCaptured, slot, (int32_t)A, tA->name);
    g_ownersDirty = true;
    std::printf("[ilha] %s DOMINADA por '%s' (%u)%s\n", nm.c_str(), tA->name.c_str(), A, old ? " (tomada de outra tripulacao)" : ""); std::fflush(stdout);
}
static void capPayout(double now) {
    if (now < g_payoutCheckAt) return;
    g_payoutCheckAt = now + std::min(10.0, g_payoutSec * 0.5);
    const int64_t u = unixNow();
    if (!g_teams.lastPayout) { g_teams.lastPayout = u; g_teams.dirty = true; return; }
    if (u - g_teams.lastPayout < g_payoutSec) return;
    const int64_t periods = std::min<int64_t>(3, (u - g_teams.lastPayout) / g_payoutSec);   // servidor desligado muito tempo: no máx. 3 pagamentos (1 h)
    g_teams.lastPayout = u - (u - g_teams.lastPayout) % g_payoutSec; g_teams.dirty = true;
    std::unordered_map<uint32_t, std::pair<int, int>> sum;   // tripulação -> (moedas, ilhas)
    for (int s = 1; s < kIslandSlots; ++s) if (g_teams.isleOwner[s]) { const World* w = worldOfSlot(s); auto& p = sum[g_teams.isleOwner[s]]; p.first += (islandIncome(w ? w->kind : (uint8_t)IslandBig) + structIncome(s)) * (int)periods; ++p.second; }   // + marcenaria/armazém
    for (auto& kv : sum) {
        auto it = g_teams.byId.find(kv.first); if (it == g_teams.byId.end()) continue;
        teams::Team& t = *it->second;
        t.treasury += kv.second.first;
        teamSystemMsg(t, (kv.second.second == 1 ? std::string("A ILHA RENDEU ") : std::to_string(kv.second.second) + " ILHAS RENDERAM ") + std::to_string(kv.second.first) + " MOEDAS PARA O COFRE");
        teamIslandNotify(t, IeIncome, 0, kv.second.first, t.name, "", now);
        std::printf("[ilha] renda: '%s' +%d moedas (%d ilhas; cofre %lld)\n", t.name.c_str(), kv.second.first, kv.second.second, (long long)t.treasury);
    }
    std::fflush(stdout);
}
static void updateConquest(double now) {
    if (!g_hasWorld) return;
    static double last = now;
    const float dt = (float)std::min(0.5, std::max(0.0, now - last)); last = now;
    struct Cnt { uint32_t team; int n; };
    static std::vector<Cnt> cnt[kIslandSlots];
    for (auto& v : cnt) v.clear();
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Client& c = g_clients[i];
        if (!c.active || c.dead || c.carriedBy >= 0 || c.st.ship) continue;
        const int sl = terrain().slotAt(c.st.x, c.st.z);
        if (!islandConquerable(sl) || !g_cap[sl].has) continue;
        const IsleCap& C = g_cap[sl];
        const float dx = c.st.x - C.x, dz = c.st.z - C.z;
        if (dx * dx + dz * dz > kCapRadius * kCapRadius || std::fabs(c.st.y - C.y) > kCapMaxDy) continue;
        const teams::Team* t = teamOfSlot(i);
        if (!t) continue;
        bool found = false;
        for (Cnt& k : cnt[sl]) if (k.team == t->id) { ++k.n; found = true; break; }
        if (!found) cnt[sl].push_back({t->id, 1});
    }
    for (int s = 1; s < kIslandSlots; ++s) {
        IsleCap& C = g_cap[s];
        if (!C.has) continue;
        uint32_t owner = g_teams.isleOwner[s];
        if (owner && !teamById(owner)) { owner = g_teams.isleOwner[s] = 0; g_teams.dirty = true; g_ownersDirty = true; }
        if (C.pole && !teamById(C.pole)) { C.pole = 0; C.prog = 0; }
        int nDef = 0, nAtt = 0, attTeams = 0; uint32_t A = 0;
        for (const Cnt& k : cnt[s]) { if (k.team == owner) nDef = k.n; else { nAtt += k.n; ++attTeams; A = k.team; } }
        C.nAtt = nAtt; C.nDef = nDef; C.att = attTeams == 1 ? A : 0;
        if (owner && now < C.shieldUntil) { C.pole = owner; C.prog = 1.0f; C.state = CapShielded; continue; }
        if (attTeams >= 2 || (attTeams == 1 && nDef > 0)) C.state = CapContested;
        else {
            const uint32_t target = attTeams == 1 ? A : owner;
            const int helpers = attTeams == 1 ? nAtt : nDef;
            const float mul = capSpeedMul(helpers);
            const float tw = attTeams == 1 && owner ? 1.0f - kTowerSlowdown * (float)structTowers(s) : 1.0f;   // TORRES DE VIGIA da ilha atrasam quem ataca
            const float raise = (helpers ? mul / kCapRaiseSec : 1.0f / kCapDecaySec) / g_capScale * tw, lower = (helpers ? mul / kCapLowerSec : 1.0f / kCapDecaySec) / g_capScale * tw;
            if (C.pole == target) {
                if (!target) { C.prog = 0; C.state = CapIdle; }
                else if (C.prog < 1.0f) { C.prog = std::min(1.0f, C.prog + dt * raise); C.state = attTeams ? CapRaising : CapRestoring; }
                else C.state = CapIdle;
            } else if (!C.pole) {   // mastro sem bandeira (ilha livre): a bandeira nova já nasce, sem "arriar" nada
                C.pole = target; C.prog = 0.0f; C.state = attTeams ? CapRaising : CapIdle;
            } else {
                C.prog -= dt * lower; C.state = attTeams ? CapLowering : CapRestoring;
                if (C.prog <= 0.0f) { C.pole = target; C.prog = 0.0f; }
            }
            if (attTeams == 1 && C.pole == A && C.prog >= 1.0f) { capCapture(s, A, now); continue; }
        }
        if (owner && attTeams && (C.state == CapLowering || C.state == CapContested) && now - C.warnAt > 60.0) {   // a dona fica sabendo do ataque
            C.warnAt = now;
            const teams::Team* tA = teamById(A ? A : cnt[s][0].team == owner && cnt[s].size() > 1 ? cnt[s][1].team : cnt[s][0].team);
            const std::string an = tA ? tA->name : std::string("PIRATAS");
            const bool alarm = now - C.alarmAt > 300.0;
            if (alarm) C.alarmAt = now;
            auto it = g_teams.byId.find(owner);
            if (it != g_teams.byId.end()) {
                if (alarm) teamSystemMsg(*it->second, isleName(s) + " EST\xC1 SOB ATAQUE DE " + an + "!");
                teamIslandNotify(*it->second, IeUnderAttack, s, 0, an, alarm ? isleName(s) + " est\xE1 sob ataque de " + an + "!" : "", now);
            }
        }
    }
    capPayout(now);
}
// 5x/s: estado do mastro a quem está perto dele; a cada mudança de dona e a cada 15 s: a lista das dominadas a todos
static void sendConquest(double now) {
    (void)now;
    uint8_t pk[kIslandSlots][160]; size_t pn[kIslandSlots] = {};
    auto build = [&](int s) {
        const IsleCap& C = g_cap[s];
        const uint32_t owner = g_teams.isleOwner[s];
        const teams::Team* tO = owner ? teamById(owner) : nullptr; const teams::Team* tP = C.pole ? teamById(C.pole) : nullptr;
        const uint8_t z[kFlagBytes] = {};
        Writer w(pk[s], sizeof pk[s]); w.header(MsgIslandCap);
        w.put<uint8_t>((uint8_t)s); w.put<uint32_t>(tO ? owner : 0u); w.put<uint32_t>(tP ? C.pole : 0u);
        w.put<uint8_t>((uint8_t)std::lround(std::max(0.0f, std::min(1.0f, C.prog)) * 255.0f)); w.put<uint8_t>(C.state);
        w.put<uint8_t>((uint8_t)std::min(255, C.nAtt)); w.put<uint8_t>((uint8_t)std::min(255, C.nDef));
        w.put<uint16_t>((uint16_t)std::max(0.0, std::min(65535.0, C.shieldUntil - nowSec())));
        w.bytes(tO ? tO->flag : z, kFlagBytes); w.bytes(tP ? tP->flag : z, kFlagBytes);
        const std::string a = tO ? tO->name.substr(0, 40) : std::string(), b = tP ? tP->name.substr(0, 40) : std::string();
        w.put<uint8_t>((uint8_t)a.size()); for (char c : a) w.put<uint8_t>((uint8_t)c);
        w.put<uint8_t>((uint8_t)b.size()); for (char c : b) w.put<uint8_t>((uint8_t)c);
        pn[s] = w.ok() ? w.size() : 1;
    };
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Client& c = g_clients[i];
        if (!c.active) continue;
        const int sl = slotOfSector((int)std::floor(c.st.x / kSectorM + 0.5f), (int)std::floor(c.st.z / kSectorM + 0.5f));
        if (!islandConquerable(sl) || !g_cap[sl].has) continue;
        const float dx = c.st.x - g_cap[sl].x, dz = c.st.z - g_cap[sl].z;
        if (dx * dx + dz * dz > kCapNearRadius * kCapNearRadius) continue;
        if (!pn[sl]) build(sl);
        if (pn[sl] > 1) sendTo(c.addr, pk[sl], pn[sl]);
    }
}
static void updateIslandOwnersSync() {
    uint8_t b[1600]; size_t n = 0;
    if (g_ownersDirty) {
        g_ownersDirty = false; n = writeOwners(b, sizeof b);
        if (n) for (const auto& c : g_clients) if (c.active) sendTo(c.addr, b, n);
        return;
    }
    const uint32_t k = g_tick % (kTickHz * 15);
    for (int i = (int)k; i < kMaxPlayers; i += kTickHz * 15) if (g_clients[i].active) { if (!n) n = writeOwners(b, sizeof b); if (n) sendTo(g_clients[i].addr, b, n); }
}
// ===================== MELHORIAS DA ILHA (shared/isle_build.h) =====================
// A tripulação DONA ergue/demole estruturas na ilha dominada; paga com a mochila de quem constrói. Estado salvo no world_state.e0x (v2).
static uint16_t g_nextStructId = 1;
static bool g_structsDirty = false;
static void writeStructSync(Writer& w, int slot) {
    const auto& v = isleStructs(slot);
    w.header(MsgStructSync); w.put<uint8_t>((uint8_t)slot); w.put<uint8_t>((uint8_t)std::min<size_t>(v.size(), kIsleMaxStructs));
    for (size_t i = 0; i < v.size() && i < (size_t)kIsleMaxStructs; ++i) {
        const IsleStruct& s = v[i];
        w.put<uint16_t>(s.id); w.put<uint8_t>(s.kind); w.put<uint8_t>(s.rot); w.put<float>(s.x); w.put<float>(s.y); w.put<float>(s.z);
    }
}
static bool g_structTouched[kIslandSlots] = {};   // ilha que já teve estrutura (a sincronia periódica manda também a lista vazia)
static void sendStructSync(const sockaddr_in& to, int slot) { uint8_t b[16 + kIsleMaxStructs * 16]; Writer w(b, sizeof b); writeStructSync(w, slot); if (w.ok()) sendTo(to, b, w.size()); }
static void broadcastStructSync(int slot) {
    uint8_t b[16 + kIsleMaxStructs * 16]; Writer w(b, sizeof b); writeStructSync(w, slot);
    if (!w.ok()) return;
    for (const auto& c : g_clients) if (c.active) sendTo(c.addr, b, w.size());
}
static void sendAllStructs(const sockaddr_in& to) { for (int s = 1; s < kIslandSlots; ++s) if (g_structTouched[s] || !isleStructs(s).empty()) sendStructSync(to, s); }
static void updateStructSync() {   // a cada 20 s cada jogador recebe todas as ilhas com estrutura (espalhado pelos ticks)
    const uint32_t k = g_tick % (kTickHz * 20);
    for (int i = (int)k; i < kMaxPlayers; i += kTickHz * 20) if (g_clients[i].active) sendAllStructs(g_clients[i].addr);
}
static void onStructReq(Client& c, Reader& r, double now) {
    const uint16_t reqId = r.get<uint16_t>(); const uint8_t op = r.get<uint8_t>(), slot = r.get<uint8_t>(), kind = r.get<uint8_t>(), rot = r.get<uint8_t>();
    const float x = r.get<float>(), z = r.get<float>(); const uint16_t id = r.get<uint16_t>();
    if (!r.ok()) return;
    auto reply = [&](uint8_t res) {
        uint8_t b[16]; Writer w(b, sizeof b); w.header(MsgStructRes); w.put<uint16_t>(reqId); w.put<uint8_t>(op); w.put<uint8_t>(res); w.put<uint8_t>(kind);
        sendTo(c.addr, b, w.size());
        c.lastStructReq = reqId; c.lastStructRes = res; c.lastStructValid = true;
    };
    if (c.lastStructValid && reqId == c.lastStructReq) { reply(c.lastStructRes); return; }   // cópia repetida: mesma resposta
    const int me = (int)(&c - g_clients);
    teams::Team* t = teamOfSlot(me);
    if (now < c.nextStruct) return reply(SrSlow);
    if (!g_hasWorld || !islandConquerable(slot) || !g_cap[slot].has || !worldOfSlot(slot)) return reply(SrNotHere);
    if (!t) return reply(SrNoCrew);
    if (g_teams.isleOwner[slot] != t->id) return reply(SrNotOwner);
    if (terrain().slotAt(c.st.x, c.st.z) != slot) return reply(SrNotHere);   // só na ilha em que está
    auto& list = isleStructs(slot);
    if (op == 2) {   // DEMOLIR: devolve metade dos materiais
        auto it = std::find_if(list.begin(), list.end(), [&](const IsleStruct& s) { return s.id == id; });
        if (it == list.end()) return reply(SrNotFound);
        const StructDef& D = structDef(it->kind);
        if (c.acct) { for (int k = 0; k < 3; ++k) if (D.cost[k] && D.costN[k] / 2) invAdd(c.acct->inv, D.cost[k], D.costN[k] / 2); invChanged(c); markDirty(*c.acct, false); }
        std::printf("[ilha] jogador %d (%s) DEMOLIU %s em %s\n", me, t->name.c_str(), D.name, isleName(slot).c_str());
        list.erase(it); ++isleStructGen(slot); g_structsDirty = true; g_structTouched[slot] = true;
        c.nextStruct = now + 0.3;
        reply(SrOk); broadcastStructSync(slot); std::fflush(stdout); return;
    }
    if (op != 1 || kind >= SkKindCount) return reply(SrNotHere);
    float y = 0;
    IsleStruct ns; ns.kind = kind; ns.rot = rot & 7; ns.x = x; ns.z = z;
    if (kind == SkWall) {   // MURALHA: o anel é calculado da ilha (a posição do pedido não importa); fica registrada no portão
        const uint8_t res = wallCheckPlace(slot);
        if (res != SrOk) return reply(res);
        const IsleWall* W = wallGeomOf(slot);
        ns.rot = 0; ns.x = W->gx; ns.z = W->gz; y = W->gy;
    } else {
        const uint8_t res = structCheckPlace(*worldOfSlot(slot), slot, kind, rot & 7, x, z, g_cap[slot].x, g_cap[slot].z, y);
        if (res != SrOk) return reply(res);
    }
    ns.y = y;
    if (!c.acct || !structHasCost(c.acct->inv, kind)) return reply(SrCost);
    for (int i = 0; i < kMaxPlayers && kind != SkWall; ++i) {   // ninguém fica preso dentro da estrutura (a muralha empurra para fora sozinha)
        const Client& o = g_clients[i];
        if (!o.active || o.st.ship) continue;
        float px = o.st.x, pz = o.st.z;
        const float yaw = structYaw(ns.rot); float lx, lz; structToLocal(yaw, x, z, px, pz, lx, lz);
        const StructDef& D = structDef(kind);
        if (std::fabs(lx) < D.hx + kPlayerRadius && std::fabs(lz) < D.hz + kPlayerRadius && std::fabs(o.st.y - y) < 4.0f) {
            bool inside = false;
            for (int b = 0; b < D.nBox; ++b) if (lx > D.box[b].x0 - kPlayerRadius && lx < D.box[b].x1 + kPlayerRadius && lz > D.box[b].z0 - kPlayerRadius && lz < D.box[b].z1 + kPlayerRadius) inside = true;
            if (inside) return reply(SrPeople);
        }
    }
    const StructDef& D = structDef(kind);
    for (int k = 0; k < 3; ++k) if (D.cost[k]) invRemove(c.acct->inv, D.cost[k], D.costN[k]);
    invChanged(c); markDirty(*c.acct, true);
    ns.id = g_nextStructId++; if (!g_nextStructId) g_nextStructId = 1;
    list.push_back(ns); ++isleStructGen(slot); g_structsDirty = true; g_structTouched[slot] = true;
    c.nextStruct = now + 0.3;
    std::printf("[ilha] jogador %d (%s) ERGUEU %s em %s (%.1f, %.1f) rot %d | %zu estruturas\n", me, t->name.c_str(), D.name, isleName(slot).c_str(), ns.x, ns.z, ns.rot * 45, list.size()); std::fflush(stdout);
    reply(SrOk); broadcastStructSync(slot);
}
// CAIS: navio chamado por um tripulante na ilha da tripulação (com muralha) atraca na frente do cais
static bool islePierSpot(const Client& c, int kind, float& x, float& z, float& yaw) {
    const int slot = terrain().slotAt(c.st.x, c.st.z);
    if (!islandConquerable(slot)) return false;
    const teams::Team* t = teamOfSlot((int)(&c - g_clients));
    if (!t || g_teams.isleOwner[slot] != t->id) return false;
    const IsleWall* W = isleWallOf(slot);
    if (!W || std::hypot(c.st.x - W->gx, c.st.z - W->gz) > 300.0f) return false;
    return shipFindAhead(kind, terrain(), W->gx + W->nx * W->p1, W->gz + W->nz * W->p1, std::atan2(W->nx, -W->nz), x, z, yaw);
}
static void updateAccounts(double now) {
    for (int i = (int)(g_tick % 30); i < kMaxPlayers; i += 30) {
        Client& c = g_clients[i];
        if (c.active && c.acct && c.invAckRev != c.invRev && now - c.invSentAt > 0.9) sendInvSync(c);
    }
    if (g_tick % 15) return;
    for (auto it = g_accounts.begin(); it != g_accounts.end();) {
        persist::Account& a = *it->second;
        if (a.dirty && (a.critical || now - a.dirtySince > 10.0)) saveAccount(a, a.critical);
        if (a.session < 0 && !a.dirty && now - a.lastUsed > 600.0 && !g_loading.count(it->first) && !g_migrating.count(it->first)) it = g_accounts.erase(it);
        else ++it;
    }
}
// ---- ESTADO DO MUNDO (itens soltos, construções, baús) ----
static std::vector<uint8_t> worldStateBytes() {
    persist::Out o; o.put<uint32_t>(0x31583045u); o.put<uint16_t>(2); o.put<uint32_t>(g_worldHash);
    o.put<uint16_t>((uint16_t)g_drops.size());
    for (const DropItem& d : g_drops) { o.put<uint8_t>(d.item); o.put<uint16_t>(d.n); o.put<float>(d.x); o.put<float>(d.y); o.put<float>(d.z); }
    int ni = 0; for (int s = 1; s < kIslandSlots; ++s) if (!isleStructs(s).empty()) ++ni;   // v2: MELHORIAS DA ILHA (estruturas por ilha)
    o.put<uint8_t>((uint8_t)ni);
    for (int s = 1; s < kIslandSlots; ++s) {
        const auto& v = isleStructs(s); if (v.empty()) continue;
        o.put<uint8_t>((uint8_t)s); o.put<uint8_t>((uint8_t)v.size());
        for (const IsleStruct& t : v) { o.put<uint8_t>(t.kind); o.put<uint8_t>(t.rot); o.put<float>(t.x); o.put<float>(t.y); o.put<float>(t.z); }
    }
    o.put<uint8_t>((uint8_t)kMaxShips);
    for (int s = 0; s < kMaxShips; ++s) for (const auto& sl : g_chests[s].s) { o.put<uint8_t>(sl.id); o.put<uint16_t>(sl.n); }
    o.crc(); return o.b;
}
static void loadWorldState() {
    std::vector<uint8_t> d;
    const std::string p = g_store.dir() + "/world_state.e0x";
    if (!persist::readFile(p, d)) return;
    if (!persist::crcOk(d)) { std::printf("AVISO: %s corrompido (ignorado; copia em .corrompido)\n", p.c_str()); persist::writeAtomic(p + ".corrompido", d); return; }
    persist::In r(d.data(), d.size() - 4);
    if (r.get<uint32_t>() != 0x31583045u) return;
    const uint16_t ver = r.get<uint16_t>();
    if (ver < 1 || ver > 2) return;
    if (r.get<uint32_t>() != g_worldHash) { std::printf("Estado do mundo salvo e de OUTRO mundo: ignorado\n"); return; }
    const uint16_t nd = r.get<uint16_t>();
    for (int i = 0; i < nd && r.ok; ++i) { DropItem it; it.id = g_nextDropId++; it.item = r.get<uint8_t>(); it.n = r.get<uint16_t>(); it.x = r.get<float>(); it.y = r.get<float>(); it.z = r.get<float>(); if (itemValid(it.item) && it.n && (int)g_drops.size() < kMaxDrops) g_drops.push_back(it); }
    int nStructs = 0;
    if (ver == 1) {   // v1: construções do sistema antigo (removido): lidas e descartadas
        const uint16_t nb = r.get<uint16_t>();
        for (int i = 0; i < nb && r.ok; ++i) { r.get<uint8_t>(); r.get<uint8_t>(); r.get<float>(); r.get<float>(); r.get<float>(); }
    } else {
        const int ni = r.get<uint8_t>();
        for (int k = 0; k < ni && r.ok; ++k) {
            const int sl = r.get<uint8_t>(), n = r.get<uint8_t>();
            for (int i = 0; i < n && r.ok; ++i) {
                IsleStruct t; t.kind = r.get<uint8_t>(); t.rot = r.get<uint8_t>() & 7; t.x = r.get<float>(); t.y = r.get<float>(); t.z = r.get<float>();
                if (!r.ok || !islandConquerable(sl) || t.kind >= SkKindCount || (int)isleStructs(sl).size() >= kIsleMaxStructs) continue;
                t.id = g_nextStructId++; isleStructs(sl).push_back(t); ++isleStructGen(sl); g_structTouched[sl] = true; ++nStructs;
            }
        }
    }
    const int ns = r.get<uint8_t>();
    for (int s = 0; s < ns && r.ok; ++s) for (int k = 0; k < kChestSlots; ++k) {
        const uint8_t id = r.get<uint8_t>(); const uint16_t n = r.get<uint16_t>();
        if (s < kMaxShips && itemValid(id) && n && n <= itemMaxStack(id)) { g_chests[s].s[k].id = id; g_chests[s].s[k].n = n; }
    }
    std::printf("Estado do mundo carregado: %zu itens soltos, %d estruturas nas ilhas, bau(s) com itens: %d\n", g_drops.size(), nStructs,
                [] { int k = 0; for (int s = 0; s < kMaxShips; ++s) if (chestCount(g_chests[s])) ++k; return k; }());
}
static std::vector<uint8_t> g_lastWorldState;
static void saveWorldState(bool force) {
    if (!g_dataOn || !g_hasWorld) return;
    std::vector<uint8_t> b = worldStateBytes();
    if (!force && b == g_lastWorldState) return;
    g_lastWorldState = b;
    g_store.saveFile("world_state.e0x", std::move(b));
}

static void receiveAll(double now) {
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(g_fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 0) return;  // EAGAIN
        Reader r(buf, (size_t)n);
        uint8_t type = r.header();
        if (!type) continue;
        if (type == MsgJoin) {
            uint8_t sk = r.get<uint8_t>(); uint32_t ck = r.get<uint32_t>();
            if (r.ok()) onJoin(from, now, sk < kSkinCount ? sk : 0, ck, (size_t)n);
            continue;
        }
        Client* c = findClient(from);
        if (!c) continue;
        c->lastRecv = now;
        if (type == MsgInput) onInput(*c, r, now);
        else if (type == MsgDebugTime) {
            float v = r.get<float>();
            if (r.ok() && finite(v) && v >= 0 && v < kCycleSec && debugAllowed(*c, now, "HORA", "%.0f s", v)) {
                g_cycleStart = v - (now - g_t0);  // cycleNow(now) passa a valer v
                std::printf("[debug] hora do mundo -> %.0f s (jogador %d)\n", v, (int)(c - g_clients));
                std::fflush(stdout);
            }
        } else if (type == MsgRespawn) {   // renascer: volta ao ponto de nascimento (mesmo círculo de 2.5 m do Join)
            int i = (int)(c - g_clients);
            if (g_hasWorld) {
                spawnPoint(i, c->st.x, c->st.z);
                terrain().pushOut(c->st.x, c->st.z, kPlayerRadius);
                c->st.y = std::max(terrain().heightAt(c->st.x, c->st.z), World::kSwimFloorH);
            } else { c->st.x = (float)((i % 4) * 3 - 4); c->st.z = (float)(-(i / 4) * 3); c->st.y = 0; }
            c->st.vy = 0; c->st.speed = 0;
            shipReleaseHelm(i); c->st.ship = 0;
            clearLife(i); c->protectUntil = now + g_spawnProtect;
            std::printf("[morte] jogador %d renasceu em (%.0f, %.0f)\n", i, c->st.x, c->st.z); std::fflush(stdout);
        } else if (type == MsgDebugTeleport) {
            float tx = r.get<float>(), tz = r.get<float>();
            if (r.ok() && finite(tx) && finite(tz) && debugAllowed(*c, now, "TELEPORTE", "(%.0f, %.0f)", tx, tz)) {
                const float lim = g_hasWorld ? Terrain::limit() : kWorldHalf;
                c->st.x = std::max(-lim, std::min(lim, tx)); c->st.z = std::max(-lim, std::min(lim, tz));
                if (g_hasWorld) { terrain().pushOut(c->st.x, c->st.z, kPlayerRadius); c->st.y = std::max(terrain().heightAt(c->st.x, c->st.z), World::kSwimFloorH); c->st.y = std::max(c->st.y, isleFloorAt(c->st.x, c->st.z)); } else c->st.y = 0;
                c->st.vy = 0; c->st.speed = 0;
                shipReleaseHelm((int)(c - g_clients)); c->st.ship = 0;
                std::printf("[debug] jogador %d teleportado para (%.0f, %.0f)\n", (int)(c - g_clients), c->st.x, c->st.z); std::fflush(stdout);
            }
        } else if (type == MsgDebugShip) {   // debug: faz nascer um navio na água perto de (x, z) [quem estiver a bordo vai junto]
            float tx = r.get<float>(), tz = r.get<float>();
            uint8_t kind = r.get<uint8_t>();
            if (r.ok() && finite(tx) && finite(tz) && kind < kMaxShips && debugAllowed(*c, now, "SPAWNAR_NAVIO", "%s em (%.0f, %.0f)", shipDef(kind).name, tx, tz)) spawnShipNear(kind, tx, tz, 14.0f);
        } else if (type == MsgDebugShipHp) {   // debug: dano no navio / afundar já
            uint8_t ship = r.get<uint8_t>(); int16_t delta = r.get<int16_t>();
            if (r.ok() && ship < kMaxShips && shipAfloat(shipState(ship)) && debugAllowed(*c, now, "VIDA_NAVIO", "%s %+d", shipDef(ship).name, (int)delta)) {
                std::printf("[debug] jogador %d: vida do %s %+d\n", (int)(c - g_clients), shipDef(ship).name, (int)delta); std::fflush(stdout);
                if (delta <= -1000) startSink(ship); else damageShip(ship, -delta);
            }
        } else if (type == MsgCannon) {
            onCannon(*c, r, now);
        } else if (type == MsgShoot) {
            onShoot(*c, r, now);
        } else if (type == MsgAccount) {
            onAccount(*c, r);
        } else if (type == MsgLogin) {
            onLogin(*c, r, now);
        } else if (type == MsgInvAck) {
            const uint32_t rv = r.get<uint32_t>();
            if (r.ok() && rv == c->invRev) c->invAckRev = rv;
        } else if (type == MsgVitals) {   // vida/fome são do cliente: o servidor só guarda para a próxima sessão
            const uint8_t hp = r.get<uint8_t>(), hu = r.get<uint8_t>();
            if (r.ok() && c->acct && c->acct->persistent && !c->dead && (hp != c->acct->health || hu != c->acct->hunger)) { c->acct->health = std::min<uint8_t>(hp, 100); c->acct->hunger = std::min<uint8_t>(hu, 100); markDirty(*c->acct, false); }
        } else if (type == MsgProfile) {
            uint8_t fl[kFlagBytes];
            if (r.bytes(fl, sizeof fl) && c->acct && c->acct->persistent && (!c->acct->hasFlag || std::memcmp(fl, c->acct->flag, sizeof fl) != 0)) {
                std::memcpy(c->acct->flag, fl, sizeof fl); c->acct->hasFlag = true; markDirty(*c->acct, false);
            }
        } else if (type == MsgDebugCoins) {   // cheat de moedas: SÓ com --debug ou ADMIN (debugAllowed)
            const int32_t d = r.get<int32_t>();
            if (r.ok() && c->acct && d > -1000000 && d < 1000000 && debugAllowed(*c, now, "MOEDAS", "%+d (saldo antes %d)", d, c->acct->coins)) { c->acct->coins = std::max(0, c->acct->coins + d); invChanged(*c, true); std::printf("[debug] jogador %d: moedas %+d\n", (int)(c - g_clients), d); std::fflush(stdout); }
        } else if (type == MsgDebugItems) {   // cheat: 1 de cada item (ferramentas só se não tiver): SÓ com --debug ou ADMIN
            const uint8_t it = r.get<uint8_t>(); const uint16_t nn = r.get<uint16_t>();
            if (r.ok() && debugAllowed(*c, now, "ITENS", itemValid(it) ? "item %d x%d" : "1 de cada", (int)it, (int)nn)) {
                InvSlot* inv = invOf(*c);
                if (itemValid(it)) invAdd(inv, it, invRoomFor(inv, it, nn));
                else for (int id = 1; id < kItemCount; ++id) { if (itemMaxStack(id) == 1 && invCount(inv, id)) continue; if (invFits(inv, id, 1)) invAdd(inv, id, 1); }
                invChanged(*c);
                std::printf("[debug] jogador %d: itens (%d x%d)\n", (int)(c - g_clients), (int)it, (int)nn); std::fflush(stdout);
            }
        } else if (type == MsgChat) {
            onChat(*c, r, now);
        } else if (type == MsgTeamQuery) {
            onTeamQuery(*c, r, now);
        } else if (type == MsgTeamInvite) {
            onTeamInvite(*c, r, now);
        } else if (type == MsgTeamOfferReply) {
            onTeamOfferReply(*c, r, now);
        } else if (type == MsgSummonShip) {
            onSummon(*c, r, now);
        } else if (type == MsgStructReq) {
            onStructReq(*c, r, now);
        } else if (type == MsgCollect) {
            uint32_t idx = r.get<uint32_t>();
            if (r.ok()) onCollect(*c, idx);
        } else if (type == MsgAct) {
            onAct(*c, r);
        } else if (type == MsgDamageAck) {
            uint32_t did = r.get<uint32_t>();
            if (r.ok()) for (size_t i = 0; i < g_dmgs.size(); ++i)
                if (g_dmgs[i].id == did && &g_clients[g_dmgs[i].to] == c) { g_dmgs.erase(g_dmgs.begin() + (long)i); break; }
        } else if (type == MsgGiftAck) {
            uint32_t gid = r.get<uint32_t>();
            if (r.ok()) for (size_t i = 0; i < g_gifts.size(); ++i)
                if (g_gifts[i].id == gid && &g_clients[g_gifts[i].to] == c) { g_gifts.erase(g_gifts.begin() + (long)i); break; }
        } else if (type == MsgLeave) {
            std::printf("[-] jogador %d saiu\n", (int)(c - g_clients));
            std::fflush(stdout);
            clearLife((int)(c - g_clients));
            dropClient((int)(c - g_clients));
        }
    }
}

// LIMPEZA: some com todo navio boiando que ninguém usa (ninguém no leme, nos canhões, a bordo nem encostado no casco). Baú zerado.
static void cleanupShips() {
    for (int i = 0; i < kMaxShips; ++i) {
        ShipState& S = shipState(i);
        if (!shipAfloat(S)) continue;   // afundando: termina de afundar sozinho
        bool used = S.helmer >= 0;
        for (const auto& c : g_clients) if (!used && c.active && !c.dead && shipInUseBy(i, c.st)) used = true;
        if (used) continue;
        S.exists = false; S.speed = 0.0f; S.helmer = -1; S.cmdSteer = 0; S.cmdThrottle = 0; ++S.gen;
        if (chestCount(g_chests[i])) { Chest e; e.ver = (uint16_t)(g_chests[i].ver + 1); g_chests[i] = e; broadcastChest(i); }
        std::printf("[navio] %s SUMIU (ninguem usando)\n", shipDef(i).name); std::fflush(stdout);
    }
}
static void updateShip(float dt) {
    for (int i = 0; i < kMaxShips; ++i) { ShipState& O = shipState(i); if (O.owner >= 0 && (!O.exists || O.owner >= kMaxPlayers || !g_clients[O.owner].active)) O.owner = -1; }   // dono saiu da sala / navio sumiu: fica sem dono
    if (!g_shipsAtStart && g_hasWorld) { g_shipCleanIn -= dt; if (g_shipCleanIn <= 0.0f) { g_shipCleanIn += g_shipCleanSec; cleanupShips(); } }
    for (int i = 0; i < kMaxShips; ++i) {
        ShipState& S = shipState(i);
        if (!S.exists) {   // afundou: renasce no ponto de nascimento original (água aberta perto do spawn da ilha), vida cheia, gen novo
            if (g_shipRespawnIn[i] > 0.0f && (g_shipRespawnIn[i] -= dt) <= 0.0f) {
                if (spawnShipNear(i, g_world.spawnX(), g_world.spawnZ(), 14.0f)) std::printf("[navio] %s RENASCEU\n", shipDef(i).name);
                else g_shipRespawnIn[i] = 5.0f;   // sem água livre agora: tenta de novo
                std::fflush(stdout);
            }
            continue;
        }
        if (S.sinkT > 0.0f) {
            for (auto& c : g_clients) if (c.active && c.st.ship && c.st.shipIdx == i) { shipEject(c.st); c.gunner = false; }
            stepShip(S, dt, terrainPtr());
            if (S.sinkT >= kShipSinkTime) {
                S.exists = false; S.sinkT = 0.0f; S.speed = 0.0f; g_shipRespawnIn[i] = g_shipsAtStart ? std::max(0.05f, g_shipRespawnSec) : 0.0f;
                std::printf("[navio] %s AFUNDOU%s\n", shipDef(i).name, g_shipsAtStart ? " (renasce sozinho)" : " (so volta se alguem chamar)"); std::fflush(stdout);
            }
            continue;
        }
        if (S.helmer >= 0 && (S.helmer >= kMaxPlayers || !g_clients[S.helmer].active || g_clients[S.helmer].st.ship != 2 || g_clients[S.helmer].st.shipIdx != i || g_clients[S.helmer].dead)) {   // timoneiro saiu / morreu
            if (S.helmer < kMaxPlayers && g_clients[S.helmer].st.ship == 2 && g_clients[S.helmer].st.shipIdx == i) g_clients[S.helmer].st.ship = 1;
            shipReleaseHelm(S.helmer);
        }
        // canhoes: no máximo kMaxGunners jogadores por navio ao mesmo tempo (quem pediu primeiro fica; os outros esperam o pedido ser negado)
        int n = 0;
        for (auto& c : g_clients) { if (c.gunner && (!c.active || c.dead || c.st.ship != 1 || !c.wantGun)) c.gunner = false; if (c.gunner && c.st.shipIdx == i) ++n; }
        for (auto& c : g_clients) if (c.active && !c.gunner && c.wantGun && !c.dead && c.st.ship == 1 && c.st.shipIdx == i && shipDef(i).hasCannons && n < kMaxGunners) { c.gunner = true; ++n; }
        stepShip(S, dt, terrainPtr());
    }
    for (auto& c : g_clients) if (c.active && c.st.ship) shipCarry(c.st);   // o convés levou todo mundo junto
}

static uint32_t g_seenStamp[kMaxPlayers]; static uint32_t g_stamp = 0;
// Os outros que entram no snapshot de `me`: os mais perto (até kSnapMaxOthers, raio kInterestRadius). Quem já estava no último
// snapshot vale como 30% mais perto (histerese: ninguém fica piscando na borda); quem está ligado a mim (carrega/é carregado/RCP) entra sempre.
static int pickInterest(int me, int* out) {
    Client& c = g_clients[me];
    if (++g_stamp == 0) { std::fill(g_seenStamp, g_seenStamp + kMaxPlayers, 0u); g_stamp = 1; }
    for (uint16_t id : c.seen) if (id < kMaxPlayers) g_seenStamp[id] = g_stamp;
    struct Cand { float score, d2; int i; };
    static Cand cand[kMaxPlayers];
    int n = 0;
    auto gather = [&](float R) {
        n = 0;
        const float R2 = R * R;
        const int x0 = gridCoord(c.st.x - R), x1 = gridCoord(c.st.x + R), z0 = gridCoord(c.st.z - R), z1 = gridCoord(c.st.z + R);
        for (int gz = z0; gz <= z1; ++gz)
            for (int gx = x0; gx <= x1; ++gx)
                for (int j = g_gridHead[gz * kGridN + gx]; j >= 0; j = g_gridNext[j]) {
                    if (j == me) continue;
                    const Client& o = g_clients[j];
                    const float dx = o.st.x - c.st.x, dz = o.st.z - c.st.z, d2 = dx * dx + dz * dz;
                    if (d2 > R2) continue;
                    const bool linked = j == c.carrying || j == c.carriedBy || j == c.reviveTarget || o.carrying == me || o.carriedBy == me || o.reviveTarget == me;
                    cand[n++] = {linked ? -1.0f : (g_seenStamp[j] == g_stamp ? d2 * 0.49f : d2), d2, j};
                }
    };
    // multidão: procura só até onde estava o 40º no tick anterior (com folga); se não achar 40 ali, procura no raio inteiro
    gather(c.reach);
    if (n < kSnapMaxOthers && c.reach < kInterestRadius) gather(kInterestRadius);
    if (n > kSnapMaxOthers) {
        std::nth_element(cand, cand + kSnapMaxOthers, cand + n, [](const Cand& a, const Cand& b) { return a.score < b.score; });
        n = kSnapMaxOthers;
        float far2 = 0; for (int k = 0; k < n; ++k) far2 = std::max(far2, cand[k].d2);
        c.reach = std::min(kInterestRadius, std::sqrt(far2) * 1.3f + 4.0f);
    } else c.reach = kInterestRadius;
    for (int k = 0; k < n; ++k) out[k] = cand[k].i;
    return n;
}

// ENVIO dos snapshots numa thread própria (2 bancos de buffers alternados): o tick do jogo não espera o kernel entregar 1000 pacotes
constexpr int kSnapBuf = 2048;
struct SnapBank { uint8_t buf[kMaxPlayers][kSnapBuf]; mmsghdr msgs[kMaxPlayers]; iovec iovs[kMaxPlayers]; sockaddr_in addrs[kMaxPlayers]; int n = 0; };
static SnapBank g_banks[2];
static std::mutex g_sendM; static std::condition_variable g_sendCv;
static int g_sendBank = -1; static bool g_sendBusy = false;
static double g_sendCost = 0;
static std::atomic<long> g_sendErrs{0};   // snapshots que o kernel recusou (log [rede])
static void senderThread() {
    for (;;) {
        int b;
        { std::unique_lock<std::mutex> l(g_sendM); g_sendCv.wait(l, [] { return g_sendBank >= 0 || !g_run; }); if (!g_run && g_sendBank < 0) return; b = g_sendBank; g_sendBank = -1; }
        const double t = nowSec();
        SnapBank& B = g_banks[b];
        // sendmmsg devolve quantos foram; erro na mensagem k (k > 0) => devolve k. Antes o lote inteiro parava no 1º erro (todos os
        // jogadores depois dele ficavam sem snapshot neste tick). Agora: buffer cheio => espera o socket (até ~20 ms) e tenta de novo;
        // outro erro (destino inalcançável etc.) => pula SÓ aquela mensagem e continua.
        int waits = 0;
        for (int k = 0; k < B.n;) {
            const int r = sendmmsg(g_fd, B.msgs + k, (unsigned)(B.n - k), 0);
            if (r > 0) { k += r; continue; }
            const int e = errno;
            if (e == EINTR) continue;
            if ((e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS) && waits < 10) { pollfd pf{g_fd, POLLOUT, 0}; poll(&pf, 1, 2); ++waits; continue; }
            ++k; g_sendErrs.fetch_add(1, std::memory_order_relaxed);
        }
        { std::lock_guard<std::mutex> l(g_sendM); g_sendBusy = false; g_sendCost += nowSec() - t; }
        g_sendCv.notify_all();
    }
}
static double g_snapCost = 0, g_snapCostMax = 0, g_buildCost = 0; static long g_snapBytes = 0, g_snapPkts = 0;   // custo do snapshot (log a cada 10 s)
static void broadcastSnapshot(double now) {
    updateShip(1.0f / (float)kTickHz);
    updateRescue(1.0f / (float)kTickHz);
    updateCombat(now);
    const double t0 = nowSec();
    int n = 0;
    for (auto& c : g_clients) if (c.active) { Client::Hist& h = c.hist[g_tick % 64]; h.tick = g_tick; h.x = c.st.x; h.y = c.st.y; h.z = c.st.z; h.hittable = !c.dead && c.carriedBy < 0; ++n; }
    g_activeN = n;
    // sincronias de 1x/s: espalhadas pelos 30 ticks (cada jogador recebe no tick = id % 30) para não juntar 1000 x N pacotes no mesmo tick
    for (int i = (int)(g_tick % 30); i < kMaxPlayers; i += 30) {
        const Client& c = g_clients[i];
        if (!c.active) continue;
        sendTakenSync(c.addr);
        if (!g_drops.empty()) sendDropSync(c.addr);
        for (int si = 0; si < kMaxShips; ++si) if (chestCount(g_chests[si])) sendChestSync(c.addr, si);
    }
    if (!g_gifts.empty()) resendGifts(now);
    {   // meia-noite do ciclo (cycleSec cruza 1350): os recursos renascem
        static float prevCycle = -1.0f;
        const float cyc = cycleNow(now), mid = kDayLengthSec + kNightLengthSec * 0.5f;
        if (g_hasWorld && prevCycle >= 0.0f && prevCycle < mid && cyc >= mid && cyc - prevCycle < 600.0f) renewResources();
        prevCycle = cyc;
    }
    if (n == 0) return;
    buildGrid();
    static int others[kMaxPlayers];
    static int bank = 0;
    { std::unique_lock<std::mutex> l(g_sendM); g_sendCv.wait(l, [] { return !g_sendBusy; }); }   // o envio anterior (outro banco) já terminou? (normal: sim, leva poucos ms)
    SnapBank& B = g_banks[bank];
    int nm = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        Client& c = g_clients[i];
        if (!c.active) continue;
        const int no = pickInterest(i, others);
        uint8_t* buf = B.buf[nm];
        Writer w(buf, kSnapBuf);
        w.header(MsgSnapshot);
        w.put<uint32_t>(g_tick);
        w.put<float>(cycleNow(now));
        w.put<uint32_t>(c.echoMs);
        w.put<uint16_t>((uint16_t)std::min(65535.0, std::max(0.0, (now - c.echoAt) * 1000.0)));
        w.put<uint16_t>((uint16_t)no);
        w.state(c.st); w.put<uint32_t>(c.lastSeq);
        c.seen.resize((size_t)no);
        for (int k = 0; k < no; ++k) { w.stateRel(g_clients[others[k]].st, c.st.x, c.st.z); c.seen[(size_t)k] = (uint16_t)others[k]; }
        for (int si = 0; si < kMaxShips; ++si) w.ship(shipState(si));
        w.put<uint8_t>(g_shipsAtStart || !g_hasWorld ? 255 : (uint8_t)std::min(254.0f, std::ceil(std::max(0.0f, g_shipCleanIn))));   // s até a LIMPEZA dos navios
        if (!w.ok()) continue;
        B.addrs[nm] = c.addr;
        B.iovs[nm] = {buf, w.size()};
        B.msgs[nm] = mmsghdr{};
        B.msgs[nm].msg_hdr.msg_name = &B.addrs[nm]; B.msgs[nm].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        B.msgs[nm].msg_hdr.msg_iov = &B.iovs[nm]; B.msgs[nm].msg_hdr.msg_iovlen = 1;
        ++nm;
        g_snapBytes += (long)w.size(); ++g_snapPkts;
    }
    g_buildCost += nowSec() - t0;
    B.n = nm;
    { std::lock_guard<std::mutex> l(g_sendM); g_sendBank = bank; g_sendBusy = true; }
    g_sendCv.notify_all();
    bank ^= 1;
    const double cost = nowSec() - t0;
    g_snapCost += cost; g_snapCostMax = std::max(g_snapCostMax, cost);
    if (g_tick % (kTickHz * 10) == 0) {
        std::printf("[rede] %d jogadores | snapshot: %.2f ms/tick no jogo (max %.2f) + envio %.2f ms/tick em outra thread | %.0f B/pacote | %.1f MB/s enviados | %ld recusados pelo kernel\n", n, g_snapCost * 1000.0 / (kTickHz * 10), g_snapCostMax * 1000.0, g_sendCost * 1000.0 / (kTickHz * 10),
                    g_snapPkts ? (double)g_snapBytes / g_snapPkts : 0.0, g_snapBytes / 10.0 / 1048576.0, g_sendErrs.exchange(0));
        std::fflush(stdout);
        g_snapCost = g_snapCostMax = g_buildCost = 0; { std::lock_guard<std::mutex> l(g_sendM); g_sendCost = 0; } g_snapBytes = g_snapPkts = 0;
    }
}

int main(int argc, char** argv) {
    uint16_t port = kDefaultPort;
    int pos = 0;
    bool flat = false, forceNew = false, noIslands = false;
    uint32_t seed = kDefaultWorldSeed;
    std::string worldPath = "worlds/world.e0w", dataDir;
    for (int i = 1; i < argc; ++i) {  // uso: server [porta] [hora_inicial_s] [--debug]
        if (std::strcmp(argv[i], "--debug") == 0) g_debugTime = true;
        else if (std::strcmp(argv[i], "--flat") == 0) flat = true;
        else if (std::strcmp(argv[i], "--new") == 0) forceNew = true;
        else if (std::strcmp(argv[i], "--no-protect") == 0) g_spawnProtect = 0.0;   // sem invulnerabilidade ao nascer (testes de dano)
        else if (std::strcmp(argv[i], "--no-islands") == 0) noIslands = true;   // só a ilha inicial (testes)
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--world") == 0 && i + 1 < argc) worldPath = argv[++i];
        else if (std::strcmp(argv[i], "--ship-cleanup") == 0 && i + 1 < argc) { g_shipCleanSec = std::max(1.0f, (float)std::atof(argv[++i])); g_shipCleanIn = g_shipCleanSec; }   // período da limpeza (padrão 60)
        else if (std::strcmp(argv[i], "--own-all-ships") == 0) g_ownAllShips = true;
        else if (std::strcmp(argv[i], "--fcm-cmd") == 0 && i + 1 < argc) g_fcmCmd = argv[++i];   // ajudante do PUSH (Firebase Cloud Messaging)
        else if (std::strcmp(argv[i], "--cap-time-scale") == 0 && i + 1 < argc) g_capScale = std::max(0.01f, (float)std::atof(argv[++i]));   // DOMINAÇÃO (testes): tempos x X
        else if (std::strcmp(argv[i], "--payout-sec") == 0 && i + 1 < argc) g_payoutSec = std::max(2, std::atoi(argv[++i]));                  // DOMINAÇÃO (testes): renda a cada S s
        else if (std::strcmp(argv[i], "--ships-at-start") == 0) g_shipsAtStart = true;   // testes antigos: navios nascem ao subir e renascem sozinhos, sem limpeza
        else if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) { g_dataOn = true; dataDir = argv[++i]; }
        else if (std::strcmp(argv[i], "--jwk") == 0 && i + 1 < argc) g_jwkPath = argv[++i];
        else if (std::strcmp(argv[i], "--firebase-project") == 0 && i + 1 < argc) g_fbProject = argv[++i];
        else if (std::strcmp(argv[i], "--reject-unverified-receipts") == 0) g_rejectUnverified = true;
        else if (std::strcmp(argv[i], "--admins") == 0 && i + 1 < argc) g_adminsPath = argv[++i];   // lista de admins (padrão <data>/admins.txt)
        else if (std::strcmp(argv[i], "--ship-respawn") == 0 && i + 1 < argc) g_shipRespawnSec = (float)std::atof(argv[++i]);   // s até o navio afundado renascer (padrão 60)
        else if (pos == 0) { port = (uint16_t)std::atoi(argv[i]); ++pos; }
        else if (pos == 1) { g_cycleStart = std::fmod(std::atof(argv[i]), (double)kCycleSec); ++pos; }
    }
    g_t0 = nowSec();
    {   // segredo do cookie do handshake (aleatório a cada execução)
        FILE* ur = std::fopen("/dev/urandom", "rb");
        if (!ur || std::fread(&g_cookieSecret, sizeof g_cookieSecret, 1, ur) != 1) g_cookieSecret = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ ((uint64_t)getpid() << 32);
        if (ur) std::fclose(ur);
    }
    if (g_dataOn) {
        if (!g_store.open(dataDir)) { std::fprintf(stderr, "ERRO: nao consegui abrir/criar o diretorio de dados '%s'\n", dataDir.c_str()); return 1; }
        if (g_jwkPath.empty()) g_jwkPath = dataDir + "/firebase_jwk.json";
        if (g_adminsPath.empty()) g_adminsPath = dataDir + "/admins.txt";
    }
    if (g_debugTime) std::printf("Admins: --debug ligado (TODOS podem usar os comandos de debug)\n");
    else if (g_adminsPath.empty()) std::printf("Admins: nenhum (sem --data nem --admins): comandos de debug ignorados para todos\n");
    else reloadAdmins(nowSec(), true);
    if (!g_jwkPath.empty()) { g_keys.refresh(g_jwkPath); g_keysCheckAt = nowSec() + 30.0; }
    std::printf("Contas: %s | idToken do Firebase (projeto %s): %zu chaves publicas em '%s'%s\n", g_dataOn ? ("PERSISTENTES em '" + dataDir + "'").c_str() : "SO NA MEMORIA (sem --data)",
                g_fbProject.c_str(), g_keys.size(), g_jwkPath.empty() ? "-" : g_jwkPath.c_str(), g_keys.size() ? "" : "  [SEM CHAVES: login do Google/Firebase vai ser recusado ate o arquivo existir]");
    signal(SIGINT, [](int) { g_run = 0; });
    signal(SIGTERM, [](int) { g_run = 0; });
    g_fd = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(g_fd, F_SETFL, fcntl(g_fd, F_GETFL, 0) | O_NONBLOCK);
    int tos = 0xB8;  // DSCP EF: prioridade no Wi-Fi (WMM) / QoS do roteador
    setsockopt(g_fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    int sockBuf = 8 * 1024 * 1024;   // 1000 jogadores: ~30 mil pacotes/s em cada sentido
    setsockopt(g_fd, SOL_SOCKET, SO_RCVBUF, &sockBuf, sizeof sockBuf);
    setsockopt(g_fd, SOL_SOCKET, SO_SNDBUF, &sockBuf, sizeof sockBuf);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port);
    if (bind(g_fd, (sockaddr*)&a, sizeof a) < 0) { std::perror("bind"); return 1; }
    if (!flat) {  // mundo: carrega do arquivo se existir (assim mudar o gerador depois não altera um mundo já criado)
        bool loaded = !forceNew && World::load(worldPath, g_world);
        if (!loaded && !forceNew) {  // o arquivo existe mas não carrega (corrompido / versão incompatível)? guarda uma cópia antes de recriar
            if (FILE* f = std::fopen(worldPath.c_str(), "rb")) {
                std::fclose(f);
                std::string bak = worldPath + ".invalido-" + std::to_string((long)std::time(nullptr));
                if (std::rename(worldPath.c_str(), bak.c_str()) == 0) std::printf("AVISO: '%s' invalido (CRC/versao): guardado como '%s'; criando mundo novo\n", worldPath.c_str(), bak.c_str());
            }
        }
        if (loaded && g_world.upgraded) {   // arquivo de versão antiga: foi atualizado em memória (+ coletáveis / tecidos); regrava com backup
            std::string bak = worldPath + ".pre-v" + std::to_string(World::kGenVersion) + ".bak";   // original (v2/v3) guardado antes da atualização
            std::rename(worldPath.c_str(), bak.c_str());
            bool saved = g_world.save(worldPath);
            std::printf("Mundo ATUALIZADO da versao anterior (+ coletaveis / tecidos da praia): %s; original guardado em '%s'\n", saved ? "regravado" : "ERRO ao regravar", bak.c_str());
        }
        if (loaded) {
            std::printf("Mundo carregado de '%s' (semente %u, hash %08x)\n", worldPath.c_str(), g_world.seed, g_world.hash);
        } else {
            if (!World::generate(seed, g_world)) { std::fprintf(stderr, "falha ao gerar o mundo (semente %u)\n", seed); return 1; }
            size_t slash = worldPath.rfind('/');
            if (slash != std::string::npos) mkdir(worldPath.substr(0, slash).c_str(), 0755);
            bool saved = g_world.save(worldPath);
            std::printf("Mundo GERADO (semente %u, hash %08x) e %s em '%s'\n", seed, g_world.hash, saved ? "SALVO" : "NAO SALVO (erro de gravacao)", worldPath.c_str());
        }
        World::Stats st = g_world.stats();
        std::printf("  ilha %.0f m | areia %.1f%% terra %.1f%% grama %.1f%% floresta %.1f%% pedra %.1f%% | arvores %d pedras %d troncos %d construcoes %d | coletaveis: gravetos %d pedrinhas %d | spawn (%.0f, %.0f)\n",
                    st.extentM, st.pct[BiomeSand], st.pct[BiomeDirt], st.pct[BiomeGrass], st.pct[BiomeForest], st.pct[BiomeRock],
                    st.count[ObjPalm] + st.count[ObjCoconut], st.count[ObjRock], st.count[ObjLog], st.count[ObjCabin] + st.count[ObjShed] + st.count[ObjTower], st.count[ObjStick], st.count[ObjPebble], g_world.spawnX(), g_world.spawnZ());
        g_hasWorld = true; g_worldHash = g_world.hash;
        g_worldBytes = std::make_shared<std::vector<uint8_t>>(g_world.toBytes());
        setTerrain(&g_world);
        g_is[0].w = &g_world;
        if (!noIslands) {   // OCEANO ABERTO: gera todas as ilhas do layout (os clientes regeneram idênticas da mesma semente)
            const double tIs = nowSec(); int nk[IslandKindCount] = {}, total = 0;
            for (int sl = 1; sl < kIslandSlots; ++sl) {
                int sx, sz; sectorOfSlot(sl, sx, sz);
                IslandSpec sp = islandSpec(g_world.seed, sx, sz);
                if (!sp.exists) continue;
                auto w = std::make_unique<World>();
                if (!World::generateIsland(sp.seed, sp.kind, sp.oxcm, sp.ozcm, *w)) { std::printf("AVISO: ilha do setor (%d,%d) nao gerou\n", sx, sz); continue; }
                g_is[sl].own = std::move(w); g_is[sl].w = g_is[sl].own.get(); terrain().setIsland(sl, g_is[sl].w);
                ++nk[sp.kind]; ++total;
            }
            std::printf("Oceano aberto: %d ilhas em %.1f s |", total, nowSec() - tIs);
            for (int k = 0; k < IslandKindCount; ++k) std::printf(" %s %d%s", islandKindName((uint8_t)k), nk[k], k + 1 < IslandKindCount ? "," : "\n");
        }
        if (g_dataOn) { loadWorldState(); teamsLoad(); }
        capInit();
        if (g_shipsAtStart) { for (int i = 0; i < kMaxShips; ++i) if (!spawnShipNear(i, g_world.spawnX(), g_world.spawnZ(), 14.0f)) std::printf("AVISO: sem agua funda para %s\n", shipDef(i).name); }
        else std::printf("Navios: nenhum nasce ao subir (so por CHAMADO); limpeza a cada %.0f s dos navios sem ninguem\n", g_shipCleanSec);
        g_tcp = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(g_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(g_tcp, (sockaddr*)&a, sizeof a) < 0 || listen(g_tcp, 64) < 0) { std::perror("bind/listen TCP (download do mundo)"); return 1; }
        fcntl(g_tcp, F_SETFL, fcntl(g_tcp, F_GETFL, 0) | O_NONBLOCK);
    } else {
        std::printf("Modo --flat: sem terreno (chao plano em y=0)\n");
    }
    std::printf("Isolated server: UDP :%d, %d Hz, max %d jogadores, ciclo %.0f s (dia %.0f + noite %.0f), hora inicial %.0f s%s\n", port, kTickHz, kMaxPlayers, kCycleSec, kDayLengthSec, kNightLengthSec, g_cycleStart, g_debugTime ? "  [--debug: hora alteravel pelos clientes]" : "");
    std::fflush(stdout);

    std::thread sender(senderThread);
    std::thread pusher;
    if (!g_fcmCmd.empty()) { pusher = std::thread(pushThread); std::printf("Push (FCM): ligado, ajudante '%s'\n", g_fcmCmd.c_str()); }
    else std::printf("Push (FCM): desligado (sem --fcm-cmd)\n");
    const double dtTick = 1.0 / kTickHz;
    double next = nowSec() + dtTick;
    while (g_run) {
        double now = nowSec();
        int waitMs = (int)std::max(0.0, (next - now) * 1000.0);
        pollfd p[2] = {{g_fd, POLLIN, 0}, {g_tcp, POLLIN, 0}};
        poll(p, g_tcp >= 0 ? 2 : 1, waitMs);
        now = nowSec();
        receiveAll(now);
        reloadAdmins(now, false);   // lista de admins: stat a cada 2 s, relê se mudou
        g_store.poll([](persist::Store::Loaded& L) { finishLoad(L); });
        updateTeams(now);
        while (g_tcp >= 0) {  // clientes pedindo o mundo: cada transferência roda em sua própria thread (não trava o jogo)
            int cfd = accept(g_tcp, nullptr, nullptr);
            if (cfd < 0) break;
            if (g_serving.load() >= 64) { close(cfd); continue; }
            ++g_serving;
            std::thread([cfd] {   // 4 bytes dizem o serviço: "E0WG" = download do mundo, "E0TQ" = TRIPULAÇÃO (tela do menu)
                e0net::teamTimeouts(cfd, 5000);
                uint8_t mg[4];
                if (!e0net::teamRecvAll(cfd, mg, 4)) close(cfd);
                else if (std::memcmp(mg, "E0WG", 4) == 0) serveWorldAfterMagic(cfd, *g_worldBytes, g_worldHash);
                else if (std::memcmp(mg, "E0TQ", 4) == 0) serveTeamConnection(cfd);
                else close(cfd);
                --g_serving;
            }).detach();
        }
        if (now >= next) {
            next += dtTick;
            if (next < now) next = now + dtTick;  // recuperação após travada
            ++g_tick;
            for (int i = 0; i < kMaxPlayers; ++i) {
                Client& c = g_clients[i];
                if (c.active && now - c.lastRecv > kTimeoutSec) {
                    std::printf("[-] jogador %d timeout\n", i); std::fflush(stdout);
                    clearLife(i);
                    dropClient(i);
                }
            }
            broadcastSnapshot(now);
            updateAccounts(now);
            updateTeamOffers(now);
            if (g_tick % kTickHz == 0) { updateTeamPositions(); flushPushes(now); }
            if (g_tick % 3 == 0) updateConquest(now);
            if (g_tick % 6 == 0) sendConquest(now);
            updateStructSync();
            updateIslandOwnersSync();
            if (g_tick % (kTickHz * 60) == 0) saveWorldState(false);
        }
    }
    // DESLIGANDO (SIGTERM do systemd / Ctrl+C): grava todas as contas e o estado do mundo e espera o disco
    for (int i = 0; i < kMaxPlayers; ++i) if (g_clients[i].active) endSession(i);
    int nSaved = 0;
    for (auto& kv : g_accounts) if (kv.second->dirty) { saveAccount(*kv.second, true); ++nSaved; }
    saveWorldState(true);
    teamsSave(true);
    { std::lock_guard<std::mutex> l(g_teamM); } g_teamCv.notify_all();   // pedidos da TRIPULAÇÃO ainda esperando: soltam (g_run = 0)
    if (g_dataOn) { g_store.flush(); std::printf("dados gravados (%d contas pendentes, %ld gravacoes no total, %ld erros)\n", nSaved, g_store.writes(), g_store.writeErrors()); g_store.close(); }
    { std::lock_guard<std::mutex> l(g_sendM); } g_sendCv.notify_all(); sender.join();
    if (pusher.joinable()) { flushPushes(1e18); { std::lock_guard<std::mutex> l(g_pushM); g_pushStop = true; } g_pushCv.notify_all(); pusher.join(); }
    std::printf("servidor encerrado\n");
    return 0;
}
