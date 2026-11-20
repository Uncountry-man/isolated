// Isolated - TRIPULAÇÃO (equipe de jogadores): regras e o SERVIÇO TCP compartilhados entre servidor, app e ferramentas (header-only, POSIX).
//
// A tela da TRIPULAÇÃO fica no MENU PRINCIPAL, antes de entrar no jogo (sem conexão UDP). Por isso ela fala com o servidor por TCP, na
// MESMA porta do download do mundo (world_sync): o servidor lê os 4 primeiros bytes ("E0WG" = mundo, "E0TQ" = tripulação).
//
//   pedido   C->S : "E0TQ" [len u32] [payload]          (len <= kTeamMaxReq)
//   resposta S->C : "E0TR" [len u32] [payload]
//   payload do pedido : [ver u8 = kTeamProtoVer][auth u8 = LoginKind][uid str8][token str16][nome str8][op u8][argumentos da op]
//   payload da resposta: [result u8 = TeamResult][estado (TeamView::write)]  (estado sempre vem, mesmo com erro: a tela se atualiza)
//
// Regras: criar exige a LICENÇA DE TRIPULAÇÃO (kTeamLicenseCost moedas, comprada na tela ou na LOJA; gasta ao criar). Entrar numa
// tripulação pelo CÓDIGO de convite é grátis. Mensagens do chat duram kTeamMsgTtl (24 h). O servidor é a autoridade de tudo.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"

