// Testes do navio (shared/ship.h + stepPlayer a bordo). Uso: g++ ... tools/shiptest.cpp shared/world.cpp -o build/shiptest && build/shiptest
#include <cstdio>
#include "../shared/protocol.h"
using namespace e0net;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FALHOU: " __VA_ARGS__); std::printf("\n"); ++fails; } } while (0)

static void run(PlayerState& p, InputCmd c, float sec) {
    c.dt = 1.0f / 30.0f;
    for (float t = 0; t < sec; t += c.dt) {
        for (int si = 0; si < kMaxShips; ++si) stepShip(shipState(si), c.dt, terrainPtr());
        if (p.ship) shipCarry(p);
        stepPlayer(p, c);
        c.shipEv &= (uint8_t)~ShipEvEvents;   // eventos valem 1 frame
    }
}

// anda (no espaço do navio) até (a, b) com o joystick por no máx. `sec` s; para quando chega perto
static void walkTo(PlayerState& p, float a, float b, float sec) {
    for (float t = 0; t < sec; t += 1.0f / 30.0f) {
        float da = a - p.sa, db = b - p.sb, d = std::hypot(da, db);
        if (d < 0.25f) return;
        InputCmd m; m.moveX = db / d; m.moveY = da / d; m.camYaw = shipOf(p).yaw;   // camYaw = proa: moveY = frente, moveX = direita
        run(p, m, 1.0f / 30.0f);
    }
}

