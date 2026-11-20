// Testes do baú dos navios (shared/chest.h + chestAct): build/chesttest
#include <cstdio>
#include "../shared/protocol.h"
using namespace e0net;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    Chest c;
    CHECK(chestPut(c, 1, 30) == 30 && c.s[0].id == 1 && c.s[0].n == 30, "guarda 30 gravetos");
    CHECK(chestPut(c, 1, 30) == 30 && c.s[0].n == 50 && c.s[1].n == 10, "completa a pilha e abre outra (%d, %d)", c.s[0].n, c.s[1].n);
    CHECK(chestPut(c, 7, 3) == 3 && chestCount(c) == 5, "ferramenta de pilha 1 ocupa 1 slot cada (%d slots)", chestCount(c));
    int total = 0; Chest f; for (int i = 0; i < 100; ++i) total += chestPut(f, 7, 1);
    CHECK(total == kChestSlots, "cheio aceita so %d (%d)", kChestSlots, total);
    CHECK(chestPut(f, 1, 1) == 0 && chestPut(c, 0, 5) == 0 && chestPut(c, 99, 5) == 0 && chestPut(c, 2, 0) == 0, "recusa cheio, id invalido e zero");
    uint8_t id = 0;
    CHECK(chestTake(c, 0, 20, id) == 20 && id == 1 && c.s[0].n == 30, "tira 20");
    CHECK(chestTake(c, 1, 99, id) == 10 && !c.s[1].id, "tira tudo o que ha e esvazia o slot");
    CHECK(chestTake(c, 1, 1, id) == 0 && chestTake(c, -1, 1, id) == 0 && chestTake(c, 24, 1, id) == 0, "slot vazio e indices invalidos");
    uint16_t v = c.ver; chestPut(c, 3, 1); CHECK((uint16_t)(c.ver - v) == 1, "versao sobe a cada mudanca");
    v = c.ver; chestPut(f, 1, 1); chestTake(c, 5, 1, id); CHECK(c.ver == v, "recusa nao muda a versao");

    // regras de alcance e de ação: o navio 0 existe; jogador a bordo perto do baú
    ShipState& S = shipState(0); S = ShipState{}; S.exists = true; S.x = 100; S.z = 100;
    const ShipDef& D = shipDef(0);
    Chest chests[kMaxShips];
    PlayerState p; p.id = 1; p.ship = 1; p.shipIdx = 0; p.sa = D.chestA + D.chestHalfA + 0.5f; p.sb = D.chestB; p.y = D.deckY;
    uint8_t item; uint16_t n;
    CHECK(chestAct(chests, p, ActChestPut, (uint16_t)(2 | (0 << 8)), 7, item, n) == 1 && n == 7 && item == 2, "guarda perto do bau (n=%u)", n);
    CHECK(chestAct(chests, p, ActChestTake, (uint16_t)(0 | (0 << 8)), 5, item, n) == 1 && n == 5 && item == 2, "pega do slot 0");
    CHECK(chests[0].s[0].n == 2, "sobraram 2");
    PlayerState far = p; far.sa = -2.0f;
    CHECK(chestAct(chests, far, ActChestTake, 0, 1, item, n) == 0, "longe do bau: recusa");
    PlayerState up = p; up.y = D.terraceY;
    CHECK(chestAct(chests, up, ActChestPut, 2, 1, item, n) == 0, "no toldo (outro nivel): recusa");
    PlayerState out = p; out.ship = 0;
    CHECK(chestAct(chests, out, ActChestPut, 2, 1, item, n) == 0, "fora do navio: recusa");
    PlayerState other = p; other.shipIdx = 1;
    CHECK(chestAct(chests, other, ActChestPut, 2, 1, item, n) == 0, "navio 1 nao existe: recusa");
    CHECK(chestAct(chests, p, ActChestPut, (uint16_t)(2 | (7 << 8)), 1, item, n) == 0, "navio fora do intervalo: recusa");
    CHECK(chestAct(chests, p, ActChestPut, (uint16_t)(2 | (1 << 8)), 1, item, n) == 0, "baú de OUTRO navio: recusa");
    CHECK(chestAct(chests, p, ActChestPut, 200, 1, item, n) == 0, "item inexistente: recusa");
    // cada navio tem o seu baú
    ShipState& G = shipState(1); G = ShipState{}; G.exists = true;
    PlayerState g = p; g.shipIdx = 1; const ShipDef& DG = shipDef(1); g.sa = DG.chestA + DG.chestHalfA + 0.5f; g.sb = DG.chestB; g.y = DG.deckY;
    CHECK(chestAct(chests, g, ActChestPut, (uint16_t)(4 | (1 << 8)), 3, item, n) == 1 && chests[1].s[0].id == 4 && chests[0].s[0].id == 2, "baus separados por navio");
    // serialização
    uint8_t buf[128]; Writer w(buf, sizeof buf); w.chest(1, chests[1]);
    Reader r(buf, w.size()); Chest back; int sh = r.chest(back);
    CHECK(r.ok() && sh == 1 && back.ver == chests[1].ver && back.s[0].id == 4 && back.s[0].n == 3 && w.size() < 100, "serializacao (%zu B)", w.size());
    { uint8_t bad[128]; Writer wb(bad, sizeof bad); Chest x; x.s[0] = {200, 9}; x.s[1] = {1, 999}; wb.chest(0, x); Reader rb(bad, wb.size()); Chest y; rb.chest(y); CHECK(!y.s[0].id && !y.s[1].id, "descarta item invalido e pilha acima do maximo"); }
    std::printf(fails ? "%d FALHAS\n" : "TODOS OS TESTES DO BAU PASSARAM\n", fails);
    return fails ? 1 : 0;
}