namespace e0net {

constexpr uint8_t kTeamProtoVer = 3;   // v2: id no jogo de cada membro online (TeamMember.slot); v3: COFRE (depositar/sacar) e LOJA da tripulação (vagas e navios)
constexpr int kTeamLicenseCost = 3000;        // moedas para criar uma tripulação (licença)
constexpr int kTeamNameMin = 3, kTeamNameMax = 20;
constexpr int kTeamCodeLen = 6;
constexpr int kTeamMaxMembers = 30;           // teto absoluto (com todas as expansões)
constexpr int kTeamBaseMembers = 10;          // vagas de uma tripulação nova; cada expansão da LOJA dá +kTeamSlotStep
constexpr int kTeamSlotStep = 5;
constexpr int kTeamSlotLevels = 4;            // 10 -> 15 -> 20 -> 25 -> 30
constexpr int kTeamSlotPrice[kTeamSlotLevels] = {2500, 5000, 10000, 20000};   // moedas do COFRE, por nível
constexpr int kTeamShipMul = 2;               // navio da tripulação = kShipPrice x 2 (serve a todos os tripulantes, sem custo por chamado)
constexpr int kTeamShipKinds = 4;
constexpr int kTeamShipPrice[kTeamShipKinds] = {kShipPrice[0] * kTeamShipMul, kShipPrice[1] * kTeamShipMul, kShipPrice[2] * kTeamShipMul, kShipPrice[3] * kTeamShipMul};
inline int teamCapacity(int extraSlots) { return std::min(kTeamMaxMembers, kTeamBaseMembers + kTeamSlotStep * std::max(0, extraSlots)); }
constexpr int kTeamMsgMax = kChatMaxLen;      // caracteres por mensagem (= chat, 80)
constexpr int kTeamMsgKeep = 400;             // máx. de mensagens guardadas por tripulação
constexpr int kTeamMsgSend = 120;             // máx. de mensagens numa resposta
constexpr int64_t kTeamMsgTtl = 24 * 3600;    // s: mensagens somem depois de 24 h
constexpr double kTeamChatGap = 1.0;          // s entre mensagens da mesma conta
constexpr uint32_t kTeamMaxReq = 4096;
constexpr uint32_t kTeamMaxResp = 256 * 1024;
constexpr int kPlayerNameMax = 16;

// ---- no JOGO (UDP) ----
constexpr int kTeamQueryMax = 48;            // ids por MsgTeamQuery
constexpr float kTeamInviteDist = 8.0f;      // m: convidar só quem está perto
constexpr double kTeamOfferTtl = 60.0;       // s: o convite some se ninguém responder
constexpr double kTeamInviteGap = 2.0;       // s entre convites do mesmo jogador
enum TeamNoticeKind : uint8_t { TnInviteSent = 1, TnAccepted = 2, TnDeclined = 3, TnJoined = 4, TnOfferGone = 5 };

enum TeamOp : uint8_t { TeamGet = 0, TeamBuyLicense = 1, TeamCreate = 2, TeamJoin = 3, TeamLeave = 4, TeamRename = 5, TeamSetFlag = 6,
                        TeamChat = 7, TeamKick = 8, TeamPromote = 9, TeamPushToken = 10,
                        TeamDeposit = 11, TeamWithdraw = 12, TeamBuy = 13 };   // Deposit: qualquer membro (amount); Withdraw: só o capitão (amount); Buy: só o capitão, do COFRE (index 0 = +vagas, 1..4 = navio tipo index-1)   // Promote: o capitão passa o cargo (index = membro); PushToken: token FCM do aparelho (text)
enum TeamResult : uint8_t { TeamOk = 1, TeamErrAuth = 2, TeamErrExpired = 3, TeamErrCoins = 4, TeamErrNoLicense = 5, TeamErrHasLicense = 6,
                            TeamErrInTeam = 7, TeamErrNoTeam = 8, TeamErrName = 9, TeamErrNameTaken = 10, TeamErrCode = 11, TeamErrFull = 12,
                            TeamErrNotOwner = 13, TeamErrFlag = 14, TeamErrText = 15, TeamErrSlow = 16, TeamErrBusy = 17, TeamErrBadReq = 18,
                            TeamErrNoData = 19, TeamErrTarget = 20, TeamErrTargetTeam = 21, TeamErrFar = 22, TeamErrNoLogin = 23,
                            TeamErrTreasury = 24, TeamErrOwned = 25, TeamErrMaxed = 26, TeamErrAmount = 27 };
inline const char* teamResultText(uint8_t r) {
    switch (r) {
        case TeamOk: return "OK";
        case TeamErrAuth: return "CONTA N\xC3" "O RECONHECIDA";
        case TeamErrExpired: return "SESS\xC3" "O EXPIRADA, TENTANDO DE NOVO";
        case TeamErrCoins: return "MOEDAS INSUFICIENTES";
        case TeamErrNoLicense: return "COMPRE A LICEN\xC7" "A PRIMEIRO";
        case TeamErrHasLicense: return "VOC\xCA J\xC1 TEM A LICEN\xC7" "A";
        case TeamErrInTeam: return "VOC\xCA J\xC1 EST\xC1 NUMA TRIPULA\xC7\xC3" "O";
        case TeamErrNoTeam: return "VOC\xCA N\xC3" "O EST\xC1 NUMA TRIPULA\xC7\xC3" "O";
        case TeamErrName: return "NOME INV\xC1LIDO (3 A 20 LETRAS)";
        case TeamErrNameTaken: return "J\xC1 EXISTE UMA TRIPULA\xC7\xC3" "O COM ESSE NOME";
        case TeamErrCode: return "C\xD3" "DIGO N\xC3" "O ENCONTRADO";
        case TeamErrFull: return "TRIPULA\xC7\xC3" "O CHEIA";
        case TeamErrNotOwner: return "S\xD3 O CAPIT\xC3" "O PODE FAZER ISSO";
        case TeamErrFlag: return "BANDEIRA INV\xC1LIDA";
        case TeamErrText: return "MENSAGEM VAZIA";
        case TeamErrSlow: return "DEVAGAR: ESPERE UM POUCO";
        case TeamErrBusy: return "SERVIDOR OCUPADO, TENTE DE NOVO";
        case TeamErrNoData: return "SERVIDOR SEM CONTAS (SEM --DATA)";
        case TeamErrTarget: return "ESSE JOGADOR N\xC3" "O PODE SER CONVIDADO AGORA";
        case TeamErrTargetTeam: return "ESSE JOGADOR J\xC1 TEM TRIPULA\xC7\xC3" "O";
        case TeamErrFar: return "CHEGUE MAIS PERTO PARA CONVIDAR";
        case TeamErrNoLogin: return "ENTRE NUMA CONTA PARA USAR TRIPULA\xC7\xC3" "O";
        case TeamErrTreasury: return "O COFRE N\xC3" "O TEM MOEDAS SUFICIENTES";
        case TeamErrOwned: return "A TRIPULA\xC7\xC3" "O J\xC1 TEM ISSO";
        case TeamErrMaxed: return "J\xC1 NO M\xC1XIMO";
        case TeamErrAmount: return "VALOR INV\xC1LIDO";
        default: return "PEDIDO RECUSADO";
    }
}

// ---------------- buffer simples (little-endian, como o resto do protocolo) ----------------
struct TBufW {
    std::vector<uint8_t> b;
    template <class T> void put(T v) { const uint8_t* p = (const uint8_t*)&v; b.insert(b.end(), p, p + sizeof(T)); }
    void str8(const std::string& s) { const size_t n = std::min<size_t>(s.size(), 255); put<uint8_t>((uint8_t)n); b.insert(b.end(), s.begin(), s.begin() + (long)n); }
    void str16(const std::string& s) { const size_t n = std::min<size_t>(s.size(), 65535); put<uint16_t>((uint16_t)n); b.insert(b.end(), s.begin(), s.begin() + (long)n); }
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};
struct TBufR {
    const uint8_t* p; size_t n, i = 0; bool ok = true;
    TBufR(const uint8_t* d, size_t len) : p(d), n(len) {}
    template <class T> T get() { T v{}; if (i + sizeof(T) > n) { ok = false; return v; } std::memcpy(&v, p + i, sizeof(T)); i += sizeof(T); return v; }
    std::string str8() { const uint8_t l = get<uint8_t>(); if (!ok || i + l > n) { ok = false; return {}; } std::string s((const char*)p + i, l); i += l; return s; }
    std::string str16() { const uint16_t l = get<uint16_t>(); if (!ok || i + l > n) { ok = false; return {}; } std::string s((const char*)p + i, l); i += l; return s; }
    void raw(void* d, size_t k) { if (i + k > n) { ok = false; return; } std::memcpy(d, p + i, k); i += k; }
};

// Nome da tripulação / do jogador: mesma limpeza do chat (Latin-1), sem passar de maxLen.
inline std::string teamCleanText(const std::string& in, int maxLen) {
    char out[kChatMaxLen + 1];
    const int n = chatSanitize(in.data(), (int)std::min<size_t>(in.size(), 200), out);
    return std::string(out, (size_t)std::min(n, maxLen));
}
// Código de convite: só A-Z e 2-9 (sem 0/O/1/I para não confundir), maiúsculas.
inline const char* teamCodeAlphabet() { return "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; }
inline std::string teamCleanCode(const std::string& in) {
    std::string s;
    for (char ch : in) {
        char c = (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
        if (std::strchr(teamCodeAlphabet(), c) && c) s += c;
        if ((int)s.size() >= kTeamCodeLen) break;
    }
    return s;
}

// ---------------- o que a tela mostra (resposta) ----------------
struct TeamMember { std::string name; bool owner = false, online = false, me = false; int slot = -1; };   // slot = id no jogo (online)
struct TeamMsg { uint32_t id = 0; uint32_t age = 0; std::string author; std::string text; bool mine = false; };
struct TeamView {
    int32_t coins = 0;
    bool license = false, inTeam = false, owner = false;
    uint32_t teamId = 0;
    std::string name, code;
    uint8_t flag[kFlagBytes] = {};
    std::vector<TeamMember> members;
    std::vector<TeamMsg> msgs;       // só as com id > "since" do pedido (o cliente junta)
    uint32_t lastMsgId = 0;          // maior id existente na tripulação (o cliente usa como "since")
    int32_t treasury = 0;            // COFRE da tripulação (moedas)
    uint8_t slotLevel = 0;           // expansões de vagas compradas (0..kTeamSlotLevels)
    uint8_t ships = 0;               // bit k = a tripulação comprou o navio do tipo k (qualquer tripulante chama sem custo)
    int capacity() const { return teamCapacity(slotLevel); }

    void write(TBufW& w) const {
        w.put<int32_t>(coins);
        w.put<uint8_t>((uint8_t)((license ? 1 : 0) | (inTeam ? 2 : 0) | (owner ? 4 : 0)));
        if (!inTeam) return;
        w.put<uint32_t>(teamId); w.str8(name); w.str8(code); w.raw(flag, kFlagBytes);
        w.put<int32_t>(treasury); w.put<uint8_t>(slotLevel); w.put<uint8_t>(ships);
        w.put<uint8_t>((uint8_t)std::min<size_t>(members.size(), 255));
        for (size_t i = 0; i < members.size() && i < 255; ++i) { const TeamMember& m = members[i]; w.str8(m.name); w.put<uint8_t>((uint8_t)((m.owner ? 1 : 0) | (m.online ? 2 : 0) | (m.me ? 4 : 0))); w.put<uint16_t>((uint16_t)(m.slot < 0 ? 0xFFFF : m.slot)); }
        w.put<uint32_t>(lastMsgId);
        w.put<uint16_t>((uint16_t)std::min<size_t>(msgs.size(), 65535));
        for (const TeamMsg& m : msgs) { w.put<uint32_t>(m.id); w.put<uint32_t>(m.age); w.str8(m.author); w.str8(m.text); w.put<uint8_t>(m.mine ? 1 : 0); }
    }
    bool read(TBufR& r) {
        coins = r.get<int32_t>();
        const uint8_t f = r.get<uint8_t>();
        license = f & 1; inTeam = (f & 2) != 0; owner = (f & 4) != 0;
        members.clear(); msgs.clear(); name.clear(); code.clear(); teamId = 0; lastMsgId = 0; treasury = 0; slotLevel = 0; ships = 0; std::memset(flag, 0, sizeof flag);
        if (!inTeam) return r.ok;
        teamId = r.get<uint32_t>(); name = r.str8(); code = r.str8(); r.raw(flag, kFlagBytes);
        treasury = r.get<int32_t>(); slotLevel = r.get<uint8_t>(); ships = r.get<uint8_t>();
        const uint8_t nm = r.get<uint8_t>();
        for (int i = 0; i < nm && r.ok; ++i) { TeamMember m; m.name = r.str8(); const uint8_t mf = r.get<uint8_t>(); m.owner = mf & 1; m.online = (mf & 2) != 0; m.me = (mf & 4) != 0; const uint16_t sl = r.get<uint16_t>(); m.slot = sl == 0xFFFF ? -1 : (int)sl; members.push_back(m); }
        lastMsgId = r.get<uint32_t>();
        const uint16_t nn = r.get<uint16_t>();
        for (int i = 0; i < nn && r.ok; ++i) { TeamMsg m; m.id = r.get<uint32_t>(); m.age = r.get<uint32_t>(); m.author = r.str8(); m.text = r.str8(); m.mine = r.get<uint8_t>() != 0; msgs.push_back(m); }
        return r.ok;
    }
};

// ---------------- pedido ----------------
struct TeamRequest {
    uint8_t authKind = 0;            // LoginLocal / LoginFirebase
    std::string uid, token, playerName;
    uint8_t op = TeamGet;
    uint32_t since = 0;              // TeamGet/qualquer: mensagens com id > since
    std::string text;                // Create/Rename: nome; Join: código; Chat: mensagem
    uint8_t flag[kFlagBytes] = {};   // Create/SetFlag
    uint8_t index = 0;               // Kick: índice do membro na lista; Buy: o que comprar
    uint32_t amount = 0;             // Deposit/Withdraw: moedas
    void write(TBufW& w) const {
        w.put<uint8_t>(kTeamProtoVer); w.put<uint8_t>(authKind); w.str8(uid); w.str16(token); w.str8(playerName);
        w.put<uint8_t>(op); w.put<uint32_t>(since); w.str8(text); w.raw(flag, kFlagBytes); w.put<uint8_t>(index); w.put<uint32_t>(amount);
    }
    bool read(TBufR& r) {
        if (r.get<uint8_t>() != kTeamProtoVer) return false;
        authKind = r.get<uint8_t>(); uid = r.str8(); token = r.str16(); playerName = r.str8();
        op = r.get<uint8_t>(); since = r.get<uint32_t>(); text = r.str8(); r.raw(flag, kFlagBytes); index = r.get<uint8_t>(); amount = r.get<uint32_t>();
        return r.ok;
    }
};

// ---------------- transporte TCP ----------------
inline bool teamSendAll(int fd, const uint8_t* d, size_t n) {
    while (n > 0) { ssize_t k = send(fd, d, n, MSG_NOSIGNAL); if (k <= 0) return false; d += k; n -= (size_t)k; }
    return true;
}
inline bool teamRecvAll(int fd, uint8_t* d, size_t n) {
    while (n > 0) { ssize_t k = recv(fd, d, n, 0); if (k <= 0) return false; d += k; n -= (size_t)k; }
    return true;
}
inline void teamTimeouts(int fd, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
// Cliente: manda o pedido e espera a resposta (bloqueante: chamar numa thread). false = sem conexão/resposta inválida.
inline bool teamCall(const char* ip, uint16_t port, const TeamRequest& req, uint8_t& result, TeamView& view, int timeoutMs = 6000) {
    in_addr ia{};
    if (!ip || inet_pton(AF_INET, ip, &ia) != 1) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr = ia; a.sin_port = htons(port);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeoutMs) <= 0) { close(fd); return false; }
        int err = 0; socklen_t el = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return false; }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    teamTimeouts(fd, timeoutMs);
    TBufW w; req.write(w);
    TBufW pkt; pkt.raw("E0TQ", 4); pkt.put<uint32_t>((uint32_t)w.b.size()); pkt.raw(w.b.data(), w.b.size());
    uint8_t head[8];
    bool ok = teamSendAll(fd, pkt.b.data(), pkt.b.size()) && teamRecvAll(fd, head, 8) && std::memcmp(head, "E0TR", 4) == 0;
    std::vector<uint8_t> body;
    if (ok) {
        uint32_t len; std::memcpy(&len, head + 4, 4);
        if (len < 1 || len > kTeamMaxResp) ok = false;
        else { body.resize(len); ok = teamRecvAll(fd, body.data(), len); }
    }
    close(fd);
    if (!ok) return false;
    TBufR r(body.data(), body.size());
    result = r.get<uint8_t>();
    return view.read(r) && r.ok;
}

}  // namespace e0net
