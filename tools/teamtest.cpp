// Testes da TRIPULAÇÃO (serviço TCP "E0TQ" do servidor; shared/team.h, server/teams.h). Usado por tools/team_test.sh:
//   teamtest seed DIR_DADOS               grava contas de teste (moedas) e uma tripulação com uma mensagem de 25 h (deve sumir)
//   teamtest run PORTA DIR_JWT            fase 1: licença, criar, entrar pelo código, chat, renomear, bandeira, expulsar, sair...
//   teamtest after PORTA DIR_JWT          fase 2 (depois de REINICIAR o servidor): tudo continua lá
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "../shared/protocol.h"
#include "../server/persist.h"
#include "../server/teams.h"

using namespace e0net;

static int fails = 0;
#define CHECK(c, ...) do { bool _ok = (c); std::printf("[%s] ", _ok ? " OK " : "FALHA"); std::printf(__VA_ARGS__); std::printf("\n"); if (!_ok) ++fails; } while (0)

static uint16_t g_port = 7796;
static std::string g_jwt;
static std::string readTok(const char* f) { std::ifstream in(g_jwt + "/" + f); std::stringstream ss; ss << in.rdbuf(); std::string s = ss.str(); while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s; }

struct Who { uint8_t kind; std::string uid, token, name; };
static Who local(const char* uid, const char* name) { return {LoginLocal, uid, "", name}; }
static Who fb(const char* tokFile, const char* name) { return {LoginFirebase, "", readTok(tokFile), name}; }

static uint8_t call(const Who& w, uint8_t op, TeamView& v, const std::string& text = "", const uint8_t* flag = nullptr, uint32_t since = 0, uint8_t index = 0, uint32_t amount = 0) {
    TeamRequest r; r.amount = amount; r.authKind = w.kind; r.uid = w.uid; r.token = w.token; r.playerName = w.name; r.op = op; r.text = text; r.since = since; r.index = index;
    if (flag) std::memcpy(r.flag, flag, kFlagBytes);
    uint8_t res = 0;
    if (!teamCall("127.0.0.1", g_port, r, res, v, 5000)) return 0;
    return res;
}

static void seedAccount(persist::Store& st, const std::string& key, int coins) {
    persist::Account a; a.key = key; a.verified = key.rfind("fb:", 0) == 0; a.created = (int64_t)std::time(nullptr); a.coins = coins;
    e0net::invStarterKit(a.inv);
    const std::string p = st.pathOf(key);
    persist::mkdirs(p.substr(0, p.rfind('/')));
    persist::writeAtomic(p, a.serialize(), true);
}
static int seed(const std::string& dir) {
    persist::mkdirs(dir + "/accounts");
    persist::Store st; st.open(dir);
    seedAccount(st, "anon:local-teamcapitao01", 10000);
    seedAccount(st, "anon:local-teammarujo01", 5000);
    seedAccount(st, "fb:alice", 100);
    st.close();
    teams::Registry R;
    auto t = std::make_shared<teams::Team>();
    const int64_t now = (int64_t)std::time(nullptr);
    t->id = 50; t->name = "VELHOS LOBOS"; t->code = "VELHO2"; t->ownerKey = "anon:local-teamvelho001"; t->created = now - 90000;
    t->members.push_back({"anon:local-teamvelho001", "VELHO", now - 90000});
    t->msgs.push_back({1, now - 25 * 3600, "anon:local-teamvelho001", "VELHO", "MENSAGEM DE ONTEM"});
    t->msgs.push_back({2, now - 3600, "anon:local-teamvelho001", "VELHO", "MENSAGEM DE AGORA"});
    t->nextMsg = 3;
    R.add(t);
    persist::writeAtomic(dir + "/teams.e0t", R.serialize(), true);
    std::printf("seed ok em %s\n", dir.c_str());
    return 0;
}