int main() {
    World W;
    if (!World::generate(kDefaultWorldSeed, W)) { std::printf("sem mundo\n"); return 1; }
    setTerrain(&W);
    ShipState& S = shipState(0);
    float x, z, yaw;
    CHECK(shipFindSpawn(0, W, W.spawnX(), W.spawnZ(), 14.0f, x, z, yaw), "sem agua funda para o navio");
    S = ShipState{}; S.exists = true; S.x = x; S.z = z; S.yaw = yaw;
    CHECK(!shipHullHitsLand(kShipDefs[0], terrainPtr(), S.x, S.z, S.yaw), "navio nasceu em terra");
    std::printf("navio em (%.0f, %.0f) yaw %.2f; spawn do jogador (%.0f, %.0f) dist %.0f m\n", x, z, yaw, W.spawnX(), W.spawnZ(), std::hypot(x - W.spawnX(), z - W.spawnZ()));

    // jogador na água ao lado do casco, embarca
    PlayerState p; p.id = 0;
    float wx, wz; shipLocalToWorld(S, 0.0f, 5.5f, wx, wz);
    p.x = wx; p.z = wz; p.y = World::kSwimFloorH;
    InputCmd c; c.shipEv = ShipEvBoard;
    run(p, c, 0.1f);
    CHECK(p.ship == 1, "nao embarcou (ship=%d)", p.ship);
    CHECK(std::fabs(p.y - kShipDeckY) < 0.01f, "y no convés %.2f", p.y);
    CHECK(std::fabs(supportHeightAt(p.x, p.z, p.y) - kShipDeckY) < 0.01f, "supportHeightAt no convés");

    // anda para todos os lados por 20 s: nunca sai do convés nem entra nos mastros
    uint32_t rs = 12345; float maxOut = 0;
    for (int i = 0; i < 40; ++i) {
        rs = rs * 1664525u + 1013904223u; float ang = (float)(rs >> 8) / 16777216.0f * 6.2831853f;
        InputCmd m; m.moveX = std::sin(ang); m.moveY = std::cos(ang); m.camYaw = 0.0f; m.flags = (i & 1) ? InputRun : 0;
        run(p, m, 0.5f);
        if (shipFloor(kShipDefs[0], p.sa, p.sb) < kNoFloor * 0.5f) maxOut = 1.0f;
        CHECK(std::fabs(p.y - shipFloor(kShipDefs[0], p.sa, p.sb)) < 0.05f || p.vy != 0.0f, "y %.2f fora do chao %.2f", p.y, shipFloor(kShipDefs[0], p.sa, p.sb));
        for (float mast : {-5.3f, 1.25f}) { float d = std::hypot(p.sa - mast, p.sb + 0.1f); CHECK(d >= 0.6f, "dentro do mastro d=%.2f", d); }
        CHECK(shipChestDist(kShipDefs[0], p.sa, p.sb) >= 0.3f || p.y > kShipDeckY + 1.2f, "dentro do bau (a=%.2f b=%.2f)", p.sa, p.sb);
    }
    CHECK(maxOut < 0.01f, "saiu do chao do navio");

    // sem escada não se chega ao toldo: andar reto para a popa (miolo) fica barrado; pelas escadas laterais chega ao leme
    p.sa = -2.0f; p.sb = 0.3f; p.y = kShipDeckY; p.vy = 0; shipCarry(p);
    walkTo(p, kHelmA, 0.0f, 6.0f);
    CHECK(p.sa >= kDeckAMin - 0.01f && p.y < kShipDeckY + 0.1f, "parede da popa barra o caminho direto (a=%.2f y=%.2f)", p.sa, p.y);
    // a colisão da escada acompanha os degraus do modelo: nada de rampa invisível antes do 1º degrau (a = -7,8)
    CHECK(std::fabs(shipFloor(kShipDefs[0], -6.5f, 2.4f) - kShipDeckY) < 0.01f, "chao plano antes da escada (y=%.2f)", shipFloor(kShipDefs[0], -6.5f, 2.4f));
    {
        const float kStep[5][2] = {{-8.0f, 1.41f}, {-8.4f, 1.80f}, {-8.8f, 2.20f}, {-9.2f, 2.60f}, {-9.65f, 3.0f}};   // degraus medidos na malha
        for (const auto& st : kStep) for (float sb : {2.4f, -2.6f}) CHECK(std::fabs(shipFloor(kShipDefs[0], st[0], sb) - st[1]) < 0.08f, "degrau a=%.2f: %.2f, malha %.2f", st[0], shipFloor(kShipDefs[0], st[0], sb), st[1]);
    }
    // o vão entre as escadas agora é chão, o baú é sólido e dá para chegar perto dele pelos 2 lados
    walkTo(p, -6.0f, 1.4f, 3.0f); walkTo(p, -9.0f, 1.4f, 3.0f);
    CHECK(std::fabs(p.sa - kDeckWallA) < 0.12f && std::fabs(p.y - kShipDeckY) < 0.02f, "anda ate a parede entre as escadas (a=%.2f y=%.2f)", p.sa, p.y);
    CHECK(shipChestDist(kShipDefs[0], p.sa, p.sb) <= kChestReach, "alcanca o bau pelo lado (d=%.2f)", shipChestDist(kShipDefs[0], p.sa, p.sb));
    walkTo(p, -5.9f, -0.9f, 3.0f); walkTo(p, kChestA, kChestB, 3.0f);
    CHECK(shipChestDist(kShipDefs[0], p.sa, p.sb) >= 0.3f && shipChestDist(kShipDefs[0], p.sa, p.sb) <= kChestReach, "bau barra quem anda de frente (d=%.2f a=%.2f)", shipChestDist(kShipDefs[0], p.sa, p.sb), p.sa);
    { float a = kChestA, b = kChestB; shipClampDeck(kShipDefs[0], a, b); CHECK(shipChestDist(kShipDefs[0], a, b) >= 0.3f && shipFloor(kShipDefs[0], a, b) == kShipDeckY, "embarcar em cima do bau cai no conves (a=%.2f b=%.2f)", a, b); }
    walkTo(p, -4.5f, 2.5f, 6.0f); walkTo(p, -10.3f, 2.5f, 8.0f);
    CHECK(std::fabs(p.y - kTerraceY) < 0.08f, "subiu a escada ate o toldo (y=%.2f a=%.2f b=%.2f)", p.y, p.sa, p.sb);
    walkTo(p, -10.4f, 0.5f, 4.0f); walkTo(p, kHelmA, 0.0f, 4.0f);
    CHECK(std::hypot(p.sa - kHelmA, p.sb) < 0.6f, "chegou ao leme (a=%.2f b=%.2f)", p.sa, p.sb);
    // descer: de volta pela escada
    walkTo(p, -10.4f, 2.5f, 4.0f); walkTo(p, -4.5f, 2.5f, 8.0f);
    CHECK(std::fabs(p.y - kShipDeckY) < 0.08f, "desceu a escada (y=%.2f)", p.y);
    // o casco e solido para quem nada: um nadador no meio do casco e empurrado para fora e nao atravessa andando por baixo
    {
        PlayerState sw; sw.id = 3; float hx, hz; shipLocalToWorld(S, 0.0f, 0.5f, hx, hz); sw.x = hx; sw.z = hz; sw.y = World::kSwimFloorH;
        InputCmd m; run(sw, m, 0.2f);
        float la, lb; shipWorldToLocal(S, sw.x, sw.z, la, lb);
        CHECK(std::fabs(lb) >= shipHullHalfB(kShipDefs[0], la) + kPlayerRadius - 0.05f || la < -16.0f || la > 13.5f, "nadador dentro do casco foi empurrado para fora (a=%.1f b=%.1f)", la, lb);
        shipLocalToWorld(S, 0.0f, 7.0f, sw.x, sw.z);
        float worst = 99.0f;
        for (int i = 0; i < 120; ++i) {   // nada direto em direcao ao centro do casco por 4 s
            float la2, lb2; shipWorldToLocal(S, sw.x, sw.z, la2, lb2);
            InputCmd mm; float dx = hx - sw.x, dz = hz - sw.z, n = std::max(0.01f, std::hypot(dx, dz)); mm.moveX = dx / n; mm.moveY = -dz / n; mm.camYaw = 0; mm.flags = InputRun;
            run(sw, mm, 1.0f / 30.0f);
            shipWorldToLocal(S, sw.x, sw.z, la2, lb2);
            worst = std::min(worst, std::fabs(lb2) - shipHullHalfB(kShipDefs[0], std::max(-15.9f, std::min(13.3f, la2))));
        }
        CHECK(worst > 0.2f, "nadador nao atravessa o casco (folga minima %.2f m)", worst);
    }

    // vai ao leme e assume
    walkTo(p, -4.5f, 2.5f, 3.0f); walkTo(p, -9.9f, 2.5f, 8.0f); walkTo(p, -10.4f, 0.5f, 4.0f); walkTo(p, kHelmA, 0.0f, 4.0f);
    c = InputCmd{}; c.shipEv = ShipEvHelm; run(p, c, 0.1f);
    CHECK(p.ship == 2 && S.helmer == 0, "nao assumiu o leme (ship=%d helmer=%d)", p.ship, S.helmer);
    PlayerState q; q.id = 1; q.ship = 1; q.sa = kHelmA + 1.0f; q.sb = 0.0f; q.y = kTerraceY; shipCarry(q);
    c = InputCmd{}; c.shipEv = ShipEvHelm; run(q, c, 0.1f);
    CHECK(q.ship == 1, "segundo jogador pegou o leme ocupado");

    // acelera reto 8 s
    float sx0 = S.x, sz0 = S.z;
    c = InputCmd{}; c.shipEv = (uint8_t)(1u << ShipThrShift);
    run(p, c, 8.0f);
    CHECK(S.speed > 6.0f, "velocidade %.2f apos 8 s", S.speed);
    float moved = std::hypot(S.x - sx0, S.z - sz0);
    CHECK(moved > 20.0f, "andou %.1f m", moved);
    float la, lb; shipWorldToLocal(S, p.x, p.z, la, lb);
    CHECK(std::fabs(la - kHelmA) < 0.01f && std::fabs(lb) < 0.01f, "timoneiro saiu do leme (%.2f,%.2f)", la, lb);
    // vira para estibordo (direita): o yaw cresce
    float yaw0 = S.yaw;
    c.moveX = 1.0f; run(p, c, 4.0f);
    CHECK(shipWrap(S.yaw - yaw0) > 0.3f, "nao virou a estibordo (%.2f)", shipWrap(S.yaw - yaw0));
    // re
    c = InputCmd{}; c.shipEv = (uint8_t)(2u << ShipThrShift); run(p, c, 15.0f);
    CHECK(S.speed < -1.0f && S.speed >= -kShipDefs[0].maxRev - 0.01f, "re %.2f", S.speed);
    // solto: para sozinho
    c = InputCmd{}; run(p, c, 60.0f);
    CHECK(std::fabs(S.speed) < 0.01f, "nao parou sozinho %.3f", S.speed);

    // acelera direto na ilha: encalha antes de entrar em terra
    {   // proa para a ilha (se o casco couber nessa direção)
        float want = std::atan2(W.spawnX() - S.x, -(W.spawnZ() - S.z));
        for (int k = 0; k < 16; ++k) { float y2 = want + k * 0.39f; if (!shipHullHitsLand(kShipDefs[0], terrainPtr(), S.x, S.z, y2)) { S.yaw = y2; break; } }
        S.speed = 0;
    }
    c = InputCmd{}; c.shipEv = (uint8_t)(1u << ShipThrShift);
    bool landed = false;
    for (int i = 0; i < 30 * 120 && !landed; ++i) { run(p, c, 1.0f / 30.0f); landed = shipHullHitsLand(kShipDefs[0], terrainPtr(), S.x, S.z, S.yaw); }
    CHECK(!landed, "casco entrou em terra");
    CHECK(S.speed == 0.0f, "nao encalhou (vel %.2f)", S.speed);
    std::printf("encalhou a %.0f m do spawn\n", std::hypot(S.x - W.spawnX(), S.z - W.spawnZ()));

    // desce
    c = InputCmd{}; c.shipEv = ShipEvLeave; run(p, c, 0.1f);
    CHECK(p.ship == 0 && S.helmer == -1, "nao desembarcou (ship=%d helmer=%d)", p.ship, S.helmer);
    CHECK(S.cmdThrottle == 0, "acelerador ficou ligado");

    // serialização
    uint8_t buf[256]; Writer w(buf, sizeof buf); PlayerState a; a.id = 3; a.ship = 2; a.sa = 1.5f; a.sb = -2.5f; w.state(a); w.ship(S);
    Reader r(buf, w.size()); PlayerState b = r.state(); ShipState t = r.ship();
    CHECK(r.ok() && b.ship == 2 && b.sa == 1.5f && b.sb == -2.5f && t.exists && t.x == S.x && t.helmer == -1, "serializacao");

    // ---- GALEÃO: os mesmos sistemas no segundo navio, com os 2 no mar ----
    {
        const ShipDef& G = kShipDefs[ShipGalleon];
        ShipState& N = shipState(0); ShipState& L = shipState(ShipGalleon);
        float gx, gz, gy;
        CHECK(shipFindSpawn(ShipGalleon, W, W.spawnX(), W.spawnZ(), 14.0f, gx, gz, gy), "sem agua funda para o galeao");
        L = ShipState{}; L.kind = ShipGalleon; L.exists = true; L.x = gx; L.z = gz; L.yaw = gy;
        CHECK(!shipHullHitsLand(G, terrainPtr(), L.x, L.z, L.yaw) && !shipHitsShip(L, L.x, L.z, L.yaw), "galeao nasceu em terra ou em cima do navio");
        CHECK(std::hypot(L.x - N.x, L.z - N.z) >= 75.0f, "galeao longe do navio (%.0f m)", std::hypot(L.x - N.x, L.z - N.z));
        CHECK(G.hull[8][0] - G.hull[0][0] > 45.0f, "galeao maior que o navio");
        // degraus medidos na malha do galeão
        const float kStep[5][2] = {{-16.2f, 1.83f}, {-16.75f, 2.34f}, {-17.3f, 2.86f}, {-17.85f, 3.37f}, {-18.4f, 3.89f}};
        for (const auto& st : kStep) for (float sb : {3.2f, -3.4f}) CHECK(std::fabs(shipFloor(G, st[0], sb) - st[1]) < 0.1f, "degrau galeao a=%.2f: %.2f, malha %.2f", st[0], shipFloor(G, st[0], sb), st[1]);
        CHECK(std::fabs(shipFloor(G, -14.0f, 3.2f) - G.deckY) < 0.01f, "chao plano antes da escada do galeao");
        // embarca pelo costado: entra no galeão, não no navio
        PlayerState g; g.id = 2; shipLocalToWorld(L, 0.0f, G.hullHalfB + 2.0f, g.x, g.z); g.y = World::kSwimFloorH;
        InputCmd e; e.shipEv = ShipEvBoard; run(g, e, 0.1f);
        CHECK(g.ship == 1 && g.shipIdx == ShipGalleon && std::fabs(g.y - G.deckY) < 0.01f, "embarcou no galeao (ship=%d idx=%d y=%.2f)", g.ship, g.shipIdx, g.y);
        uint32_t rs2 = 777;
        for (int i = 0; i < 60; ++i) {
            rs2 = rs2 * 1664525u + 1013904223u; float ang = (float)(rs2 >> 8) / 16777216.0f * 6.2831853f;
            InputCmd m; m.moveX = std::sin(ang); m.moveY = std::cos(ang); m.camYaw = 0.0f; m.flags = (i & 1) ? InputRun : 0;
            run(g, m, 0.5f);
            CHECK(shipFloor(G, g.sa, g.sb) > kNoFloor * 0.5f, "saiu do chao do galeao");
            CHECK(std::fabs(g.y - shipFloor(G, g.sa, g.sb)) < 0.05f || g.vy != 0.0f, "y %.2f fora do chao do galeao %.2f", g.y, shipFloor(G, g.sa, g.sb));
            for (int k = 0; k < G.nMasts; ++k) CHECK(std::hypot(g.sa - G.masts[k][0], g.sb - G.masts[k][1]) >= G.masts[k][2] - 0.02f, "dentro do mastro %d", k);
            CHECK(shipChestDist(G, g.sa, g.sb) >= 0.3f || g.y > G.deckY + 1.2f, "dentro do bau do galeao");
        }
        // baú: alcançável pela frente
        walkTo(g, -9.0f, -0.135f, 6.0f); walkTo(g, G.chestA, G.chestB, 6.0f);
        CHECK(shipChestDist(G, g.sa, g.sb) >= 0.3f && shipChestDist(G, g.sa, g.sb) <= G.chestReach, "alcanca o bau do galeao (d=%.2f)", shipChestDist(G, g.sa, g.sb));
        // sobe a escada, vai ao leme, assume; só o galeão anda
        walkTo(g, -12.0f, 3.2f, 6.0f); walkTo(g, -19.4f, 3.2f, 8.0f);
        CHECK(std::fabs(g.y - G.terraceY) < 0.08f, "subiu a escada do galeao (y=%.2f a=%.2f b=%.2f)", g.y, g.sa, g.sb);
        walkTo(g, -19.5f, 0.5f, 4.0f); walkTo(g, G.helmA, 0.0f, 4.0f);
        InputCmd hc; hc.shipEv = ShipEvHelm; run(g, hc, 0.1f);
        CHECK(g.ship == 2 && L.helmer == 2 && N.helmer == -1, "assumiu o leme do galeao (ship=%d helmer=%d navio=%d)", g.ship, L.helmer, N.helmer);
        float nx0 = N.x, lx0 = L.x, lz0 = L.z;
        hc = InputCmd{}; hc.shipEv = (uint8_t)(1u << ShipThrShift); run(g, hc, 8.0f);
        CHECK(L.speed > 5.0f && L.speed <= G.maxFwd + 0.01f, "galeao acelerou (%.2f m/s)", L.speed);
        CHECK(std::hypot(L.x - lx0, L.z - lz0) > 15.0f && N.x == nx0, "so o galeao andou");
        float la2, lb2; shipWorldToLocal(L, g.x, g.z, la2, lb2);
        CHECK(std::fabs(la2 - G.helmA) < 0.01f, "timoneiro vai junto com o galeao");
        hc = InputCmd{}; hc.shipEv = ShipEvHelm; run(g, hc, 0.1f);
        CHECK(g.ship == 1 && L.helmer == -1, "largou o leme do galeao");
        // nadador contra o casco do galeão
        PlayerState sw; sw.id = 3; shipLocalToWorld(L, 0.0f, 1.0f, sw.x, sw.z); sw.y = World::kSwimFloorH; { InputCmd m; run(sw, m, 0.2f); }
        float wa, wb; shipWorldToLocal(L, sw.x, sw.z, wa, wb);
        CHECK(std::fabs(wb) >= shipHullHalfB(G, wa) + kPlayerRadius - 0.05f, "nadador empurrado para fora do galeao (b=%.1f)", wb);
        // os 2 cascos não se atravessam: galeão parado, navio vindo direto contra o costado
        L.speed = 0; L.cmdThrottle = 0; N.speed = 0; N.cmdThrottle = 1; N.helmer = -1;
        { float bx, bz; shipLocalToWorld(L, 0.0f, G.hullHalfB + 30.0f, bx, bz); N.x = bx; N.z = bz; N.yaw = shipWrap(L.yaw - 1.5707963f); }
        bool hit = false;
        for (int i = 0; i < 30 * 40; ++i) {
            stepShip(N, 1.0f / 30.0f, nullptr);
            const auto& kp = kShipDefs[0].land;
            for (const auto& q : kp) { float wx, wz; shipLocalToWorld(N, q[0], q[1], wx, wz); if (shipPointInHull(L, wx, wz)) hit = true; }
        }
        CHECK(!hit, "navio atravessou o galeao");
        CHECK(N.speed == 0.0f, "navio parou no casco do galeao (vel %.2f)", N.speed);
        N.cmdThrottle = 0;
    }

    // ---- CHALUPA: pequena, sem canhões; leme e baú como os outros ----
    {
        const ShipDef& H = kShipDefs[ShipBoat];
        ShipState& B = shipState(ShipBoat);
        CHECK(!H.hasCannons && kShipDefs[0].hasCannons && kShipDefs[ShipGalleon].hasCannons, "so a chalupa nao tem canhao");
        CHECK(H.hull[8][0] - H.hull[0][0] < 20.0f, "chalupa e pequena (%.1f m)", H.hull[8][0] - H.hull[0][0]);
        float bx, bz, by;
        CHECK(shipFindSpawn(ShipBoat, W, W.spawnX(), W.spawnZ(), 14.0f, bx, bz, by), "sem agua para a chalupa");
        B = ShipState{}; B.kind = ShipBoat; B.exists = true; B.x = bx; B.z = bz; B.yaw = by;
        CHECK(!shipHullHitsLand(H, terrainPtr(), B.x, B.z, B.yaw) && !shipHitsShip(B, B.x, B.z, B.yaw), "chalupa nasceu em terra ou em cima de outro navio");
        const float kStep[5][2] = {{-4.69f, 0.87f}, {-4.96f, 1.12f}, {-5.2f, 1.36f}, {-5.5f, 1.61f}, {-5.73f, 1.85f}};
        for (const auto& st : kStep) for (float sb : {1.6f, -1.75f}) CHECK(std::fabs(shipFloor(H, st[0], sb) - st[1]) < 0.1f, "degrau chalupa a=%.2f: %.2f, malha %.2f", st[0], shipFloor(H, st[0], sb), st[1]);
        CHECK(std::fabs(shipFloor(H, -3.0f, 1.6f) - H.deckY) < 0.01f, "chao plano antes da escada da chalupa");
        PlayerState g; g.id = 4; shipLocalToWorld(B, 0.0f, H.hullHalfB + 2.0f, g.x, g.z); g.y = World::kSwimFloorH;
        InputCmd e; e.shipEv = ShipEvBoard; run(g, e, 0.1f);
        CHECK(g.ship == 1 && g.shipIdx == ShipBoat && std::fabs(g.y - H.deckY) < 0.01f, "embarcou na chalupa (ship=%d idx=%d y=%.2f)", g.ship, g.shipIdx, g.y);
        uint32_t rs3 = 4242;
        for (int i = 0; i < 80; ++i) {
            rs3 = rs3 * 1664525u + 1013904223u; float ang = (float)(rs3 >> 8) / 16777216.0f * 6.2831853f;
            InputCmd m; m.moveX = std::sin(ang); m.moveY = std::cos(ang); m.camYaw = 0.0f; m.flags = (i & 1) ? InputRun : 0;
            run(g, m, 0.4f);
            CHECK(shipFloor(H, g.sa, g.sb) > kNoFloor * 0.5f, "saiu do chao da chalupa");
            CHECK(std::hypot(g.sa - H.masts[0][0], g.sb - H.masts[0][1]) >= H.masts[0][2] - 0.02f, "dentro do mastro da chalupa");
            CHECK(shipChestDist(H, g.sa, g.sb) >= 0.3f || g.y > H.deckY + 1.2f, "dentro do bau da chalupa");
        }
        walkTo(g, -2.5f, H.chestB, 5.0f); walkTo(g, H.chestA, H.chestB, 4.0f);
        CHECK(shipChestDist(H, g.sa, g.sb) >= 0.3f && shipChestDist(H, g.sa, g.sb) <= H.chestReach, "alcanca o bau da chalupa (d=%.2f)", shipChestDist(H, g.sa, g.sb));
        walkTo(g, -3.0f, 1.65f, 5.0f); walkTo(g, -6.3f, 1.65f, 6.0f);
        CHECK(std::fabs(g.y - H.terraceY) < 0.08f, "subiu a escada da chalupa (y=%.2f a=%.2f b=%.2f)", g.y, g.sa, g.sb);
        walkTo(g, -6.3f, 0.3f, 3.0f); walkTo(g, H.helmA, 0.0f, 3.0f);
        InputCmd hc; hc.shipEv = ShipEvHelm; run(g, hc, 0.1f);
        CHECK(g.ship == 2 && B.helmer == 4, "assumiu o leme da chalupa (ship=%d helmer=%d a=%.2f b=%.2f)", g.ship, B.helmer, g.sa, g.sb);
        float x0 = B.x, z0 = B.z;
        hc = InputCmd{}; hc.shipEv = (uint8_t)(1u << ShipThrShift); run(g, hc, 3.0f);
        CHECK(B.speed > 5.0f, "chalupa acelera rapido (%.2f m/s em 3 s)", B.speed);
        CHECK(std::hypot(B.x - x0, B.z - z0) > 5.0f, "chalupa andou");
        hc = InputCmd{}; hc.shipEv = ShipEvHelm; run(g, hc, 0.1f);
        CHECK(g.ship == 1 && B.helmer == -1, "largou o leme da chalupa");
    }

    // ---- BERGANTIM: esguio e rápido, 2 mastros, canhões; leme, escadas e baú como os outros ----
    {
        const ShipDef& H = kShipDefs[ShipBrig];
        ShipState& B = shipState(ShipBrig);
        CHECK(H.hasCannons && H.nMasts == 2 && H.maxFwd > kShipDefs[ShipSloop].maxFwd, "bergantim: canhoes, 2 mastros, mais rapido que o navio");
        float bx, bz, by;
        CHECK(shipFindSpawn(ShipBrig, W, W.spawnX(), W.spawnZ(), 14.0f, bx, bz, by), "sem agua para o bergantim");
        B = ShipState{}; B.kind = ShipBrig; B.exists = true; B.x = bx; B.z = bz; B.yaw = by;
        CHECK(!shipHullHitsLand(H, terrainPtr(), B.x, B.z, B.yaw) && !shipHitsShip(B, B.x, B.z, B.yaw), "bergantim nasceu em terra ou em cima de outro navio");
        const float kStep[5][2] = {{-11.0f, 1.41f}, {-11.4f, 1.80f}, {-11.8f, 2.20f}, {-12.2f, 2.60f}, {-12.65f, 3.0f}};   // degraus do original deslocados 3 m (brigA)
        for (const auto& st : kStep) for (float sb : {2.3f, -2.45f}) CHECK(std::fabs(shipFloor(H, st[0], sb) - st[1]) < 0.1f, "degrau bergantim a=%.2f: %.2f, malha %.2f", st[0], shipFloor(H, st[0], sb), st[1]);
        PlayerState g; g.id = 5; shipLocalToWorld(B, 0.0f, H.hullHalfB + 2.0f, g.x, g.z); g.y = World::kSwimFloorH;
        InputCmd e; e.shipEv = ShipEvBoard; run(g, e, 0.1f);
        CHECK(g.ship == 1 && g.shipIdx == ShipBrig && std::fabs(g.y - H.deckY) < 0.01f, "embarcou no bergantim (ship=%d idx=%d y=%.2f)", g.ship, g.shipIdx, g.y);
        uint32_t rs4 = 777;
        for (int i = 0; i < 80; ++i) {
            rs4 = rs4 * 1664525u + 1013904223u; float ang = (float)(rs4 >> 8) / 16777216.0f * 6.2831853f;
            InputCmd m; m.moveX = std::sin(ang); m.moveY = std::cos(ang); m.camYaw = 0.0f; m.flags = (i & 1) ? InputRun : 0;
            run(g, m, 0.4f);
            CHECK(shipFloor(H, g.sa, g.sb) > kNoFloor * 0.5f, "saiu do chao do bergantim");
            for (int k = 0; k < H.nMasts; ++k) CHECK(std::hypot(g.sa - H.masts[k][0], g.sb - H.masts[k][1]) >= H.masts[k][2] - 0.02f, "dentro do mastro %d do bergantim", k);
        }
        walkTo(g, -7.0f, H.chestB, 6.0f); walkTo(g, H.chestA, H.chestB, 4.0f);
        CHECK(shipChestDist(H, g.sa, g.sb) >= 0.3f && shipChestDist(H, g.sa, g.sb) <= H.chestReach, "alcanca o bau do bergantim (d=%.2f)", shipChestDist(H, g.sa, g.sb));
        const float sbm = H.midB + 0.5f * (H.stairB0 + H.stairB1);
        walkTo(g, -9.0f, sbm, 5.0f); walkTo(g, H.stairA1 - 0.4f, sbm, 6.0f);
        CHECK(std::fabs(g.y - H.terraceY) < 0.08f, "subiu a escada do bergantim (y=%.2f a=%.2f b=%.2f)", g.y, g.sa, g.sb);
        walkTo(g, H.stairA1 - 0.4f, 0.3f, 3.0f); walkTo(g, H.helmA, 0.0f, 3.0f);
        InputCmd hc; hc.shipEv = ShipEvHelm; run(g, hc, 0.1f);
        CHECK(g.ship == 2 && B.helmer == 5, "assumiu o leme do bergantim (ship=%d helmer=%d a=%.2f b=%.2f)", g.ship, B.helmer, g.sa, g.sb);
        float x0 = B.x, z0 = B.z;
        hc = InputCmd{}; hc.shipEv = (uint8_t)(1u << ShipThrShift); run(g, hc, 3.0f);
        CHECK(B.speed > 4.0f, "bergantim acelera (%.2f m/s em 3 s)", B.speed);
        CHECK(std::hypot(B.x - x0, B.z - z0) > 5.0f, "bergantim andou");
        hc = InputCmd{}; hc.shipEv = ShipEvHelm; run(g, hc, 0.1f);
        CHECK(g.ship == 1 && B.helmer == -1, "largou o leme do bergantim");
        B.speed = 0.0f; B.cmdThrottle = 0;
    }

    // ---- VIDA E NAUFRÁGIO ----
    {
        CHECK(kShipDefs[ShipBoat].maxHp / kShipShellDamage == 4 && kShipDefs[ShipSloop].maxHp / kShipShellDamage == 10 && kShipDefs[ShipGalleon].maxHp / kShipShellDamage == 16,
              "balas para afundar: chalupa 4, navio 10, galeao 16");
        ShipState& N = shipState(ShipSloop); const ShipDef& D = kShipDefs[ShipSloop];
        N.speed = 0; N.cmdThrottle = 0; N.helmer = -1; N.hp = (int16_t)D.maxHp; N.sinkT = 0;
        float cx, cz; shipLocalToWorld(N, 0.0f, 0.0f, cx, cz);
        CHECK(shipShellDamage(N, cx, cz) == kShipShellDamage, "bala no meio do casco: %d", shipShellDamage(N, cx, cz));
        float ex, ez; shipLocalToWorld(N, 0.0f, shipHullHalfB(D, 0.0f) + 1.0f, ex, ez);
        int de = shipShellDamage(N, ex, ez);
        CHECK(de > kShipShellDamage / 2 && de < kShipShellDamage, "bala 1 m fora do casco tira menos (%d)", de);
        float fx, fz; shipLocalToWorld(N, 0.0f, shipHullHalfB(D, 0.0f) + 8.0f, fx, fz);
        CHECK(shipShellDamage(N, fx, fz) == 0, "bala a 8 m do casco nao pega");
        shipLocalToWorld(N, D.hull[8][0] + 5.0f, 0.0f, fx, fz);
        CHECK(shipShellDamage(N, fx, fz) == 0, "bala 5 m a frente da proa nao pega");
        // a bordo, o navio afunda: o jogador vai para a agua ao lado do casco, ninguem embarca, o casco some do caminho
        PlayerState q; q.id = 7; shipLocalToWorld(N, 0.0f, D.hullHalfB + 2.0f, q.x, q.z); q.y = World::kSwimFloorH;
        { InputCmd e; e.shipEv = ShipEvBoard; run(q, e, 0.1f); }
        CHECK(q.ship == 1 && q.shipIdx == ShipSloop, "embarcou antes do naufragio");
        N.hp = 0; N.sinkT = 1e-3f; N.speed = 3.0f;
        { InputCmd m; run(q, m, 0.2f); }
        float qa, qb; shipWorldToLocal(N, q.x, q.z, qa, qb);
        CHECK(q.ship == 0 && q.y < 0.1f && std::fabs(qb) > shipHullHalfB(D, qa), "naufragio joga quem esta a bordo na agua (ship=%d y=%.2f b=%.1f)", q.ship, q.y, qb);
        CHECK(shipBoardTarget(q.x, q.y, q.z) != ShipSloop, "nao embarca no navio afundando");
        { InputCmd e; e.shipEv = ShipEvBoard; run(q, e, 0.1f); }
        CHECK(q.ship == 0, "EMBARCAR recusado afundando");
        CHECK(shipShellDamage(N, cx, cz) == 0, "navio afundando nao leva mais dano");
        float sx = N.x, sz = N.z, sh = std::fabs(N.speed);
        { InputCmd m; run(q, m, kShipSinkTime); }
        CHECK(N.sinkT >= kShipSinkTime && std::fabs(N.speed) < sh * 0.1f && N.helmer == -1, "afundou em %.0f s e parou (vel %.2f, andou %.0f m)", kShipSinkTime, N.speed, std::hypot(N.x - sx, N.z - sz));
        float dy, pitch, roll; shipSinkPose(D, N.sinkT, dy, pitch, roll);
        CHECK(dy < -20.0f && roll > 0.3f, "pose do naufragio: desce %.0f m, aderna %.0f graus", dy, roll * 57.3f);
        float hx = N.x, hz = N.z;
        CHECK(!shipHullPush(N, hx, hz, kPlayerRadius) && supportHeightAt(N.x, N.z) < D.deckY - 0.5f, "casco afundando nao empurra nem e chao");
        N.sinkT = 0; N.hp = (int16_t)D.maxHp; N.speed = 0;
    }

    std::printf(fails ? "%d FALHAS\n" : "TODOS OS TESTES DO NAVIO PASSARAM\n", fails);
    return fails ? 1 : 0;
}