static const uint8_t kFlagA[kFlagBytes] = {0, 1, 7, 0, 0, 1, 1, 1, 1, 0, 0};
static const uint8_t kFlagB[kFlagBytes] = {2, 0, 0, 9, 0, 1, 7, 1, 1, 0, 0};

// DOMINAÇÃO DE ILHAS: 2 tripulações (AZUIS 60, VERMELHOS 61) de 1 visitante cada; o 3º (local-conqsolo01) fica sem tripulação
static int conqSeed(const std::string& dir) {
    persist::mkdirs(dir + "/accounts");
    teams::Registry R;
    const int64_t now = (int64_t)std::time(nullptr);
    const char* names[2] = {"AZUIS", "VERMELHOS"}; const char* keys[2] = {"anon:local-conqazul01", "anon:local-conqverm01"};
    for (int i = 0; i < 2; ++i) {
        auto t = std::make_shared<teams::Team>();
        t->id = 60 + i; t->name = names[i]; t->code = i ? "VERME2" : "AZUL22"; t->ownerKey = keys[i]; t->created = now;
        t->members.push_back({keys[i], names[i], now, ""});
        t->flag[0] = (uint8_t)(1 + i); t->flag[6] = 3;
        R.add(t);
    }
    persist::writeAtomic(dir + "/teams.e0t", R.serialize(), true);
    std::printf("conqseed ok em %s\n", dir.c_str());
    return 0;
}

static int run() {
    const Who A = local("local-teamcapitao01", "Capit\xE3o Ana");
    const Who B = fb("token_alice.txt", "Alice");
    const Who C = local("local-teammarujo01", "Marujo");
    const Who V = local("local-teamvelho001", "Velho");
    TeamView v;
    uint8_t r = call(A, TeamGet, v);
    CHECK(r == TeamOk && v.coins == 10000 && !v.license && !v.inTeam, "A sem tripulação, 10000 moedas (r %d, moedas %d)", r, v.coins);
    r = call(A, TeamCreate, v, "Os Corsarios", kFlagA);
    CHECK(r == TeamErrNoLicense && !v.inTeam, "criar sem licença é recusado (r %d)", r);
    r = call(B, TeamBuyLicense, v);
    CHECK(r == TeamErrCoins && v.coins == 100 && !v.license, "licença sem moedas suficientes é recusada (r %d, moedas %d)", r, v.coins);
    r = call(A, TeamBuyLicense, v);
    CHECK(r == TeamOk && v.license && v.coins == 10000 - kTeamLicenseCost, "A comprou a licença (moedas %d)", v.coins);
    r = call(A, TeamBuyLicense, v);
    CHECK(r == TeamErrHasLicense && v.coins == 10000 - kTeamLicenseCost, "segunda licença recusada (r %d)", r);
    r = call(A, TeamCreate, v, "ab", kFlagA);
    CHECK(r == TeamErrName && !v.inTeam && v.license, "nome curto recusado (r %d)", r);
    r = call(A, TeamCreate, v, "  Os   Cors\xE1rios  ", kFlagA);
    CHECK(r == TeamOk && v.inTeam && v.owner && !v.license && v.code.size() == (size_t)kTeamCodeLen && v.members.size() == 1 && std::memcmp(v.flag, kFlagA, kFlagBytes) == 0,
          "A criou '%s' (código %s, membros %zu, licença gasta)", v.name.c_str(), v.code.c_str(), v.members.size());
    CHECK(v.name == std::string("Os Cors\xE1rios"), "nome limpo (espaços extras saem, acento Latin-1 fica)");
    CHECK(!v.msgs.empty() && v.msgs.back().author.empty(), "mensagem do sistema da fundação (%zu msgs)", v.msgs.size());
    const std::string code = v.code;
    std::string lower = code; for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    r = call(B, TeamJoin, v, "ZZZZZZ");
    CHECK(r == TeamErrCode && !v.inTeam, "código errado recusado (r %d)", r);
    r = call(B, TeamJoin, v, lower);
    CHECK(r == TeamOk && v.inTeam && !v.owner && v.members.size() == 2, "Alice (Firebase) entrou pelo código em minúsculas (%zu membros)", v.members.size());
    r = call(B, TeamCreate, v, "Outra", kFlagB);
    CHECK(r == TeamErrInTeam, "quem já está numa tripulação não cria outra (r %d)", r);
    r = call(C, TeamJoin, v, code);
    CHECK(r == TeamOk && v.members.size() == 3, "Marujo entrou (%zu membros)", v.members.size());
    CHECK(v.members[0].owner && v.members[0].name == std::string("Capit\xE3o Ana"), "capitão primeiro na lista ('%s')", v.members[0].name.c_str());
    r = call(B, TeamChat, v, "Ol\xE1, tripula\xE7\xE3o!");
    CHECK(r == TeamOk, "Alice mandou mensagem (r %d)", r);
    r = call(B, TeamChat, v, "de novo");
    CHECK(r == TeamErrSlow, "segunda mensagem em < 1 s recusada (r %d)", r);
    r = call(B, TeamChat, v, "   ");
    CHECK(r == TeamErrText || r == TeamErrSlow, "mensagem vazia recusada (r %d)", r);
    r = call(A, TeamGet, v);
    bool found = false; for (const TeamMsg& m : v.msgs) if (m.author == "Alice" && m.text.find("tripula") != std::string::npos && !m.mine) found = true;
    CHECK(r == TeamOk && found, "A vê a mensagem da Alice (%zu msgs, última id %u)", v.msgs.size(), v.lastMsgId);
    const uint32_t last = v.lastMsgId;
    r = call(A, TeamGet, v, "", nullptr, last);
    CHECK(r == TeamOk && v.msgs.empty(), "since = última: nenhuma mensagem nova (%zu)", v.msgs.size());
    r = call(B, TeamRename, v, "Piratas");
    CHECK(r == TeamErrNotOwner, "só o capitão renomeia (r %d)", r);
    r = call(A, TeamRename, v, "Corsarios do Sul");
    CHECK(r == TeamOk && v.name == "Corsarios do Sul", "renomeou para '%s'", v.name.c_str());
    r = call(B, TeamSetFlag, v, "", kFlagB);
    CHECK(r == TeamErrNotOwner, "só o capitão troca a bandeira (r %d)", r);
    r = call(A, TeamSetFlag, v, "", kFlagB);
    r = call(C, TeamGet, v);
    CHECK(r == TeamOk && std::memcmp(v.flag, kFlagB, kFlagBytes) == 0, "Marujo vê a bandeira nova");
    // nome repetido (sem diferenciar maiúsculas)
    TeamView vc; call(C, TeamLeave, vc);
    CHECK(!vc.inTeam, "Marujo saiu");
    r = call(C, TeamBuyLicense, vc);
    CHECK(r == TeamOk && vc.license, "Marujo comprou a licença (moedas %d)", vc.coins);
    r = call(C, TeamCreate, vc, "CORSARIOS DO SUL", kFlagA);
    CHECK(r == TeamErrNameTaken && vc.license && !vc.inTeam, "nome já usado recusado (r %d)", r);
    r = call(C, TeamJoin, vc, code);
    CHECK(r == TeamOk && vc.members.size() == 3, "Marujo voltou (%zu)", vc.members.size());
    // expulsar: o índice é o da lista enviada (capitão primeiro)
    int idxC = -1; r = call(A, TeamGet, v); for (size_t i = 0; i < v.members.size(); ++i) if (v.members[i].name == "Marujo") idxC = (int)i;
    r = call(B, TeamKick, v, "", nullptr, 0, (uint8_t)idxC);
    CHECK(r == TeamErrNotOwner, "só o capitão expulsa (r %d)", r);
    r = call(A, TeamKick, v, "", nullptr, 0, 0);
    CHECK(r == TeamErrBadReq && v.members.size() == 3, "capitão não se expulsa (r %d)", r);
    r = call(A, TeamKick, v, "", nullptr, 0, (uint8_t)idxC);
    CHECK(r == TeamOk && v.members.size() == 2, "Marujo expulso (%zu membros)", v.members.size());
    r = call(C, TeamGet, vc);
    CHECK(r == TeamOk && !vc.inTeam && vc.license, "Marujo fora da tripulação, licença ainda guardada");
    // COFRE e LOJA da tripulação (A = capitão com 7000 moedas, B = Alice com 100)
    r = call(B, TeamDeposit, v, "", nullptr, 0, 0, 50);
    CHECK(r == TeamOk && v.treasury == 50 && v.coins == 50, "Alice depositou 50 (cofre %d, saldo %d)", v.treasury, v.coins);
    r = call(B, TeamDeposit, v, "", nullptr, 0, 0, 500);
    CHECK(r == TeamErrCoins && v.treasury == 50, "depositar mais do que tem recusado (r %d)", r);
    r = call(B, TeamDeposit, v, "", nullptr, 0, 0, 0);
    CHECK(r == TeamErrAmount, "depositar 0 recusado (r %d)", r);
    r = call(A, TeamDeposit, v, "", nullptr, 0, 0, 3000);
    CHECK(r == TeamOk && v.treasury == 3050 && v.coins == 4000, "capitão depositou 3000 (cofre %d, saldo %d)", v.treasury, v.coins);
    r = call(B, TeamWithdraw, v, "", nullptr, 0, 0, 10);
    CHECK(r == TeamErrNotOwner, "só o capitão saca (r %d)", r);
    r = call(A, TeamWithdraw, v, "", nullptr, 0, 0, 50);
    CHECK(r == TeamOk && v.treasury == 3000 && v.coins == 4050, "capitão sacou 50 (cofre %d, saldo %d)", v.treasury, v.coins);
    r = call(A, TeamWithdraw, v, "", nullptr, 0, 0, 999999);
    CHECK(r == TeamErrTreasury && v.treasury == 3000, "sacar mais do que há no cofre recusado (r %d)", r);
    r = call(B, TeamBuy, v, "", nullptr, 0, 0);
    CHECK(r == TeamErrNotOwner, "só o capitão compra na loja (r %d)", r);
    r = call(A, TeamBuy, v, "", nullptr, 0, 0);
    CHECK(r == TeamOk && v.slotLevel == 1 && v.capacity() == 15 && v.treasury == 500, "comprou +5 vagas (capacidade %d, cofre %d)", v.capacity(), v.treasury);
    r = call(A, TeamBuy, v, "", nullptr, 0, 3);
    CHECK(r == TeamErrTreasury && !(v.ships & 4), "chalupa sem moedas no cofre recusada (r %d)", r);
    r = call(A, TeamDeposit, v, "", nullptr, 0, 0, 3000);
    r = call(A, TeamBuy, v, "", nullptr, 0, 3);
    CHECK(r == TeamOk && (v.ships & 4) && v.treasury == 3500 - kTeamShipPrice[2], "comprou a chalupa (navios %d, cofre %d)", v.ships, v.treasury);
    r = call(A, TeamBuy, v, "", nullptr, 0, 3);
    CHECK(r == TeamErrOwned, "navio repetido recusado (r %d)", r);
    r = call(A, TeamBuy, v, "", nullptr, 0, 9);
    CHECK(r == TeamErrBadReq, "item inexistente recusado (r %d)", r);
    // o capitão passa o cargo para Alice e sai (como membro comum: a tripulação continua)
    r = call(A, TeamPromote, v, "Alice", nullptr, 0, 1);
    CHECK(r == TeamOk && !v.owner, "capitão passou o cargo para Alice");
    r = call(A, TeamLeave, v);
    CHECK(r == TeamOk && !v.inTeam && v.coins == 10000 - kTeamLicenseCost - 6000 + 50, "ex-capitão saiu (moedas %d)", v.coins);
    r = call(B, TeamGet, v);
    CHECK(r == TeamOk && v.inTeam && v.owner && v.members.size() == 1, "Alice é a capitã e a tripulação continua");
    // CAPITÃO ABANDONA = tripulação DESFEITA: o Marujo funda outra, o capitão antigo entra, o Marujo sai -> acaba para todos
    r = call(C, TeamCreate, vc, "Navio Fantasma", kFlagA);
    const std::string code2 = vc.code;
    CHECK(r == TeamOk && vc.owner, "Marujo fundou 'Navio Fantasma'");
    r = call(A, TeamJoin, v, code2);
    CHECK(r == TeamOk && v.members.size() == 2, "capitão antigo entrou no Navio Fantasma");
    r = call(C, TeamLeave, vc);
    CHECK(r == TeamOk && !vc.inTeam, "o capitão do Navio Fantasma abandonou");
    r = call(A, TeamGet, v);
    CHECK(r == TeamOk && !v.inTeam, "TRIPULAÇÃO DESFEITA: o outro membro ficou sem tripulação");
    r = call(V, TeamJoin, v, code2);
    CHECK(r == TeamErrInTeam || r == TeamErrCode, "o código da tripulação desfeita não vale mais (r %d)", r);
    // erros de autenticação
    Who bad = local("xx", "x");
    r = call(bad, TeamGet, v);
    CHECK(r == TeamErrAuth, "uid inválido recusado (r %d)", r);
    Who exp = fb("token_alice_expirado.txt", "Alice");
    r = call(exp, TeamGet, v);
    CHECK(r == TeamErrExpired, "token expirado -> TeamErrExpired (r %d)", r);
    Who aud = fb("token_alice_outroaud.txt", "Alice");
    r = call(aud, TeamGet, v);
    CHECK(r == TeamErrAuth, "token de outro projeto recusado (r %d)", r);
    // mensagens de mais de 24 h somem
    r = call(V, TeamGet, v);
    bool old = false, recent = false; for (const TeamMsg& m : v.msgs) { if (m.text == "MENSAGEM DE ONTEM") old = true; if (m.text == "MENSAGEM DE AGORA") recent = true; }
    CHECK(r == TeamOk && v.inTeam && !old && recent, "mensagem de 25 h sumiu, a de 1 h ficou (%zu msgs)", v.msgs.size());
    // pedido malformado não derruba nada
    { TeamView x; TeamRequest q; q.authKind = 9; uint8_t rr = 0; const bool ok = teamCall("127.0.0.1", g_port, q, rr, x, 3000); CHECK(ok && rr == TeamErrAuth, "tipo de login inválido (r %d)", rr); }
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}
static int after() {
    TeamView v;
    uint8_t r = call(fb("token_alice.txt", "Alice"), TeamGet, v);
    bool msg = false; for (const TeamMsg& m : v.msgs) if (m.author == "Alice") msg = true;
    CHECK(r == TeamOk && v.inTeam && v.owner && v.name == "Corsarios do Sul" && msg && std::memcmp(v.flag, kFlagB, kFlagBytes) == 0, "depois de reiniciar: tripulação, capitã, nome, bandeira e chat continuam");
    CHECK(r == TeamOk && v.treasury == 3500 - kTeamShipPrice[2] && v.slotLevel == 1 && (v.ships & 4), "depois de reiniciar: cofre %d, vagas nivel %d e chalupa continuam", v.treasury, v.slotLevel);
    r = call(local("local-teamcapitao01", "Ana"), TeamGet, v);
    CHECK(r == TeamOk && !v.inTeam && !v.license && v.coins == 10000 - kTeamLicenseCost - 6000 + 50, "capitão antigo: fora, sem licença, moedas %d", v.coins);
    r = call(local("local-teammarujo01", "Marujo"), TeamGet, v);
    CHECK(r == TeamOk && !v.inTeam && !v.license && v.coins == 5000 - kTeamLicenseCost, "Marujo: sem tripulação (a dele foi desfeita), licença gasta (moedas %d)", v.coins);
    std::printf("\n%s (%d falha(s))\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "seed") == 0) return seed(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "conqseed") == 0) return conqSeed(argv[2]);
    if (argc >= 6 && std::strcmp(argv[1], "say") == 0) {   // teamtest say PORTA UID_LOCAL NOME "texto": manda uma mensagem no chat da tripulação (testes no aparelho)
        g_port = (uint16_t)std::atoi(argv[2]); TeamView v;
        const uint8_t r = call(local(argv[3], argv[4]), TeamChat, v, argv[5]);
        std::printf("say: resultado %d (%s)\n", r, teamResultText(r)); return r == TeamOk ? 0 : 1;
    }
    if (argc >= 5 && std::strcmp(argv[1], "grant") == 0) {   // teamtest grant DIR_DADOS CHAVE MOEDAS (servidor PARADO): conta de REVISÃO/teste com tudo
        persist::Store st; if (!st.open(argv[2])) return 1;   // liberado: moedas, os 4 navios, licença de tripulação, ferramentas, arma, munição e poções
        persist::Account a; bool bad = false;
        if (!st.loadNow(argv[3], a, &bad)) { std::fprintf(stderr, "conta %s nao encontrada\n", argv[3]); st.close(); return 1; }
        using namespace item;
        a.coins = std::atoi(argv[4]); a.ships = 0x0F; a.teamLicense = 1; a.health = 100; a.hunger = 100;
        invClear(a.inv);   // ~29 kg de 40: ainda dá para coletar
        const uint8_t hot[5] = {Dagger, Hatchet, Pickaxe, Torch, Gun};
        for (int i = 0; i < 5; ++i) { a.inv[kInvHotFirst + i].id = hot[i]; a.inv[kInvHotFirst + i].n = 1; }
        const InvSlot bag[7] = {{Ammo, 30}, {PotHeal, 10}, {PotFull, 10}, {PotSpeed, 10}, {PotShield, 10}, {Coconut, 10}, {Rope, 10}};
        for (int i = 0; i < 7; ++i) a.inv[i] = bag[i];
        invSanitize(a.inv);
        persist::writeAtomic(st.pathOf(argv[3]), a.serialize(), true); st.close();
        std::printf("%s: %d moedas, navios 0x%X, licenca %d, %.1f kg\n", argv[3], (int)a.coins, a.ships, a.teamLicense, invWeight(a.inv)); return 0;
    }
    if (argc >= 5 && std::strcmp(argv[1], "coins") == 0) {   // teamtest coins DIR_DADOS CHAVE N (servidor PARADO): põe N moedas na conta (testes no aparelho)
        persist::Store st; if (!st.open(argv[2])) return 1;
        persist::Account a; bool bad = false;
        if (!st.loadNow(argv[3], a, &bad)) { std::fprintf(stderr, "conta %s nao encontrada\n", argv[3]); st.close(); return 1; }
        a.coins = std::atoi(argv[4]); persist::writeAtomic(st.pathOf(argv[3]), a.serialize(), true); st.close();
        std::printf("%s: %d moedas\n", argv[3], (int)a.coins); return 0;
    }
    if (argc >= 4) {
        g_port = (uint16_t)std::atoi(argv[2]); g_jwt = argv[3];
        if (std::strcmp(argv[1], "run") == 0) return run();
        if (std::strcmp(argv[1], "after") == 0) return after();
    }
    std::fprintf(stderr, "uso: teamtest seed DIR | teamtest run PORTA DIR_JWT | teamtest after PORTA DIR_JWT\n");
    return 2;
}
