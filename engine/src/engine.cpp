#include "engine.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <android/input.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "camera.h"
#include "camera_rig.h"
#include "map_gen.h"
#include "input.h"
#include "net_client.h"
#include "renderer.h"
#include "ui_theme.h"
#include "audio.h"
#include "loading.h"
#include "main_menu.h"
#include <GLES3/gl3.h>
#ifdef E0_CINEMA
#include <dirent.h>
#include <sys/stat.h>
#include <cctype>
#include "cinema.h"
#include "cinema_rec.h"
#endif
#include "wifi_lock.h"
#include "chat_input.h"
#include "team_client.h"
#include "monetize.h"
#include "auth.h"
#include "../../shared/conquest.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated", __VA_ARGS__)

namespace e0 {

Game* createGame();  // definido pelo jogo

namespace {

// Mundo para a colisão da câmera: terreno (a água também é piso: a câmera nunca desce abaixo de y = 0).
struct TerrainCamWorld : CameraWorld {
    const e0net::World* w = nullptr;
    float ground(float x, float z) const {
        float h = w ? e0net::terrain().heightAt(x, z) : 0.0f; h = h > 0.0f ? h : 0.0f;
        for (int i = 0; i < e0net::kMaxShips; ++i) {   // o convés dos navios também é chão para a câmera
            const e0net::ShipState& S = e0net::shipState(i); const e0net::ShipDef& D = e0net::shipDef(i);
            if (!S.exists) continue;
            float a, b; e0net::shipWorldToLocal(S, x, z, a, b);
            if (a >= D.aMin - 0.5f && a <= D.aMax + 0.5f && std::fabs(b) <= e0net::shipDeckHalfW(D, a) + 0.5f) h = std::max(h, D.deckY);
        }
        return h;
    }
    float sweepSphere(const Vec3& from, const Vec3& to, float r) const override {
        Vec3 d = to - from;
        float len = std::sqrt(dot(d, d));
        if (len < 1e-4f) return 1.0f;
        int steps = (int)(len / 0.25f) + 1;
        float prev = 0.0f;
        for (int i = 1; i <= steps; ++i) {
            float t = (float)i / (float)steps;
            Vec3 p = from + d * t;
            if (p.y - r < ground(p.x, p.z)) {          // bateu: refina entre prev e t
                float lo = prev, hi = t;
                for (int k = 0; k < 5; ++k) {
                    float m = 0.5f * (lo + hi);
                    Vec3 q = from + d * m;
                    if (q.y - r < ground(q.x, q.z)) hi = m; else lo = m;
                }
                return lo;
            }
            prev = t;
        }
        return 1.0f;
    }
};

struct Touch { int id = -1; float sx = 0, sy = 0, x = 0, y = 0; };

constexpr double kDoubleTapSec = 0.30;  // janela do duplo toque (pulo)

#if E0_DEBUG_UI
// Presets de hora do mundo (segundos do ciclo; 0 = nascer do sol, 900 = pôr do sol), em ordem cronológica a partir da meia-noite.
struct TimePreset { const char* name; float cycleSec; };
constexpr TimePreset kTimePresets[6] = {
    {"MEIA-NOITE", 1350.0f}, {"NASCER DO SOL", 30.0f}, {"MANHA", 180.0f},
    {"MEIO-DIA", 450.0f},    {"POR DO SOL", 870.0f},   {"NOITE", 1100.0f},
};
#endif

// Cor do corpo por jogador (índice = id).
Vec3 playerColor(int id) {
    static const Vec3 c[] = {{0.10f, 0.42f, 0.52f}, {0.75f, 0.22f, 0.20f}, {0.25f, 0.60f, 0.25f},
                             {0.85f, 0.65f, 0.15f}, {0.55f, 0.32f, 0.72f}};
    return c[(id < 0 ? 0 : id) % 5];
}

struct Engine {
    android_app* app = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig cfg = nullptr;
    bool ready = false;
    int w = 0, h = 0;

    Renderer renderer;
    Camera camera;          // câmera final (cópia da saída do CameraRig)
    CameraRig rig;          // sistema de câmera (3ª/1ª pessoa, colisão, suavização, FOV, tremor)
    TerrainCamWorld camWorld;  // colisão da câmera com o relevo
    int lastWorldGen = -1;     // geração do mundo já enviada ao renderer
    e0net::World::Stats wstats;  // estatísticas do mundo atual (aba TERRA)
    Game* game = nullptr;
    Input input;
    NetClient net;
    std::vector<e0net::PlayerState> remotes;
    AnimPlayer localAnim;
    AnimPlayer remoteAnims[e0net::kMaxPlayers];
    uint8_t remoteLastAct[e0net::kMaxPlayers] = {};
    bool remoteSeen[e0net::kMaxPlayers] = {};
    int animDebugClip = -1;  // -1 = AUTO (gameplay), 0..ClipCount-1 = clipe fixo p/ inspeção na aba JOG

    // Controles de toque
    Touch joyT, lookT;      // joystick (metade esquerda) e olhar (metade direita)
    int runPtr = -1;        // ponteiro que está SEGURANDO o botão de correr (segurar = correr)
    double lastRunTap = -10; // instante do último toque no botão (2 toques rápidos = pular)
    bool jumpQueued = false;
    uint8_t actionQueued = 0;  // PlayerAction disparada no frame (1/5/6=Bater 1..3, 2=Cortar, 3=Minerar, 4=Coletar)
    uint32_t punchRng = 0x9E3779B9u;
    int lastPunchIdx = -1;

    uint8_t nextPunchAction() {
        static const uint8_t kPunches[3] = {e0net::ActPunch, e0net::ActPunch2, e0net::ActPunch3};
        uint64_t ns = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        punchRng ^= (uint32_t)(ns ^ (ns >> 17));
        punchRng = punchRng * 1664525u + 1013904223u;
        int idx = (int)((punchRng >> 16) % 3u);
        if (idx == lastPunchIdx) {
            punchRng = punchRng * 1664525u + 1013904223u;
            if ((punchRng & 3u) != 0u) idx = (idx + 1 + (int)((punchRng >> 24) & 1u)) % 3;
        }
        lastPunchIdx = idx;
        return kPunches[idx];
    }

    // Painel de debug (canto superior direito) com abas; geometria do último frame guardada p/ toque
    enum { kTabs = 9 };
    bool dbgOpen = true;                  // false = recolhido (1 linha)
    int dbgTab = 0;                       // aba ativa
    float dbgX = 0, dbgY = 0, dbgW = 0, dbgH = 0;
    float dbgBarH = 0, dbgToggleEnd = 0, tabX[kTabs] = {}, tabW[kTabs] = {};
    float fpsAcc = 0, fpsWorst = 0; int fpsFrames = 0;      // janela de 0.5 s
    float fps = 0, frameMs = 0, worstMs = 0;
    double logAcc = 0;

    // Modo God (Aba GOD do debug)
    bool godFly = false;
    bool godFlyFast = false;
    float flyAscendDist = 0.0f;
    float flyDescendDist = 0.0f;

    // Qualidade gráfica (sombra/reflexo/LOD): AUTOMÁTICA pelo FPS ou fixa (toque na 1ª linha da aba GRAF)
    int gradeMode = 1;                       // filtro de cor pirata: 0 = OFF, 1 = SUAVE, 2 = FORTE (aba GRAF, 2ª linha)
    int qMode = -1;                          // -1 = AUTO, 0..2 = fixa
    int qCur = Renderer::QMed;
    int slowCnt = 0, fastCnt = 0;            // janelas de 0.5 s abaixo / acima da meta
    double lastBad[3] = {-1000, -1000, -1000};  // quando cada nível falhou (não sobe de volta por 90 s)
    float dbgPad = 0, dbgLineH = 0;
    // Indicador de coleta (SÓ VISUAL: o botão COLETAR aparece mas NÃO faz nada — ainda não existe sistema de coleta)
    std::vector<uint32_t> collectiblesBySlot[e0net::kIslandSlots]; // gids (ilha << 20 | índice) particionados por slot de ilha
    uint32_t lastTakenGen = 0xFFFFFFFFu, lastHarvestGen = 0xFFFFFFFFu;
    uint32_t animFrame = 0;
    int collectCur = -1;                     // alvo atual (-1 = nenhum)
    float collectAlpha = 0;                  // aparece/some suavemente
    uint8_t collectType = 0; Vec3 collectPos{}; float collectDist = 0;
    int collectPtr = -1; bool collectDown = false;   // dedo em cima do botão (só feedback visual)
    // Coleta de verdade: toque em COLETAR -> animação COLETAR -> ao chegar a mão ao chão (kCollectGrabT) pede ao servidor
    // (MsgCollect). O item some na hora (otimista) e só volta se o servidor não confirmar em kCollectWaitT.
    static constexpr float kCollectGrabT = 0.45f, kCollectWaitT = 2.0f;
    // Cortar árvores / minerar pedras: no momento do golpe da animação (hitPendT) o jogo manda MsgAct/ActHit; o servidor sorteia o material.
    struct HitPend { int idx = -1; float t = 0; uint8_t type = 0; };
    HitPend hitPend[3];
    // ---- Áudio: passos sincronizados com a fase da animação (por superfície do bioma), nado, pouso e ambientes (oceano / selva) ----
    Audio audio;
    struct FootSfx { int lastN = -1; bool wasGround = true, wasSwim = false; float airT = 0, lastSwimPh = 0; };
    FootSfx footLocal, footRemote[e0net::kMaxPlayers];
    float ambT = 0;
    struct SfxPend { float t; bool stone; float x, z; };
    std::vector<SfxPend> sfxPend;      // golpes de OUTROS jogadores (o som sai no momento do impacto da animação deles)
    void updateHitSfx(float dt) {
        for (size_t i = 0; i < sfxPend.size();) {
            SfxPend& s = sfxPend[i];
            if ((s.t -= dt) > 0) { ++i; continue; }
            float dx = s.x - camera.position.x, dz = s.z - camera.position.z, d = std::sqrt(dx * dx + dz * dz);
            if (d < 35.0f) {
                float fall = 1.0f - d / 35.0f, ry = rig.yaw();
                float pan = d > 0.5f ? (dx * std::cos(ry) + dz * std::sin(ry)) / d * 0.8f : 0.0f;
                if (s.stone) audio.playMine(fall * fall, pan); else audio.playChop(fall * fall, pan);
            }
            sfxPend[i] = sfxPend.back(); sfxPend.pop_back();
        }
    }
    int surfaceAt(float x, float z) const {
        if (!net.world()) return Audio::SurfGrass;
        switch (e0net::terrain().biomeAt(x, z)) {
            case e0net::BiomeSand: return Audio::SurfSand;
            case e0net::BiomeGrass: case e0net::BiomeForest: return Audio::SurfGrass;
            default: return Audio::SurfHard;
        }
    }
    // um personagem (local ou remoto): dispara os sons dele; remotos têm volume por distância e pan pela câmera
    void footAudio(FootSfx& f, const AnimPlayer& ap, float px, float pz, float speed, bool grounded, bool swimming, bool local) {
        float vol = 1.0f, pan = 0.0f;
        if (!local) {
            float dx = px - camera.position.x, dz = pz - camera.position.z, d = std::sqrt(dx * dx + dz * dz);
            if (d > 30.0f) { f.lastN = -1; f.wasGround = grounded; f.wasSwim = swimming; return; }
            float fall = 1.0f - d / 30.0f; vol = fall * fall;
            float ry = rig.yaw(); pan = d > 0.5f ? (dx * std::cos(ry) + dz * std::sin(ry)) / d * 0.8f : 0.0f;
        }
        const int surf = surfaceAt(px, pz);
        const bool moving = grounded && !swimming && speed > 0.5f;
        if (moving) {
            int n = (int)std::floor(ap.locoPhase() * 2.0f);
            if (f.lastN >= 0 && n != f.lastN) audio.playStep(surf, (0.35f + 0.5f * std::min(1.0f, speed / 6.0f)) * vol, pan);
            f.lastN = n;
        } else f.lastN = -1;
        if (!f.wasGround && grounded && f.airT > 0.25f && !swimming) audio.playStep(surf, 1.0f * vol, pan);   // pouso
        f.airT = grounded ? 0.0f : f.airT + 1.0f / 60.0f;
        f.wasGround = grounded;
        if (swimming) {
            if (!f.wasSwim) audio.playSwim(0.9f * vol, pan);                                                   // mergulho
            else if (speed > 0.4f && ap.swimPhase() < f.lastSwimPh - 0.5f) audio.playSwim(0.6f * vol, pan);    // uma braçada por ciclo
            f.lastSwimPh = ap.swimPhase();
        }
        f.wasSwim = swimming;
    }
    // ambientes: oceano pela proximidade da água, selva pelo tipo de chão em volta (mais baixa à noite)
    void updateAmbience(float dt, const e0net::PlayerState& me) {
        ambT -= dt; if (ambT > 0) return; ambT = 0.3f;
        const e0net::World* W = net.world();
        float ocean = 0.35f, jungle = 0.3f;   // sem mundo (chão plano): ambiente fixo suave
        if (W) {
            static const float kR[3] = {6.0f, 20.0f, 45.0f}, kW[3] = {1.0f, 0.6f, 0.3f};
            float wat = 0, wsum = 0, forest = 0;
            for (int r = 0; r < 3; ++r) {
                int nw = 0; float fo = 0;
                for (int k = 0; k < 8; ++k) {
                    float a = (float)k * 0.785398f, x = me.x + std::cos(a) * kR[r], z = me.z + std::sin(a) * kR[r];
                    if (e0net::terrain().heightAt(x, z) < 0.0f) ++nw;
                    else switch (e0net::terrain().biomeAt(x, z)) { case e0net::BiomeForest: fo += 1.0f; break; case e0net::BiomeGrass: fo += 0.5f; break; case e0net::BiomeDirt: fo += 0.12f; break; default: break; }
                }
                wat += kW[r] * nw / 8.0f; forest += kW[r] * fo / 8.0f; wsum += kW[r];
            }
            wat /= wsum; forest /= wsum;
            bool inWater = e0net::terrain().heightAt(me.x, me.z) < 0.0f;
            ocean = inWater ? 1.0f : 0.06f + 0.94f * std::min(1.0f, wat * 2.4f);
            jungle = std::min(1.0f, forest * 1.5f) * (inWater ? 0.15f : 1.0f);
        }
        float t = net.timeOfDay(); bool night = t > 900.0f;
        if (loadActive || uiFull()) { ocean = jungle = 0.0f; }   // loading e telas cheias: só a trilha sonora
        audio.setAmbience(ocean * 0.9f, jungle * (night ? 0.55f : 1.0f) * 0.9f);
    }
    float toolWarnT = 0;
    uint32_t lastRespawnGen = 0xFFFFFFFFu;
    std::unordered_set<uint32_t> harvestDone; bool harvestInit = false; float harvestWarm = 0, harvestConnT = 0;
    int collectDropId = -1;                           // alvo atual = item SOLTO no chão (id do servidor) em vez de coletável do mundo
    int dropPendId = -1; float dropPendT = 0; uint8_t dropPendItem = 0; uint16_t dropPendN = 0;
    const char* collectLabel = "ITEM"; char collectLabelBuf[40] = {};
    int collectPending = -1; float collectPendT = 0;   // coletável aguardando o momento da "pegada"
    std::unordered_map<uint32_t, float> collectLocalHide;   // por gid: segundos restantes de esconder otimista (ausente/0 = nada)

    // ---- Notificações (canto central esquerdo): 1 linha fixa por notificação, fundo preto com a ponta direita esvanecendo ----
    struct Notif { char text[40]; int item; int count; float age; };
    std::vector<Notif> notifs;                         // mais antiga primeiro; no máximo kMaxNotifs
    static constexpr int kMaxNotifs = 5;
    static constexpr float kNotifLife = 4.0f;
    void notify(const char* text, int item = 0, int count = 0) {
        for (Notif& n : notifs)   // mesmo item ainda na tela: soma (PEGOU X3 GRAVETO) em vez de empilhar linhas
            if (item && n.item == item && n.age < kNotifLife - 0.8f) {
                n.count += count; n.age = std::min(n.age, 0.25f);
                std::snprintf(n.text, sizeof n.text, "PEGOU X%d %s", n.count, itemDef(item).name);
                return;
            }
        Notif n{}; n.item = item; n.count = count; n.age = 0;
        if (item) std::snprintf(n.text, sizeof n.text, "PEGOU X%d %s", count, itemDef(item).name);
        else std::snprintf(n.text, sizeof n.text, "%s", text);
        if ((int)notifs.size() >= kMaxNotifs) notifs.erase(notifs.begin());
        notifs.push_back(n);
    }
    // Alvo de AÇÃO (SÓ VISUAL): palmeira/coqueiro (CORTAR) ou pedra grande (MINERAR) à FRENTE do personagem; sem alvo o botão vira BATER.
    std::vector<uint32_t> actionablesBySlot[e0net::kIslandSlots]; // gids das palmeiras, coqueiros, toras e pedras particionados por slot de ilha
    int actCur = -1; float actAlpha = 0; uint8_t actType = 0; Vec3 actPos{}; float actDist = 0;
    int actionPtr = -1;                      // dedo no botão de ação (só consome o toque: nenhuma ação)
    double hudT = 0;
    struct TeleTarget { const char* name; float x, z; };
    std::vector<TeleTarget> tele;            // pontos de interesse do mundo atual (só os que existem nesta ilha)
    int teleIdx = -1;
    int timeIdx = -1;                        // preset de hora escolhido nas setas (-1 = ainda não escolhido)

    // Layout da UI (em pixels, proporcional à altura da tela)
    struct Rect2D {
        float x = 0, y = 0, w = 0, h = 0;
        bool contains(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
    };
    int uiScale() const { return std::max(2, h / 360); }

    // HUD em telas MENOS largas (16:9, 4:3, tablets): joystick e botões redondos encolhem com a proporção (1,0 em >= 2,0:1 como o
    // moto g75; 0,9 em 16:9; 0,7 em 4:3) e a barra de acesso rápido se encaixa no vão entre o joystick e a grade de botões (hotbarSlotGeom).
    float hudK() const {
        const float a = h > 0 ? (float)w / (float)h : 2.21f;
        const float k = std::min(1.0f, std::max(0.0f, (a - 1.33f) / (2.0f - 1.33f)));
        return 0.70f + 0.30f * k;
    }
    float joyR() const { return 0.14f * h * hudK(); }
    float joyHomeX() const { return joyR() * 1.8f; }
    float joyHomeY() const { return h - joyR() * 1.7f; }
    float runR() const { return 0.085f * h * hudK(); }
    float runX() const { return w - runR() * 2.4f; }
    float runY() const { return h - runR() * 2.6f; }
    // BOTÕES DA DIREITA numa GRADE fixa em volta do principal (mesmo espaçamento em tudo; colunas da direita para a esquerda, linhas de baixo para cima):
    //   linha 0: [MIRAR/SALVAR/SOLTAR] [BATER/CORTAR/ATIRAR/RE] [CORRER/ACELERAR]
    //   linha 1: [CARREGAR]            [COLETAR]                [EMBARCAR/DESCER/SOLTAR LEME]
    //   linha 2:                                                [câmera (pequeno)]
    float btnGap() const { return runR() * 2.45f; }
    float gridX(int col) const { return runX() - btnGap() * (float)col; }
    float gridY(int row) const { return runY() - btnGap() * (float)row; }
    float camR() const { return runR() * 0.62f; }
    float camX() const { return gridX(0); }
    float camY() const { return gridY(2); }
    bool inCamButton(float x, float y) const {
        float dx = x - camX(), dy = y - camY(), r = camR() * 1.4f;
        return dx * dx + dy * dy <= r * r;
    }
    float collectR() const { return runR(); }                    // mesmo tamanho dos outros botões
    float collectX() const { return gridX(1); }
    float collectY() const { return gridY(1); }                      // COLETAR acima do BATER
    float actionR() const { return runR(); }
    float actionX() const { return gridX(1); }
    float actionY() const { return gridY(0); }                       // BATER/CORTAR/MINERAR ao lado do CORRER
    bool inActionButton(float x, float y) const {
        const e0net::PlayerState me = net.predicted();
        if (e0net::isSwimmingPos(me.x, me.y, me.z)) return false;
        float dx = x - actionX(), dy = y - actionY(), r = actionR() * 1.15f;
        return dx * dx + dy * dy <= r * r;
    }
    bool inCollectButton(float x, float y) const {
        if (collectAlpha < 0.5f) return false;
        float dx = x - collectX(), dy = y - collectY(), r = collectR() * 1.15f;
        return dx * dx + dy * dy <= r * r;
    }
    static const char* collectName(uint8_t type) { return type == e0net::ObjStick ? "GRAVETO" : (type == e0net::ObjPebble ? "PEDRINHA" : (type == e0net::ObjCloth ? "TECIDO" : "ITEM")); }
    static const char* actionName(uint8_t type) { return type == e0net::ObjPalm ? "PALMEIRA" : (type == e0net::ObjCoconut ? "COQUEIRO" : (type == e0net::ObjLog ? "TORA" : "PEDRA")); }
    const char* actionNameCur() const {   // pedras especiais mostram o minério
        if (actType == e0net::ObjRock && actCur >= 0) {
            const e0net::World* AW = net.island(e0net::gidSlot((uint32_t)actCur));
            uint8_t ore = AW ? AW->oreOf(e0net::gidIdx((uint32_t)actCur)) : 0;
            if (ore == 1) return "MINERIO DE OURO";
            if (ore == 2) return "MINERIO DE DIAMANTE";
            if (ore == 3) return "MINERIO DE FERRO";
        }
        return actionName(actType);
    }
    const char* actionLabel() const { return actCur < 0 ? "BATER" : (actType == e0net::ObjRock ? "MINERAR" : "CORTAR"); }

    // Barra de Acesso Rápido (Hotbar): 5 slots no centro inferior da tela
    static constexpr int kHotbarSlots = 5;
    // ---- Fome (só cliente por enquanto; sem vida/dano ainda) ----
    // 100 = saciado, 0 = faminto. Cai em ~20 min parado (correr/nadar 2x, nado rápido e golpes 3x). Por enquanto SÓ o coco (consumível) restaura; planejado: a fome só se recupera com coco.
    static constexpr float kHungerDrainPerSec = 100.0f / 1200.0f, kCoconutFood = 25.0f;
    float hunger = 100.0f; bool hungerWarnLow = false, hungerWarnZero = false;
    // ---- Vida e moedas (só cliente; sem dano/morte ainda) ----
    // Vida 0..100: com a FOME em 0 a vida desce (~2,5 min até zerar); com a fome entre 85 e 100 a vida sobe (~3,3 min de 0 a 100); entre 0 e 85 fica estável.
    // Moedas: só um contador por enquanto (sem utilidade; uso futuro).
    static constexpr float kStarveDrainPerSec = 100.0f / 150.0f, kRegenPerSec = 100.0f / 200.0f, kRegenMinHunger = 85.0f;
    static constexpr float kLowHealth = 30.0f, kLowHunger = 25.0f;   // abaixo disso as bordas da tela piscam (vermelho = vida, laranja = fome)
    // SEM FORÇA: com fome baixa (< kLowHunger) ou pouca vida (< kLowHealth) não dá para CORRER nem PULAR (voo do GOD não conta); avisa ao tentar
    bool tooWeak() const { return !godFly && (hunger < kLowHunger || health < kLowHealth); }
    float weakWarnT = 0.0f;
    void warnWeak() { if (weakWarnT <= 0.0f) { notify(health < kLowHealth ? "POUCA VIDA: SEM FORCA PARA CORRER OU PULAR" : "COM FOME: SEM FORCA PARA CORRER OU PULAR"); weakWarnT = 2.5f; } }
    float hitFlashT = 0.0f;   // piscar vermelho ao levar dano de outro jogador
    float health = 100.0f, godLifeUnused = 0.0f; bool godLife = false; int coins = 0;
    void updateHealth(float dt) {
        if (dying()) return;
        if (godLife) { health = 100.0f; return; }
        if (health <= 0.0f) { die(); return; }   // checa ANTES de regenerar (senão a regeneração "salva" a vida zerada)
        if (hunger <= 0.0f) health = std::max(0.0f, health - kStarveDrainPerSec * dt);
        else if (hunger >= kRegenMinHunger) health = std::min(100.0f, health + kRegenPerSec * dt);
    }
    // MORTE (multiplayer): perde TUDO (itens do inventário e moedas, na hora) e FICA CAÍDO no chão (os outros veem o corpo com a TAG "ID n + MORTO") até:
    //   - ser SALVO por outro jogador (RCP de kReviveTime s: volta com 30 de vida) ou - ser CARREGADO no ombro (o corpo acompanha quem carrega) ou
    //   - tocar RENASCER (aparece após kRespawnBtnAfter s; offline sempre): volta ao ponto de nascimento com vida/fome 100.
    // O status (morto/carregado/salvando) é do SERVIDOR (PlayerState.status/link); aqui só se avisa a morte (ActDie) e se reage ao que ele replica.
    static constexpr float kRespawnBtnAfter = 10.0f, kWakeFade = 1.6f;
    bool selfDead = false, sawDeadFlag = false; float deadT = 0.0f, wakeFadeT = 0.0f; int deathLostItems = 0, deathLostCoins = 0;
    bool dying() const { return selfDead; }
    bool deathBtnVisible() const { return selfDead && (deadT >= kRespawnBtnAfter || !net.connected()); }
    void die() {
        wheelOpen = false;
        if (selfDead) return;
        deathLostItems = 0; for (const auto& st : inv) deathLostItems += st.n;
        deathLostCoins = 0;   // MOEDAS NÃO se perdem ao morrer (são compradas/ganhas por anúncio; ficam na carteira: wallet.cfg)
        { bool dag = false; for (const auto& st : inv) if (st.id == ItDagger && st.n > 0) dag = true;   // morrer: perde tudo menos a adaga
          for (auto& st : inv) st = {}; if (dag) inv[kInvHotFirst] = {ItDagger, 1}; }
        invSel = -1; invDragPtr = -1; hotbarCur = 0;
        craftBusy = -1; invOpen = false; craftOpen = false; chestUi = false; closeShop();
        armaLoaded = false; armaReloadT = -1.0f; armaAim = false; armaFireQ = -1.0f;
        selfDead = true; sawDeadFlag = false; deadT = 0.0f; wakeFadeT = 0.0f;
        input.moveX = input.moveY = 0; runPtr = -1;
        if (net.connected()) net.requestAct(e0net::ActDie, 0, 0);
        LOG("MORREU: perdeu %d itens e %d moedas", deathLostItems, deathLostCoins);
    }
    void revivedByOthers() {   // o servidor tirou o status MORTO: alguém terminou o RCP
        selfDead = false; health = 30.0f; hunger = std::max(hunger, 40.0f); hungerWarnLow = hungerWarnZero = false; wakeFadeT = kWakeFade;
        notify("VOCE FOI SALVO");
    }
    void respawnSelf(bool viaAd = false) {       // botão RENASCER (viaAd = pulou a espera assistindo um anúncio premiado)
        selfDead = false; health = 100.0f; hunger = 100.0f; hungerWarnLow = hungerWarnZero = false; wakeFadeT = kWakeFade;
        net.requestRespawn(); rig.snap();
        // INTERSTICIAL ao renascer pelo botão: no máx. 1 a cada kAdGap s (e nunca logo depois de um premiado)
        if (!viaAd && adGapT >= kAdGap && (pay.status() & Monetize::InterstitialReady)) { adGapT = 0.0f; pay.showInterstitial(); }
    }
    // ---- MONETIZAÇÃO (AdMob + Google Play Billing; ver monetize.h e CLAUDE.md "Monetização") ----
    Monetize pay;
    Auth auth;      // CONTA (tela de login: Google pelo Firebase ou visitante); ver auth.h
    static constexpr int kAdTagShop = 1, kAdTagRespawn = 2;   // onde o anúncio premiado foi pedido (volta no evento REWARD)
    static constexpr float kAdGap = 180.0f;                    // intervalo mínimo entre anúncios de tela cheia
    float adGapT = 0.0f, payPollT = 0.0f;
    // morto esperando: RENASCER AGORA assistindo um anúncio (só enquanto o botão normal ainda não apareceu)
    bool deathAdVisible() const { return selfDead && !deathBtnVisible() && pay.active() && (pay.status() & Monetize::RewardedReady); }
    void updateMonetize(float dt) {
        adGapT += dt;
        payPollT -= dt;
        if (payPollT > 0.0f || !pay.active()) return;
        payPollT = 0.25f;
        std::vector<std::string> evs;
        pay.poll(evs);
        if (pay.status() & Monetize::AdShowing) adGapT = 0.0f;
        for (const std::string& e : evs) {
            std::vector<std::string> f; size_t a = 0;
            for (;;) { size_t b = e.find('|', a); f.push_back(e.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
            while (f.size() < 3) f.emplace_back();
            const std::string& k = f[0];
            if (k == "REWARD") {
                int tag = std::atoi(f[1].c_str());
                if (tag == kAdTagShop) {
                    if (netGame) net.requestAct(e0net::ActAdReward, 1, 0);   // online: quem credita é o servidor (com limite de taxa)
                    else {
                        int add = 50; { int n; const ShopItem* it = shopItems(n); for (int i = 0; i < n; ++i) if (it[i].flags & ShopAd) add = it[i].price; }
                        coins += add;
                        char m[48]; std::snprintf(m, sizeof m, "+%d MOEDAS PELO ANUNCIO", add);
                        if (shopOpen) shopSay(m); else notify(m);
                    }
                } else if (tag == kAdTagRespawn && selfDead) respawnSelf(true);
                adGapT = 0.0f;
            } else if (k == "AD_FAIL") {
                if (std::atoi(f[1].c_str()) == kAdTagShop && shopOpen) shopSay("ANUNCIO INDISPONIVEL, TENTE DE NOVO");
            } else if (k == "BUY_OK") {
                while (f.size() < 5) f.emplace_back();
                if (!f[3].empty() && walletHasToken(f[3])) { LOG("compra %s ja entregue antes (token repetido): ignorada", f[1].c_str()); continue; }
                if (netGame) {   // online: o SERVIDOR credita (recibo, 1 vez por token); aqui só guarda o recibo até ele confirmar
                    if (!f[3].empty()) walletAddReceipt(f[1], f[3], f[4], f[2] == "1");
                    if (shopOpen) shopSay("COMPRA OK: CREDITANDO NO SERVIDOR..."); else notify("COMPRA OK: CREDITANDO NO SERVIDOR...");
                    saveWallet(); continue;
                }
                shopDeliver(f[1], f[2] == "1");
                if (!f[3].empty()) { walletAddReceipt(f[1], f[3], f[4], f[2] == "1"); }
                saveWallet();
            } else if (k == "BUY_CANCEL") {
                if (shopOpen) shopSay("COMPRA CANCELADA");
            } else if (k == "BUY_PENDING") {
                if (shopOpen) shopSay("PAGAMENTO PENDENTE: AS MOEDAS CHEGAM QUANDO FOR APROVADO"); else notify("PAGAMENTO PENDENTE");
            } else if (k == "BUY_FAIL") {
                std::string m = "COMPRA FALHOU: " + f[2];
                for (char& c : m) c = (char)std::toupper((unsigned char)c);
                if (m.size() > 44) m.resize(44);
                if (shopOpen) shopSay(m.c_str()); else notify(m.c_str());
            } else if (k == "PRICE") {
                std::string v;   // só ASCII imprimível (a Play usa espaço não separável: "R$\u00a04,90")
                for (unsigned char c : f[2]) v += (c >= 32 && c < 127) ? (char)c : (c == 0xC2 ? '\0' : ' ');
                v.erase(std::remove(v.begin(), v.end(), '\0'), v.end());
                shopPlayPrice[f[1]] = v;
            }
        }
    }
    void updateDeath(float dt) {
        wakeFadeT = std::max(0.0f, wakeFadeT - dt);
        hitFlashT = std::max(0.0f, hitFlashT - dt);
        if (!selfDead) return;
        deadT += dt;
        const uint8_t st = net.predicted().status;
        if (st & e0net::StDead) sawDeadFlag = true;
        else if (sawDeadFlag && net.connected() && deadT > 1.0f) revivedByOthers();   // já estava marcado como morto e deixou de estar: foi salvo
    }
    Rect2D deathBtnRect() const { float u = std::min((float)h / 1080.0f, (float)w / 1900.0f); return {w * 0.5f - 230 * u, h * 0.74f, 460 * u, 92 * u}; }

    // ---- Navio: embarcar / leme (ver shared/ship.h) ----
    // shipCtx: 0 = nada, 1 = perto do casco (EMBARCAR), 2 = a bordo (DESCER, LEME se perto dele), 3 = no leme (ACELERAR, RE, SOLTAR; joystick = leme)
    int shipCtx = 0; bool shipNearHelm = false;
    int shipSel = 0;   // navio da HUD: aquele em que estou, ou o mais perto ao alcance de embarque
    const e0net::ShipState& curShip() const { return e0net::shipState(shipSel); }
    // Navio cuja VIDA aparece na HUD: o meu (a bordo) ou o mais perto a <= maxGap m do casco; -1 = nenhum
    int hpShip(float maxGap) const {
        const e0net::PlayerState me = net.predicted();
        if (me.ship && me.shipIdx < e0net::kMaxShips && e0net::shipState(me.shipIdx).exists) return me.shipIdx;
        int best = -1; float bd = 1e30f;
        for (int i = 0; i < e0net::kMaxShips; ++i) {
            const e0net::ShipState& S = e0net::shipState(i);
            if (!S.exists) continue;
            const e0net::ShipDef& D = e0net::shipDef(i);
            float d = std::hypot(S.x - me.x, S.z - me.z) - 0.5f * (D.hull[8][0] - D.hull[0][0]);
            if (d <= maxGap && d < bd) { bd = d; best = i; }
        }
        return best;
    }
    const e0net::ShipDef& curDef() const { return e0net::shipDef(shipSel); }
    // Baú do convés: por enquanto só o visual é do cliente (a tampa abre só para quem tocou; ainda não guarda itens)
    bool chestNear = false, chestOpen = false, chestUi = false; float chestOpenT = 0.0f; Vec3 chestPos{};
    int chestSelSide = 0, chestSelIdx = -1;   // seleção na tela do baú: 1 = slot do baú, 2 = slot da mochila
    std::string chestMsg; float chestMsgT = 0.0f;   // aviso dentro da tela do baú (as notificações ficam escondidas lá)
    int shipThrPtr = -1, shipRevPtr = -1;     // dedos segurando ACELERAR / RE
    uint8_t shipEvQueued = 0;                  // eventos de borda para o próximo input
    float shipSpeedShown = 0.0f;

    // ---- CANHOES: o jogador a bordo vê o mar DE CIMA e toca onde a bala deve cair (o servidor valida e simula) ----
    bool gunnerMode = false; float gunBlend = 0.0f;
    bool gunGranted = false; float gunWait = 0.0f;               // o servidor concede 1 dos 2 lugares (StGunner)
    bool gunHasTarget = false; float gunTx = 0, gunTz = 0;      // último ponto tocado (mostrado como mira)
    float gunReload = 0.0f, gunMsgT = 0.0f; const char* gunMsg = "";
    struct CBall { float t0, flight; Vec3 from, to; };
    int shipLifeSt[e0net::kMaxShips] = {-1, -1, -1, -1};   // 0 = não existe, 1 = boiando, 2 = afundando (para avisar nas mudanças)
    std::vector<CBall> cballs; float cannonClock = 0.0f;
    static constexpr float kGunPitch = -1.50f, kGunFov = 1.10f;
    Rect2D gunExitRect() const { float u = (float)uiScale(); return {(float)w - 100.0f * u - 24.0f, (float)h - 30.0f * u - 24.0f, 100.0f * u, 30.0f * u}; }
    Camera gunCamera() const {   // de cima, com a proa para cima da tela
        const e0net::ShipState& S = curShip();
        Camera c; c.yaw = S.yaw; c.pitch = kGunPitch; c.fovY = kGunFov; c.zFar = 700.0f;
        Vec3 f = c.forward();
        const float dist = curDef().gunHeight / -std::sin(kGunPitch);
        c.position = Vec3{S.x, 0.0f, S.z} - f * dist;
        return c;
    }
    bool gunPick(float px, float py, const Camera& c, float& gx, float& gz) const {   // pixel -> ponto no plano da água (y = 0)
        const float asp = h > 0 ? (float)w / (float)h : 1.0f, ty = std::tan(c.fovY * 0.5f);
        const float nx = (2.0f * px / (float)w - 1.0f), ny = (1.0f - 2.0f * py / (float)h);
        Vec3 d = c.forward() + c.rightVec() * (nx * ty * asp) + c.upVec() * (ny * ty);
        if (d.y > -0.02f) return false;
        float t = -c.position.y / d.y;
        gx = c.position.x + d.x * t; gz = c.position.z + d.z * t;
        return true;
    }
    void gunnerTouch(float x, float y) {
        if (gunExitRect().contains(x, y)) { gunnerMode = false; return; }
        float gx, gz;
        if (!gunPick(x, y, gunCamera(), gx, gz)) return;
        const e0net::ShipState& S = curShip();
        float dx = gx - S.x, dz = gz - S.z, d = std::sqrt(dx * dx + dz * dz);
        if (d > 0.01f) {   // limita ao alcance do canhão
            float dc = std::max(curDef().rangeMin, std::min(curDef().rangeMax, d));
            gx = S.x + dx / d * dc; gz = S.z + dz / d * dc;
            if (dc != d) { gunMsg = d > dc ? "ALCANCE MAXIMO" : "MUITO PERTO"; gunMsgT = 1.2f; }
        }
        gunTx = gx; gunTz = gz; gunHasTarget = true;
        if (!gunGranted) return;
        if (gunReload > 0.0f) { gunMsg = "RECARREGANDO..."; gunMsgT = 1.0f; return; }
        net.requestCannon(gx, gz);
        gunReload = e0net::kCannonReload;   // otimista; o servidor também limita
    }
    void updateGunner(float dt) {
        gunReload = std::max(0.0f, gunReload - dt); gunMsgT = std::max(0.0f, gunMsgT - dt); cannonClock += dt;
        const e0net::PlayerState me = net.predicted();
        if (gunnerMode && (me.ship != 1 || selfDead || isleMode || uiFull() || !curShip().exists || !curDef().hasCannons)) gunnerMode = false;
        if (gunnerMode && net.connected()) {
            if (me.status & e0net::StGunner) gunGranted = true;
            else if (gunGranted) gunnerMode = false;                                  // perdeu o lugar
            else if ((gunWait -= dt) <= 0.0f) { gunnerMode = false; notify("CANHOES OCUPADOS (2/2)"); }   // o servidor negou
        }
        gunBlend += ((gunnerMode ? 1.0f : 0.0f) - gunBlend) * (1.0f - std::exp(-dt * 6.0f));
        if (!gunnerMode && gunBlend < 0.003f) gunBlend = 0.0f;
        e0net::PlayerState mv = me; (void)mv;
        for (NetClient::CannonFire f; net.popCannonFire(f); ) {   // alguém atirou: a bala voa e sai fumaça do costado
            cballs.push_back({cannonClock, f.flight, {f.sx, f.sy, f.sz}, {f.tx, f.ty, f.tz}});
            renderer.burst({f.sx, f.sy, f.sz}, 2);
            if (net.connected() && f.shooter == net.myId()) gunReload = e0net::kCannonReload;
            float dd = std::hypot(f.sx - me.x, f.sz - me.z); if (dd < 60.0f) rig.addTrauma(0.12f * (1.0f - dd / 60.0f));
        }
        for (NetClient::CannonHit hi; net.popCannonHit(hi); ) {
            const bool onShip = hi.ship < e0net::kMaxShips;
            renderer.burst({hi.x, hi.y, hi.z}, onShip ? 0 : hi.kind);
            float dd = std::hypot(hi.x - me.x, hi.z - me.z); if (dd < 90.0f) rig.addTrauma(0.35f * (1.0f - dd / 90.0f));
            if (onShip) {   // a bala pegou o casco de um navio: fumaça + farpas no convés e aviso
                const e0net::ShipDef& HD = e0net::shipDef(hi.ship);
                renderer.burst({hi.x, HD.deckY + 0.8f, hi.z}, 2); renderer.burst({hi.x, HD.deckY + 1.6f, hi.z}, 2);
                char t[64];
                if (me.ship && me.shipIdx == hi.ship) { std::snprintf(t, sizeof t, "%s", "NAVIO ATINGIDO!"); rig.addTrauma(0.25f); }
                else if (net.connected() && hi.from == net.myId()) std::snprintf(t, sizeof t, "ACERTOU O %s!", HD.name);
                else if (dd < 200.0f) std::snprintf(t, sizeof t, "%s ATINGIDO (%d/%d)", HD.name, (int)hi.hp, HD.maxHp);
                else t[0] = 0;
                if (t[0]) notify(t);
            }
        }
        for (int i = 0; i < e0net::kMaxShips; ++i) {   // naufrágio / renascimento (vem do snapshot): avisa todo mundo
            const e0net::ShipState& S = e0net::shipState(i);
            const int st = !S.exists ? 0 : (S.sinkT > 0.0f ? 2 : 1);
            if (shipLifeSt[i] >= 0 && st != shipLifeSt[i]) {
                char t[64]; t[0] = 0;
                if (st == 2) std::snprintf(t, sizeof t, "%s AFUNDOU!", e0net::shipDef(i).name);
                else if (st == 0 && shipLifeSt[i] == 1) std::snprintf(t, sizeof t, "%s SUMIU: NINGU\xC9M ESTAVA USANDO", e0net::shipDef(i).name);   // LIMPEZA (chegar por chamado já avisa "CHEGOU")
                if (t[0]) notify(t);
                if (st == 2 && std::hypot(S.x - me.x, S.z - me.z) < 120.0f) rig.addTrauma(0.4f);
            }
            shipLifeSt[i] = st;
        }
        for (size_t i = 0; i < cballs.size();) { if (cannonClock - cballs[i].t0 > cballs[i].flight + 0.05f) cballs.erase(cballs.begin() + (long)i); else ++i; }
    }
    void addCannonballs(std::vector<DropVis>& dv) const {
        for (const CBall& b : cballs) {
            if (cannonClock < b.t0) continue;   // bordada do modo cinema: a bala ainda não saiu
            float k = std::min(1.0f, (cannonClock - b.t0) / std::max(0.1f, b.flight));
            float dist = std::hypot(b.to.x - b.from.x, b.to.z - b.from.z), apex = std::min(45.0f, 0.28f * dist + 4.0f);
            Vec3 p{b.from.x + (b.to.x - b.from.x) * k, b.from.y + (b.to.y - b.from.y) * k + 4.0f * apex * k * (1.0f - k), b.from.z + (b.to.z - b.from.z) * k};
            dv.push_back({{p.x, p.y - 0.55f, p.z}, k * 9.0f, {0.3f, 0.3f, 0.3f}, {0.07f, 0.07f, 0.08f}, 2, 1u, 0, 4.0f});
        }
    }
    void updateShipUi() {
        shipCtx = 0; shipNearHelm = false;
        const e0net::PlayerState me = net.predicted();
        const int board = me.ship ? -1 : e0net::shipBoardTarget(me.x, me.y, me.z);
        if (me.ship) shipSel = me.shipIdx < e0net::kMaxShips ? me.shipIdx : 0;
        else if (board >= 0) shipSel = board;
        const e0net::ShipState& S = curShip(); const e0net::ShipDef& D = curDef();
        if (!S.exists || selfDead || isleMode || godFly || rescuingNow()) { shipThrPtr = shipRevPtr = -1; return; }
        if (me.ship == 2) shipCtx = 3;
        else if (me.ship == 1) {
            shipCtx = 2;
            float da = me.sa - D.helmA;
            shipNearHelm = !carryingNow() && std::sqrt(da * da + me.sb * me.sb) <= D.helmReach && std::fabs(me.y - D.terraceY) < 0.4f && (S.helmer < 0 || S.helmer == me.id) && (S.owner < 0 || S.owner == me.id);
        } else if (board >= 0) shipCtx = 1;
        if (shipCtx != 3) shipThrPtr = shipRevPtr = -1;
        shipSpeedShown = S.speed;
    }
    void updateChest(float dt) {
        const e0net::ShipState& S = curShip(); const e0net::ShipDef& D = curDef();
        const e0net::PlayerState me = net.predicted();
        chestNear = shipCtx == 2 && (S.owner < 0 || S.owner == me.id) && std::fabs(me.y - D.deckY) < 0.6f && e0net::shipChestDist(D, me.sa, me.sb) <= D.chestReach;
        if (!chestNear) chestOpen = false;   // se afastou: a tampa fecha sozinha
        chestMsgT = std::max(0.0f, chestMsgT - dt);
        if (chestUi && (!chestNear || selfDead || isleMode || invOpen || craftOpen || mapOpen || shopOpen)) closeChestUi();
        float step = dt / 0.35f;
        chestOpenT = chestOpen ? std::min(1.0f, chestOpenT + step) : std::max(0.0f, chestOpenT - step);
        if (S.exists) {
            float wx, wz; e0net::shipLocalToWorld(S, D.chestA, D.chestB, wx, wz);
            chestPos = {wx, D.deckY + 0.62f * D.chestHalfB / e0net::kChestHalfB, wz};
        }
    }
    // botões do navio (HUD): 0 = principal (EMBARCAR / DESCER / SOLTAR LEME) acima do botão de ação; 2 = ACELERAR (no lugar do correr); 3 = RE (no lugar da ação).
    // LEME, CANHAO e ABRIR (baú) são BOTÕES NO CENÁRIO (WorldBtn, abaixo): ficam presos ao timão / amurada / baú, projetados na tela.
    bool chestBtnOn() const { return chestNear && rescueId < 0 && !carryingNow(); }
    float shipBtnX(int k) const { return k == 2 ? runX() : (k == 3 ? actionX() : gridX(0)); }   // 0 = acima do CORRER
    float shipBtnY(int k) const { return k == 2 ? runY() : (k == 3 ? actionY() : gridY(1)); }
    int shipBtnList(int* ks) const { ks[0] = 0; return shipCtx != 0 ? 1 : 0; }   // botões HUD visíveis do navio (índices de shipBtnX/Y)
    int shipBtnAt(float x, float y) const {
        if (shipCtx == 0 || isleMode || uiFull()) return -1;
        auto in = [&](int k) { float dx = x - shipBtnX(k), dy = y - shipBtnY(k), r = actionR() * (k == 2 ? 1.35f : 1.15f); return dx * dx + dy * dy <= r * r; };
        if (in(0)) return 0;
        if (shipCtx == 3 && (in(2) || in(3))) return in(2) ? 2 : 3;
        return -1;
    }
    void shipBtnPress(int k, int pid) {
        if (k == 0) shipEvQueued |= shipCtx == 1 ? e0net::ShipEvBoard : (shipCtx == 2 ? e0net::ShipEvLeave : e0net::ShipEvHelm);
        else if (k == 2) shipThrPtr = pid;
        else if (k == 3) shipRevPtr = pid;
    }
    const char* shipBtnLabel(int k) const {
        if (k == 0) return shipCtx == 1 ? "EMBARCAR" : (shipCtx == 2 ? "DESCER" : "SOLTAR");
        return k == 2 ? "ACELERAR" : "RE";
    }
    // ---- Botões no cenário (mundo 3D): LEME no timão, CANHAO na amurada mais perto, ABRIR no baú ----
    // Desenhados como as etiquetas do mundo (tamanho constante na tela, projetados com project()), com cara de botão (disco + anel + ícone + rótulo).
    enum { WbHelm = 0, WbCannon, WbChest, WbCount };
    struct WorldBtn { float alpha = 0.0f; bool on = false, line = false; Vec3 base{}, top{}; };
    WorldBtn wbtn[WbCount];
    bool cannonBtnOn = false; Vec3 cannonPos{};
    float worldBtnR() const { return actionR() * 0.62f; }
    static const char* worldBtnLabel(int k) { return k == WbHelm ? "LEME" : (k == WbCannon ? "CANHAO" : "ABRIR"); }
    static int worldBtnIcon(int k) { return k == WbHelm ? HiWheel : (k == WbCannon ? HiCannon : HiChest); }
    void updateWorldBtns(float dt) {
        const e0net::PlayerState me = net.predicted();
        const e0net::ShipState& S = curShip(); const e0net::ShipDef& D = curDef();
        const bool ok = shipCtx == 2 && S.exists && !gunnerMode && !isleMode && !uiFull();
        // CANHAO: na amurada do lado em que estou, na altura (a) em que estou (limitado ao trecho dos canhões); só no convés principal
        cannonBtnOn = false;
        if (ok && !shipNearHelm && D.hasCannons && !carryingNow() && std::fabs(me.y - D.deckY) < 0.6f) {
            float a = std::max(std::max(D.aMin, -D.muzzleA), std::min(std::min(D.aMax, D.muzzleA), me.sa));
            float side = me.sb >= D.midB ? 1.0f : -1.0f, b = D.midB + side * (e0net::shipDeckHalfW(D, a) + 0.3f);
            float da = me.sa - a, db = me.sb - b;
            if (std::sqrt(da * da + db * db) <= 3.6f) {
                cannonBtnOn = true;
                float wx, wz; e0net::shipLocalToWorld(S, a, b, wx, wz);
                cannonPos = {wx, D.deckY + 0.95f, wz};
            }
        }
        bool on[WbCount] = {ok && shipNearHelm, cannonBtnOn, ok && chestBtnOn()};
        if (S.exists) {
            float wx, wz; e0net::shipLocalToWorld(S, D.wheelA, -0.05f, wx, wz);   // mesmo ponto do timão de Renderer::buildWheel
            Vec3 wheel{wx, D.wheelY + 0.95f * D.wheelScale, wz};   // logo acima do aro
            wbtn[WbHelm].base = wbtn[WbHelm].top = wheel; wbtn[WbHelm].line = false;
            wbtn[WbChest].base = chestPos; wbtn[WbChest].line = true;
            wbtn[WbChest].top = {chestPos.x, std::max(me.y + 1.45f, chestPos.y + 0.6f), chestPos.z};
            if (cannonBtnOn) {
                wbtn[WbCannon].base = cannonPos; wbtn[WbCannon].line = true;
                wbtn[WbCannon].top = {cannonPos.x, std::max(me.y + 1.45f, cannonPos.y + 0.6f), cannonPos.z};
            }
        }
        for (int k = 0; k < WbCount; ++k) {
            WorldBtn& b = wbtn[k]; b.on = on[k];
            float target = b.on ? 1.0f : 0.0f;
            b.alpha += (target - b.alpha) * (1.0f - std::exp(-dt * 12.0f));
            if (target == 0.0f && b.alpha < 0.004f) b.alpha = 0.0f;
        }
    }
    int worldBtnAt(float x, float y) const {   // botão do cenário sob o toque (só os ativos = dentro do alcance)
        if (isleMode || uiFull() || gunnerMode) return -1;
        const float r = worldBtnR() * 1.3f;
        for (int k = 0; k < WbCount; ++k) {
            if (!wbtn[k].on || wbtn[k].alpha < 0.3f) continue;
            float px, py; if (!project(wbtn[k].top, px, py)) continue;
            float dx = x - px, dy = y - py; if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }
    void worldBtnPress(int k) {
        updateShipUi();   // revalida o alcance no momento do toque
        if (k == WbHelm) { if (shipCtx == 2 && shipNearHelm) shipEvQueued |= e0net::ShipEvHelm; }
        else if (k == WbCannon) {
            if (!cannonBtnOn || shipCtx != 2 || !curDef().hasCannons || carryingNow()) return;
            int n = 0; for (const auto& r : remotes) if ((r.status & e0net::StGunner) && r.shipIdx == shipSel) ++n;
            if (net.connected() && n >= e0net::kMaxGunners) { notify("CANHOES OCUPADOS (2/2)"); }
            else { gunnerMode = true; gunHasTarget = false; gunGranted = !net.connected(); gunWait = 1.2f; }
        } else if (k == WbChest) { if (shipCtx == 2 && chestBtnOn()) openChestUi(); }
    }

    // ---- Salvar / carregar jogadores caídos ----
    int rescueId = -1;                 // jogador caído mais perto (alcance kRescueMaxDist), -1 = nenhum
    float rescueProg = 0.0f;           // s de RCP que EU estou fazendo (barra "SALVANDO")
    bool carryingNow() const { return (net.predicted().status & e0net::StCarrying) != 0; }
    bool actionLocked() const { return rescuingNow() || localAnim.gettingUp(); }   // salvando alguém ou levantando do chão: sem andar/correr/pular/bater
    bool rescuingNow() const { return (net.predicted().status & e0net::StRescuing) != 0; }
    void updateRescueUi(float dt) {
        rescueId = -1;
        const e0net::PlayerState me = net.predicted();
        if (!selfDead && !e0net::isSwimmingPos(me.x, me.y, me.z) && !carryingNow() && !rescuingNow()) {
            float best = 1e9f;
            for (const auto& p : remotes) {
                if (!(p.status & e0net::StDead) || (p.status & e0net::StCarried)) continue;
                float dx = p.x - me.x, dz = p.z - me.z, d = std::sqrt(dx * dx + dz * dz);
                if (d <= e0net::kRescueMaxDist && std::fabs(p.y - me.y) < 1.6f && d < best) { best = d; rescueId = p.id; }
            }
        }
        rescueProg = rescuingNow() ? rescueProg + dt : 0.0f;
    }
    bool rescueBtnVisible() const { return false; }   // SALVAR/CARREGAR/SOLTAR saíram da HUD: agora é o MENU NO MUNDO (segurar no jogador)
    // ---- MENU DE RESGATE NO MUNDO 3D: segurar (~0,45 s) o dedo no jogador caído mais perto (ou em quem eu carrego) abre uma lista CLICÁVEL sobre a cabeça dele ----
    bool rmOpen = false; int rmId = -1; float rmAlpha = 0.0f;
    int teamTouchPtr = -1;   // dedo que está usando o painel da TRIPULAÇÃO no jogo
    int rmHoldPtr = -1; float rmHoldT = 0.0f, rmHoldX0 = 0, rmHoldY0 = 0, rmHoldX = 0, rmHoldY = 0;
    static constexpr float kRmHoldSec = 0.45f;
    // ---- MENU DE SEGURAR NO JOGADOR (estilo "interação"): segurar o dedo ~0,45 s no corpo de qualquer jogador perto abre, AO LADO do dedo, um olho
    // e uma coluna de botões grandes. Vivo: CONVIDAR PARA TRIPULAÇÃO / CHAT DA TRIPULAÇÃO (se for tripulante) e DAR o item da mão. Caído: SALVAR e
    // CARREGAR. Carregando alguém: SOLTAR. O menu fica onde o dedo estava (rmAx/rmAy) e fecha se a pessoa se afastar.
    // kinds: 0 SALVAR, 1 CARREGAR, 2 SOLTAR, 3 CONVIDAR PARA TRIPULAÇÃO, 4 DAR ITEM DA MÃO, 5 CHAT DA TRIPULAÇÃO
    static constexpr float kRmRange = 4.0f;   // m: até onde o menu alcança um jogador vivo
    float rmAx = 0.0f, rmAy = 0.0f;
    bool rmBlocked() const { return selfDead || isleMode || uiFull() || gunnerMode || wheelOpen; }
    const e0net::PlayerState* rmRemote(int id) const { for (const auto& p : remotes) if (p.id == id) return &p; return nullptr; }
    int rmHeldSlot() const { return hotbarCur >= 0 && inv[kInvHotFirst + hotbarCur].id ? kInvHotFirst + hotbarCur : -1; }
    int rmItems(int id, int* out) const {
        if (id < 0 || rmBlocked()) return 0;
        if (carryingNow()) { const int l = (int)net.predicted().link; if (l > 0 && l - 1 == id) { out[0] = 2; return 1; } return 0; }
        const e0net::PlayerState* p = rmRemote(id);
        if (!p || (p->status & e0net::StCarried)) return 0;
        const e0net::PlayerState me = net.predicted();
        const float d = std::hypot(p->x - me.x, p->z - me.z);
        int n = 0;
        if (id == rescueId) { out[n++] = 0; out[n++] = 1; }
        if (!(p->status & e0net::StDead) && d <= kRmRange && std::fabs(p->y - me.y) < 2.5f && net.connected()) {
            out[n++] = crewMember(id) ? 5 : 3;
            if (rmHeldSlot() >= 0 && d <= e0net::kGiveMaxDist) out[n++] = 4;
        }
        return n;
    }
    int rmTargetId() const {   // dica "SEGURE": quem eu carrego, o caído mais perto ou o jogador vivo mais perto (a até 3 m)
        if (rmBlocked()) return -1;
        if (carryingNow()) { int l = (int)net.predicted().link; return l > 0 ? l - 1 : -1; }
        if (rescueId >= 0) return rescueId;
        const e0net::PlayerState me = net.predicted();
        int best = -1; float bd = 3.0f; int tmp[4];
        for (const auto& p : remotes) { const float d = std::hypot(p.x - me.x, p.z - me.z); if (d < bd && rmItems(p.id, tmp) > 0) { bd = d; best = p.id; } }
        return best;
    }
    int rmCount() const { int k[4]; return rmItems(rmId, k); }
    int rmKind(int i) const { int k[4]; const int n = rmItems(rmId, k); return i < n ? k[i] : -1; }
    std::string rmLabel(int i) const {   // '\n' = 2 linhas
        switch (rmKind(i)) {
            case 0: return "SALVAR";
            case 1: return "CARREGAR";
            case 2: return "SOLTAR";
            case 3: return "CONVIDAR PARA\nTRIPULA\xC7\xC3" "O";
            case 5: return "CHAT DA\nTRIPULA\xC7\xC3" "O";
            case 4: {
                const int sl = rmHeldSlot(); if (sl < 0) return "DAR ITEM";
                char t[48]; if (inv[sl].n > 1) std::snprintf(t, sizeof t, "DAR %s\n(X%d)", itemDef(inv[sl].id).name, (int)inv[sl].n); else std::snprintf(t, sizeof t, "DAR %s", itemDef(inv[sl].id).name);
                return t;
            }
            default: return "";
        }
    }
    bool rmTagPos(int id, Vec3& out) const { for (const auto& t : tags) if (!t.self && t.id == id) { out = t.pos; return true; } return false; }
    // capsula na tela (pés -> cabeça) do jogador: o dedo precisa estar em cima do corpo
    bool rmHitBody(int id, float x, float y) const {
        Vec3 head, foot;
        if (!rmTagPos(id, head)) return false;
        bool found = false;
        for (const auto& p : remotes) if (p.id == id) { foot = {p.x, p.y, p.z}; found = true; break; }
        if (!found) return false;
        float hx, hy, fx, fy;
        if (!project(head, hx, hy) || !project(foot, fx, fy)) return false;
        float vx = hx - fx, vy = hy - fy, len2 = vx * vx + vy * vy;
        float t = len2 > 1e-3f ? ((x - fx) * vx + (y - fy) * vy) / len2 : 0.0f; t = std::max(0.0f, std::min(1.0f, t));
        float cx = fx + vx * t - x, cy = fy + vy * t - y;
        const float r = std::max(std::sqrt(len2) * 0.6f, 20.0f * (float)uiScale());
        return cx * cx + cy * cy <= r * r;
    }
    int rmHoldId = -1;
    void rmHoldBegin(int pid, float x, float y) {   // o dedo precisa estar no CORPO de um jogador com opções (o mais perto, se houver vários)
        if (rmHoldPtr >= 0 || rmOpen) return;
        const e0net::PlayerState me = net.predicted();
        int best = -1; float bd = 1e9f; int tmp[4];
        for (const auto& p : remotes) {
            if (rmItems(p.id, tmp) == 0 || !rmHitBody(p.id, x, y)) continue;
            const float d = std::hypot(p.x - me.x, p.z - me.z);
            if (d < bd) { bd = d; best = p.id; }
        }
        if (best < 0) return;
        rmHoldId = best; rmHoldPtr = pid; rmHoldT = 0.0f; rmHoldX0 = rmHoldX = x; rmHoldY0 = rmHoldY = y;
    }
    void updateRescueMenu(float dt) {
        int tmp[4];
        if (rmOpen && rmItems(rmId, tmp) == 0) rmOpen = false;
        if (rmHoldPtr >= 0) {
            float dx = rmHoldX - rmHoldX0, dy = rmHoldY - rmHoldY0;
            const int tid = rmHoldId;
            if (rmItems(tid, tmp) == 0 || dx * dx + dy * dy > std::pow(22.0f * (float)uiScale() / 3.0f, 2.0f)) rmHoldPtr = -1;   // mexeu o dedo / sem alvo: era joystick ou câmera
            else if ((rmHoldT += dt) >= kRmHoldSec) {
                rmOpen = true; rmId = tid; rmAx = rmHoldX0; rmAy = rmHoldY0;
                if (joyT.id == rmHoldPtr) joyT.id = -1;
                if (lookT.id == rmHoldPtr) lookT.id = -1;
                rmHoldPtr = -1;
            }
        }
        rmAlpha += ((rmOpen ? 1.0f : 0.0f) - rmAlpha) * (1.0f - std::exp(-dt * 14.0f));
        if (!rmOpen && rmAlpha < 0.004f) rmAlpha = 0.0f;
    }
    float rmU() const { return std::min((float)h / 1080.0f, (float)w / 1900.0f); }
    float rmBtnH(int i) const { return (rmLabel(i).find('\n') != std::string::npos ? 128.0f : 92.0f) * rmU(); }
    bool rmItemRect(int k, Rect2D& r) const {   // coluna de botões à direita do dedo (à esquerda se não couber), centrada na altura do dedo
        const float u = rmU(), bw = 470.0f * u, gap = 14.0f * u;
        const int n = rmCount();
        if (k < 0 || k >= n) return false;
        float total = 0.0f; for (int i = 0; i < n; ++i) total += rmBtnH(i) + (i ? gap : 0.0f);
        float x0 = rmAx + 74.0f * u;
        if (x0 + bw > (float)w - 20.0f * u) x0 = rmAx - 74.0f * u - bw;
        float y = std::max(20.0f * u, std::min((float)h - 20.0f * u - total, rmAy - total * 0.5f));
        for (int i = 0; i < k; ++i) y += rmBtnH(i) + gap;
        r = {x0, y, bw, rmBtnH(k)};
        return true;
    }
    bool rmTouchDown(float x, float y) {   // true = o toque foi num item (consumido); fora da lista só fecha
        if (!rmOpen) return false;
        for (int k = 0; k < rmCount(); ++k) {
            Rect2D r; if (!rmItemRect(k, r) || !r.contains(x, y)) continue;
            const int id = rmId, kind = rmKind(k); rmOpen = false;
            if (kind == 2) net.requestAct(e0net::ActDropCarry, 0, 0);
            else if (kind == 3) {   // convite: diz o motivo se não dá
                const NetClient::TeamInfo& me = net.teamInfo(net.myId()), &t = net.teamInfo(id);
                if (!me.known || me.team == 0) notify("VOCE NAO TEM TRIPULACAO: CRIE UMA NO PAINEL");
                else if (t.known && t.team != 0) notify("ELE JA TEM TRIPULACAO");
                else teamInvite(id);
            } else if (kind == 5) openTeam();
            else if (kind == 4) {   // DAR o item da mão para ESTA pessoa
                const int sl = rmHeldSlot();
                if (sl >= 0) { net.requestAct(e0net::ActGiveTo, (uint16_t)((sl & 31) | (id << 5)), inv[sl].n); inv[sl] = {}; }   // some na hora (o servidor confirma ou o inventário dele volta)
            } else if (id >= 0 && kind >= 0) net.requestAct(kind == 0 ? e0net::ActRevive : e0net::ActCarry, (uint16_t)id, 0);
            return true;
        }
        rmOpen = false;
        return false;
    }
    void drawRescueMenu() {   // passe de texto: só a dica "SEGURE" (o menu aberto é desenhado em drawHoldMenu, com formas arredondadas)
        if (rmOpen || rmAlpha > 0.0f) return;
        const int ts = std::max(2, uiScale() - 1);
        const int tid = rmTargetId();
        Vec3 tp; float px, py;
        if (tid >= 0 && rmTagPos(tid, tp) && project(tp, px, py)) {
            const char* hint = "SEGURE PARA OPCOES";
            const float tw = Renderer::textWidth(hint, ts);
            renderer.rect(px - tw * 0.5f - 3.0f * ts, py - 22.0f * ts, tw + 6.0f * ts, 11.0f * ts, 0, 0, 0, 0.55f);
            renderer.text(px - tw * 0.5f, py - 20.0f * ts, ts, hint, 1.0f, 0.9f, 0.55f, 0.95f);
        }
    }
    void drawHoldMenu() {   // menu aberto: olho no dedo + botões escuros arredondados com texto branco
        if (rmAlpha <= 0.0f) return;
        const float a = rmAlpha, u = rmU();
        const int n = rmCount();
        if (n <= 0) return;
        renderer.beginUi();
        renderer.uiGlow(rmAx, rmAy, 60.0f * u, 60.0f * u, 0.0f, 0.0f, 0.0f, 0.35f * a);
        renderer.uiIconTint(HiFlEye, rmAx, rmAy, 34.0f * u, 1.0f, 1.0f, 1.0f, 0.95f * a);
        for (int k = 0; k < n; ++k) {
            Rect2D r; if (!rmItemRect(k, r)) break;
            const int kind = rmKind(k);
            renderer.uiWoodPanel(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 12.0f * u, (kind == 3 || kind == 5) ? theme::PanelCardSel : theme::PanelCard, 0.96f * a, (float)k);
        }
        renderer.endUi();
        renderer.beginText();
        const int ts = std::max(2, (int)std::lround(4.0f * u));
        for (int k = 0; k < n; ++k) {
            Rect2D r; if (!rmItemRect(k, r)) break;
            const std::string lb = rmLabel(k);
            const size_t nl = lb.find('\n');
            const std::string l1 = lb.substr(0, nl), l2 = nl == std::string::npos ? "" : lb.substr(nl + 1);
            const float lh = 7.0f * (float)ts, gap = 14.0f * u, th = l2.empty() ? lh : lh * 2.0f + gap;
            float y = r.y + (r.h - th) * 0.5f;
            for (const std::string& L : {l1, l2}) {
                if (L.empty()) continue;
                int t = ts; while (t > 2 && Renderer::textWidth(L.c_str(), t) > r.w - 40.0f * u) --t;
                renderer.text(r.x + (r.w - Renderer::textWidth(L.c_str(), t)) * 0.5f, y, t, L.c_str(), theme::kCream.r, theme::kCream.g, theme::kCream.b, a, false);
                y += lh + gap;
            }
        }
        renderer.endText();
    }
    // corpo CARREGADO: a pose OMBRO já está no espaço de quem carrega; fica presa ao tronco dele (skinning do osso Spine2) para balançar junto ao andar.
    // Usa a posição DESENHADA de quem carrega (se sou eu, a predita; senão a interpolada) para o corpo não atrasar.
    // ===================== BACAMARTE (arma de fogo; o servidor decide os acertos: ver MsgShoot/MsgShotFx em protocol.h) =====================
    // Botão de AÇÃO vira ATIRAR (o dedo que atira continua girando a câmera), botão MIRAR (alterna) à esquerda dele. 1 tiro por carga:
    // depois do tiro recarrega sozinho (kGunReload s, gasta 1 MUNICAO ao terminar). Mirando: câmera no ombro, anda devagar, chumbos juntos.
    // Sem mirar o tiro sai do quadril (o corpo vira para a mira na hora) com os chumbos bem abertos. TIRO NA CABEÇA = MORTE CERTA.
    bool armaLoaded = true, armaAim = false, armaKitGiven = false, armaFiredEvt = false, armaHitHead = false;
    float armaReloadT = -1.0f;      // >= 0: recarregando há tantos s
    float armaAfterShot = 0.0f;     // > 0: começa a recarregar sozinho quando zerar
    bool armaShotQueued = false;   // ATIRAR apertado durante a recarga: dispara assim que terminar
    float armaFireQ = -1.0f;        // >= 0: tiro pedido, esperando o corpo virar para a mira (no máx. esse tempo)
    float armaHoldAim = 0.0f;       // tiro do quadril: o corpo fica virado para a mira por um instante
    float armaHitT = 0.0f, armaShotT = 10.0f, armaWarnT = 0.0f;
    int armaPtr = -1, armaReloadSfx = 0;
    std::vector<uint16_t> armaMyShots;   // shotIds que eu disparei (o rastro deles já foi desenhado na hora)
    bool remoteFired[e0net::kMaxPlayers] = {};
    float remoteVX[e0net::kMaxPlayers] = {}, remoteVZ[e0net::kMaxPlayers] = {}, remotePX[e0net::kMaxPlayers] = {}, remotePZ[e0net::kMaxPlayers] = {};
    bool armaHeld() const { return heldTool() == e0net::HeldGun; }
    bool armaMode() const {
        if (!armaHeld() || isleMode || uiFull() || dying() || gunnerMode || shipCtx == 3 || carryingNow() || introActive || actionLocked()) return false;
        const e0net::PlayerState me = net.predicted();
        return !e0net::isSwimmingPos(me.x, me.y, me.z);
    }
    bool armaAimingNow() const { return armaMode() && ((armaAim && armaReloadT < 0.0f) || armaFireQ >= 0.0f || armaHoldAim > 0.0f); }
    float aimBtnX() const { return gridX(2); }
    float aimBtnY() const { return gridY(0); }
    bool inAimButton(float x, float y) const {
        if (!armaMode() || rescueBtnVisible()) return false;
        float dx = x - aimBtnX(), dy = y - aimBtnY(), r = actionR() * 1.15f;
        return dx * dx + dy * dy <= r * r;
    }
    void armaStartReload() {
        if (armaLoaded || armaReloadT >= 0.0f) return;
        if (invCount(ItAmmo) <= 0) { if (armaWarnT <= 0) { notify("SEM MUNICAO (FABRIQUE NO CRAFT)"); armaWarnT = 2.0f; } return; }
        armaReloadT = 0.0f; armaReloadSfx = 0;   // a mira volta sozinha ao terminar (MIRAR continua ligado)
    }
    void armaTrigger() {   // toque no ATIRAR
        if (armaReloadT >= 0.0f) { armaShotQueued = true; if (armaWarnT <= 0) { notify("RECARREGANDO... ATIRA LOGO EM SEGUIDA"); armaWarnT = 1.2f; } return; }
        if (!armaLoaded) {
            audio.playClick(0.7f, 0.75f, 0.0f);   // gatilho em seco
            if (invCount(ItAmmo) > 0) { armaStartReload(); armaShotQueued = true; } else if (armaWarnT <= 0) { notify("SEM MUNICAO (FABRIQUE NO CRAFT)"); armaWarnT = 2.0f; }
            return;
        }
        if (armaFireQ < 0.0f) armaFireQ = 0.22f;   // vira o corpo para a mira e dispara (ver updateArma)
    }
    // Ponto que a mira (centro da tela) está apontando: começa na altura do jogador (o que está entre a câmera e ele não conta)
    Vec3 armaAimPoint(const e0net::PlayerState& me) const {
        const Vec3 c = camera.position, f = camera.forward();
        const float t0 = std::max(0.0f, (me.x - c.x) * f.x + (me.y + 1.4f - c.y) * f.y + (me.z - c.z) * f.z);
        const Vec3 o = c + f * t0;
        uint8_t kind; float best = e0net::gunTraceWorld(o.x, o.y, o.z, f.x, f.y, f.z, e0net::kGunRange + 10.0f, kind);
        for (const auto& p : remotes) {
            if (p.status & (e0net::StDead | e0net::StCarried)) continue;
            float th = e0net::rayHitSphere(o.x, o.y, o.z, f.x, f.y, f.z, p.x, p.y + e0net::kHeadY, p.z, e0net::kHeadR);
            float tb = e0net::rayHitCapsule(o.x, o.y, o.z, f.x, f.y, f.z, p.x, p.y + e0net::kBodyY0, p.y + e0net::kBodyY1, p.z, e0net::kBodyR);
            if (th >= 0 && th < best) best = th;
            if (tb >= 0 && tb < best) best = tb;
        }
        return o + f * best;
    }
    // O centro da mira está sobre um jogador vivo (visível: nenhum obstáculo antes dele)?
    // Barato: testa primeiro os jogadores (esfera/cápsula) e só marcha no mundo até o MAIS PERTO acertado (sem jogador na mira = 0
    // passos; antes eram ~350 passos até 70 m, 2x por quadro). Resultado guardado por quadro (câmera + nº de remotos iguais).
    mutable Vec3 armaOvC{1e9f, 0, 0}, armaOvF{0, 0, 0};
    mutable size_t armaOvN = (size_t)-1;
    mutable bool armaOvR = false;
    bool armaOverPlayer() const {
        const Vec3 c = camera.position, f = camera.forward();
        if (c.x == armaOvC.x && c.y == armaOvC.y && c.z == armaOvC.z && f.x == armaOvF.x && f.y == armaOvF.y && f.z == armaOvF.z && remotes.size() == armaOvN)
            return armaOvR;
        armaOvC = c; armaOvF = f; armaOvN = remotes.size(); armaOvR = false;
        const e0net::PlayerState me = net.visual();
        const float t0 = std::max(0.0f, (me.x - c.x) * f.x + (me.y + 1.4f - c.y) * f.y + (me.z - c.z) * f.z);
        const Vec3 o = c + f * t0;
        float best = -1.0f;
        for (const auto& p : remotes) {
            if (p.status & (e0net::StDead | e0net::StCarried)) continue;
            const float th = e0net::rayHitSphere(o.x, o.y, o.z, f.x, f.y, f.z, p.x, p.y + e0net::kHeadY, p.z, e0net::kHeadR);
            const float tb = e0net::rayHitCapsule(o.x, o.y, o.z, f.x, f.y, f.z, p.x, p.y + e0net::kBodyY0, p.y + e0net::kBodyY1, p.z, e0net::kBodyR);
            if (th >= 0 && th <= e0net::kGunRange && (best < 0 || th < best)) best = th;
            if (tb >= 0 && tb <= e0net::kGunRange && (best < 0 || tb < best)) best = tb;
        }
        if (best < 0) return false;
        uint8_t kind;
        armaOvR = e0net::gunTraceWorld(o.x, o.y, o.z, f.x, f.y, f.z, best, kind) >= best;   // nada antes do jogador
        return armaOvR;
    }
    void armaFire(const e0net::PlayerState& me) {
        CharInst lc{{me.x, me.y, me.z}, me.yaw, {1, 1, 1}, false, localAnim.boneRows(), 0.0f, 0};
        lc.attachRows = localAnim.attachRows();
        Vec3 mz, mdir; Renderer::gunMuzzle(lc, mz, mdir);
        const Vec3 T = armaAimPoint(me), f = camera.forward();
        Vec3 d = T - mz; float dl = std::sqrt(dot(d, d));
        d = dl > 0.5f ? d * (1.0f / dl) : f;
        if (dot(d, f) < 0.25f) d = f;   // alvo atrás da boca do cano (colado na parede): vai reto na mira
        const bool aimed = armaAim && rig.aimAmount() > 0.6f;
        const float o[3] = {mz.x, mz.y, mz.z}, dd[3] = {d.x, d.y, d.z};
        const uint16_t id = net.requestShoot(o, dd, aimed);
        armaMyShots.push_back(id); if (armaMyShots.size() > 16) armaMyShots.erase(armaMyShots.begin());
        // efeitos na hora (sem esperar o servidor): clarão, fumaça, som, coice e os rastros previstos (o acerto vem do servidor)
        renderer.gunShot(mz, d);
        audio.playGun(1.0f, 0.0f, 0.0f);
        rig.addTrauma(aimed ? 0.22f : 0.32f);
        rig.kick(aimed ? 0.050f : 0.035f, ((float)(punchRng % 100) / 100.0f - 0.5f) * 0.02f);
        punchRng = punchRng * 1664525u + 1013904223u;
        if (net.connected()) {
            float rx = -d.z, rz = d.x, rl = std::sqrt(rx * rx + rz * rz); if (rl < 1e-4f) { rx = 1; rz = 0; rl = 1; } rx /= rl; rz /= rl;
            const float ux = -d.y * rz, uy = rz * d.x - rx * d.z, uz = d.y * rx, spread = aimed ? e0net::kGunSpreadAim : e0net::kGunSpreadHip;
            for (int k = 0; k < e0net::kGunPellets; ++k) {
                punchRng = punchRng * 1664525u + 1013904223u; const float a = (float)(punchRng >> 8 & 0xFFFF) / 65535.0f * 6.2831853f;
                punchRng = punchRng * 1664525u + 1013904223u; const float rr = (k == 0 && aimed) ? 0.0f : spread * std::sqrt((float)(punchRng >> 8 & 0xFFFF) / 65535.0f);
                Vec3 pd{d.x + rx * std::cos(a) * rr + ux * std::sin(a) * rr, d.y + uy * std::sin(a) * rr, d.z + rz * std::cos(a) * rr + uz * std::sin(a) * rr};
                pd = normalize(pd);
                uint8_t kind; const float t = e0net::gunTraceWorld(mz.x, mz.y, mz.z, pd.x, pd.y, pd.z, e0net::kGunRange, kind);
                const Vec3 e = mz + pd * t;
                renderer.gunTracer(mz + pd * 0.6f, e);
                if (kind == e0net::ShotTerrain || kind == e0net::ShotWater) renderer.gunImpact(e, pd, kind);
            }
        }
        armaLoaded = false; armaAfterShot = 0.45f; armaFiredEvt = true; armaHoldAim = 0.7f; armaShotT = 0.0f;
        LOG("tiro %u (%s) alvo (%.1f %.1f %.1f)", (unsigned)id, aimed ? "mirando" : "quadril", T.x, T.y, T.z);
    }
    void updateArma(float dt, const e0net::PlayerState& me) {
        potSpeedT = std::max(0.0f, potSpeedT - dt); potShieldT = std::max(0.0f, potShieldT - dt);
        armaHitT = std::max(0.0f, armaHitT - dt); armaWarnT = std::max(0.0f, armaWarnT - dt); armaShotT += dt;
        armaHoldAim = std::max(0.0f, armaHoldAim - dt);
        if (invCount(ItGun) <= 0) { armaLoaded = false; armaReloadT = -1.0f; }
        const bool mode = armaMode();
        if (!mode) { armaFireQ = -1.0f; if (!armaHeld()) armaAim = false; }
        if (armaFireQ >= 0.0f) {   // espera o corpo virar para onde a câmera olha (rápido) e dispara
            armaFireQ -= dt;
            if (std::fabs(e0net::wrapPi(rig.yaw() - me.yaw)) < 0.30f || armaFireQ < 0.0f) { armaFireQ = -1.0f; if (armaLoaded && mode) armaFire(me); }
        }
        if (!armaLoaded && armaReloadT < 0.0f && armaAfterShot > 0.0f && (armaAfterShot -= dt) <= 0.0f && armaHeld()) armaStartReload();
        if (mode && actionPtr >= 0 && armaReloadT >= 0.0f) armaShotQueued = true;   // segurar ATIRAR durante a recarga também enfileira o tiro
        if (armaReloadT >= 0.0f) {
            if (!armaHeld() || dying()) { armaReloadT = -1.0f; armaShotQueued = false; }   // trocou de item: a recarga para (continua descarregada)
            else {
                armaReloadT += dt;
                static const float kSfxAt[4] = {0.25f, 0.62f, 1.55f, 1.95f};
                while (armaReloadSfx < 4 && armaReloadT >= kSfxAt[armaReloadSfx]) {
                    const int k = armaReloadSfx++;
                    if (k == 1) audio.playRamrod(0.75f, 0.0f); else audio.playClick(0.6f, k == 0 ? 0.8f : (k == 2 ? 1.15f : 0.9f), 0.15f);
                }
                if (armaReloadT >= e0net::kGunReload) {
                    armaReloadT = -1.0f;
                    if (invCount(ItAmmo) > 0) { invRemove(ItAmmo, 1); if (invServer()) net.requestAct(e0net::ActUse, 0, ItAmmo); armaLoaded = true; if (armaShotQueued && armaFireQ < 0.0f) armaFireQ = 0.22f; } else notify("SEM MUNICAO");
                    armaShotQueued = false;
                }
            }
        }
        // tiros que o servidor confirmou (de todos): rastros, clarão e som dos outros; sangue de quem foi atingido
        for (NetClient::ShotFx f; net.popShotFx(f); ) {
            bool mine = !net.connected() || (int)f.shooter == net.myId();
            if (mine && net.connected()) { mine = false; for (uint16_t id : armaMyShots) if (id == f.shotId) mine = true; }
            const Vec3 o{f.ox, f.oy, f.oz};
            if (!mine) {
                Vec3 d0 = f.n > 0 ? Vec3{f.p[0].x, f.p[0].y, f.p[0].z} - o : Vec3{0, 0, -1};
                float l0 = std::sqrt(dot(d0, d0)); d0 = l0 > 1e-3f ? d0 * (1.0f / l0) : Vec3{0, 0, -1};
                renderer.gunShot(o, d0);
                const float dist = std::sqrt((o.x - camera.position.x) * (o.x - camera.position.x) + (o.z - camera.position.z) * (o.z - camera.position.z));
                const Vec3 rgt = camera.rightVec(); const Vec3 to = o - camera.position;
                const float pan = std::max(-1.0f, std::min(1.0f, dot(to, rgt) / std::max(1.0f, dist)));
                if (dist < 400.0f) audio.playGun(std::max(0.08f, 1.0f - dist / 400.0f), pan * 0.8f, std::min(1.0f, dist / 120.0f));
                if (dist < 40.0f) rig.addTrauma(0.12f * (1.0f - dist / 40.0f));
                if (f.shooter < e0net::kMaxPlayers) remoteFired[f.shooter] = true;
            }
            for (int k = 0; k < f.n; ++k) {
                const Vec3 e{f.p[k].x, f.p[k].y, f.p[k].z};
                Vec3 pd = e - o; float pl = std::sqrt(dot(pd, pd)); pd = pl > 1e-3f ? pd * (1.0f / pl) : Vec3{0, 0, -1};
                const uint8_t kd = f.p[k].kind;
                if (!mine) { renderer.gunTracer(o + pd * 0.6f, e); if (kd != e0net::ShotNone) renderer.gunImpact(e, pd, kd); }
                else if (kd == e0net::ShotBody || kd == e0net::ShotHead) renderer.gunImpact(e, pd, kd);   // o meu: só o sangue (o rastro já foi)
                else if (!net.connected()) { renderer.gunTracer(o + pd * 0.6f, e); if (kd != e0net::ShotNone) renderer.gunImpact(e, pd, kd); }
            }
        }
    }

    static Mat4 rowsToMat(const Vec4* r) {
        Mat4 m;
        for (int i = 0; i < 3; ++i) { m.m[i] = (&r[i].x)[0]; m.m[4 + i] = (&r[i].x)[1]; m.m[8 + i] = (&r[i].x)[2]; m.m[12 + i] = (&r[i].x)[3]; }
        return m;
    }
    void attachCarried(CharInst& c, int carrierId, const e0net::PlayerState& me) {
        if (carrierId < 0) return;
        if (net.connected() && carrierId == net.myId()) { c.pos = {me.x, me.y, me.z}; c.yaw = me.yaw; c.local = rowsToMat(localAnim.attachRows() + 9); c.hasLocal = true; return; }
        for (const auto& q : remotes) if (q.id == carrierId) { c.pos = {q.x, q.y, q.z}; c.yaw = q.yaw; c.local = rowsToMat(remoteAnims[q.id % e0net::kMaxPlayers].attachRows() + 9); c.hasLocal = true; return; }
    }
    static bool isConsumable(int id) { return id == ItCoconut || (id >= ItPotHeal && id <= ItPotShield); }
    float potSpeedT = 0.0f, potShieldT = 0.0f;   // tempo restante dos efeitos das poções (HUD; o servidor é quem vale)
    // Esforço acelera a queda: parado 1x, correr 2x, nadar 2x (nado rápido 3x), golpes (bater/cortar/minerar) 3x por ~1,3 s a cada golpe.
    bool hungerSwimming = false; float hungerActionT = 0.0f;
    void updateHunger(float dt, bool running) {
        hungerActionT = std::max(0.0f, hungerActionT - dt);
        float mult = 1.0f;
        if (running) mult = std::max(mult, 2.0f);
        if (hungerSwimming) mult = std::max(mult, running ? 3.0f : 2.0f);
        if (hungerActionT > 0.0f) mult = std::max(mult, 3.0f);
        hunger = std::max(0.0f, hunger - kHungerDrainPerSec * mult * dt);
        if (hunger > 30.0f) { hungerWarnLow = false; hungerWarnZero = false; }
        if (hunger <= 25.0f && !hungerWarnLow) { hungerWarnLow = true; notify("COM FOME"); }
        if (hunger <= 0.0f && !hungerWarnZero) { hungerWarnZero = true; notify("FAMINTO! COMA UM COCO"); }
    }
    // Toque num slot JÁ selecionado: consumível = come 1 (desconta a quantidade); senão desseleciona (guarda o equipamento)
    void hotbarTapSelected() {
        ItemStack& st = inv[kInvHotFirst + hotbarCur];
        if (st.id >= ItPotHeal && st.id <= ItPotShield) {   // POÇÕES
            if (selfDead) return;
            if (st.id == ItPotHeal || st.id == ItPotFull) {
                if (health >= 99.5f) { notify("VIDA CHEIA"); return; }
                if (invServer()) net.requestAct(e0net::ActUse, (uint16_t)(kInvHotFirst + hotbarCur + 1), st.id);
                health = std::min(100.0f, health + (st.id == ItPotFull ? 100.0f : 40.0f));
                notify(st.id == ItPotFull ? "VIDA RESTAURADA" : "RECUPEROU 40 DE VIDA");
            } else {
                if (!net.connected()) { notify("POCAO SO FUNCIONA ONLINE"); return; }
                const bool sp = st.id == ItPotSpeed;
                net.requestAct(e0net::ActPotion, sp ? e0net::PotionSpeed : e0net::PotionShield, (uint16_t)(kInvHotFirst + hotbarCur + 1));
                if (sp) { potSpeedT = e0net::kSpeedPotionSec; notify("VELOCIDADE +50% POR 15 S"); }
                else { potShieldT = e0net::kShieldPotionSec; notify("INVULNERAVEL POR 8 S"); }
            }
            if (--st.n == 0) st = {};
            return;
        }
        if (st.id && isConsumable(st.id)) {
            if (hunger >= 99.5f) { notify("SEM FOME"); return; }
            if (invServer()) net.requestAct(e0net::ActUse, (uint16_t)(kInvHotFirst + hotbarCur + 1), st.id);
            hunger = std::min(100.0f, hunger + kCoconutFood);
            char t[40]; std::snprintf(t, sizeof t, "COMEU X1 %s", itemDef(st.id).name); notify(t);
            if (--st.n == 0) st = {};
            return;
        }
        hotbarCur = -1;
    }
    int hotbarCur = 0;   // slot selecionado atualmente (0 a 4); -1 = nenhum (mãos vazias, equipamento guardado)

    struct HotbarSlotGeom {
        float cx, cy;       // centro do slot em pixels
        float halfW, halfH; // meia largura e meia altura
        float radius;       // border radius
    };

    HotbarSlotGeom hotbarSlotGeom(int i) const {
        float sc = (float)h / 1080.0f;
        // Retangulares "mais altos que gordos" (proporção ~ 1 : 1.25)
        float slotW = std::round(120.0f * (0.85f + 0.15f * sc));
        float slotH = std::round(140.0f * (0.85f + 0.15f * sc));
        float gap = std::round(16.0f * (0.85f + 0.15f * sc));
        float radius = std::round(16.0f * (0.85f + 0.15f * sc)); // leves border radius
        float totalW = (float)kHotbarSlots * slotW + (float)(kHotbarSlots - 1) * gap;
        // Vão livre embaixo: da borda do joystick (posição de descanso) até a 3ª coluna da grade de botões (MIRAR/SALVAR). Centralizada na
        // tela se couber; senão no centro do vão; e, se nem assim couber (4:3), encolhe a barra inteira.
        const float freeL = joyHomeX() + joyR() + gap, freeR = gridX(2) - runR() - gap;
        float cxBar = w * 0.5f;
        if (freeR - freeL > 0.0f) {
            const float fit = std::min(1.0f, (freeR - freeL) / totalW);
            if (fit < 1.0f) { slotW = std::floor(slotW * fit); slotH = std::floor(slotH * fit); gap = std::floor(gap * fit); radius = std::floor(radius * fit); totalW = (float)kHotbarSlots * slotW + (float)(kHotbarSlots - 1) * gap; }
            if (cxBar - totalW * 0.5f < freeL || cxBar + totalW * 0.5f > freeR) cxBar = (freeL + freeR) * 0.5f;
        }
        float startX = cxBar - totalW * 0.5f;
        float slotX = startX + (float)i * (slotW + gap);
        float marginB = std::round(16.0f * (0.85f + 0.15f * sc));
        float slotY = h - marginB - slotH;
        return {slotX + slotW * 0.5f, slotY + slotH * 0.5f, slotW * 0.5f, slotH * 0.5f, radius};
    }

    Rect2D hotbarSlotRect(int i) const {
        HotbarSlotGeom g = hotbarSlotGeom(i);
        return {g.cx - g.halfW, g.cy - g.halfH, g.halfW * 2.0f, g.halfH * 2.0f};
    }

    int inHotbar(float x, float y) const {
        for (int i = 0; i < kHotbarSlots; ++i) {
            Rect2D r = hotbarSlotRect(i);
            if (x >= r.x - 4.0f && x <= r.x + r.w + 4.0f && y >= r.y - 4.0f && y <= r.y + r.h + 4.0f) {
                return i;
            }
        }
        return -1;
    }

    // ===================== INVENTÁRIO (só cliente por enquanto; sem coleta/replicação) =====================
    // Grade 5 colunas x 6 linhas = 30 slots. A linha 6 (slots 25..29) É a barra de acesso rápido (hotbar).
    struct ItemDef { const char* name; const char* d1; const char* d2; float r, g, b; uint16_t maxStack; float kg; };
    enum { ItNone = 0, ItStick, ItPebble, ItWood, ItStone, ItCoconut, ItFrond, ItHatchet, ItPickaxe, ItGold, ItDiamond, ItTorch, ItDagger, ItRope, ItCloth, ItIron, ItGun, ItAmmo, ItPotHeal, ItPotFull, ItPotSpeed, ItPotShield, ItCount };   // ids 9 e 10 = o servidor solta estes (ver ActHit); 11 = tocha (só kit/soltar)
    static const ItemDef& itemDef(int id) {
        static const ItemDef kDefs[ItCount] = {
            {"", "", "", 0, 0, 0, 1, 0.0f},
            {"GRAVETO",  "GALHO FINO.",           "BOM PARA FOGUEIRA.",     0.62f, 0.45f, 0.27f, 50, 0.05f},
            {"PEDRINHA", "PEDRA PEQUENA.",        "FERRAMENTAS SIMPLES.",   0.62f, 0.63f, 0.66f, 50, 0.10f},
            {"MADEIRA",  "TORA CORTADA.",         "CONSTRUCAO E COMBUSTIVEL.", 0.55f, 0.36f, 0.18f, 20, 1.50f},
            {"PEDRA",    "PEDRA MINERADA.",       "CONSTRUCAO E FERRAMENTAS.",  0.45f, 0.47f, 0.52f, 20, 2.00f},
            {"COCO",     "FRUTO DO COQUEIRO.",    "MATA FOME E SEDE.",      0.45f, 0.30f, 0.16f, 10, 0.60f},
            {"FOLHA",    "FOLHA DE PALMEIRA.",    "COBERTURA E CORDAS.",    0.25f, 0.62f, 0.28f, 30, 0.10f},
            {"MACHADINHA", "FERRAMENTA DE CORTE.",  "CORTA ARVORES. DANO 15 EM JOGADOR.", 0.56f, 0.55f, 0.55f, 1, 1.20f},
            {"PICARETA",   "FERRAMENTA DE MINERAR.", "QUEBRA PEDRAS. DANO 18 EM JOGADOR.", 0.52f, 0.52f, 0.54f, 1, 2.00f},
            {"OURO",       "MINERIO DE OURO.",       "VALIOSO. RARO.",           0.98f, 0.78f, 0.14f, 30, 0.40f},
            {"DIAMANTE",   "CRISTAL AZUL BRILHANTE.", "MUITO RARO E DURO.",       0.40f, 0.82f, 1.00f, 20, 0.25f},
            {"TOCHA",      "TOCHA DE MADEIRA E PANO.", "ILUMINA A NOITE. DANO 7 EM JOGADOR.", 0.98f, 0.55f, 0.14f, 1, 0.60f},
            {"ADAGA",      "ADAGA DE PEDRA.",        "AUTODEFESA. DANO 25 EM JOGADOR.", 0.50f, 0.52f, 0.54f, 1, 0.50f},
            {"CORDA",      "CORDA TRANCADA.",        "CRIADA NO CRAFT (3 FOLHAS).", 0.78f, 0.55f, 0.28f, 20, 0.20f},
            {"TECIDO",     "PANO RESISTENTE.",       "USADO NA TOCHA. VEM DO MAR (PRAIA).", 0.90f, 0.88f, 0.80f, 20, 0.10f},
            {"FERRO",      "MINERIO DE FERRO.",      "METAL COMUM. FERRAMENTAS FUTURAS.", 0.62f, 0.40f, 0.30f, 30, 0.80f},
            {"BACAMARTE",  "ARMA DE FOGO DE CHUMBO.", "1 TIRO POR CARGA. CABECA = MORTE.", 0.40f, 0.22f, 0.12f, 1, 3.50f},
            {"MUNICAO",    "POLVORA E CHUMBO.",      "1 CARGA POR TIRO DO BACAMARTE.", 0.32f, 0.32f, 0.36f, 30, 0.05f},
            {"POCAO DE CURA",   "RECUPERA 40 DE VIDA.",         "TOQUE NO SLOT SELECIONADO.", 0.92f, 0.25f, 0.30f, 10, 0.30f},
            {"POCAO DE VIDA",   "RECUPERA TODA A VIDA.",        "TOQUE NO SLOT SELECIONADO.", 1.00f, 0.45f, 0.75f, 10, 0.30f},
            {"POCAO DE VELOCIDADE", "+50% DE VELOCIDADE POR 15 S.", "TOQUE NO SLOT SELECIONADO.", 0.95f, 0.90f, 0.20f, 10, 0.30f},
            {"POCAO DE ESCUDO", "INVULNERAVEL POR 8 S.",        "TOQUE NO SLOT SELECIONADO.", 0.30f, 0.60f, 1.00f, 10, 0.30f},
        };
        return kDefs[id > 0 && id < ItCount ? id : 0];
    }
    using ItemStack = e0net::InvSlot;   // mesmo tipo do servidor (shared/inventory.h)
    static constexpr int kInvCols = 5, kInvRows = 6, kInvSlots = kInvCols * kInvRows;
    static constexpr int kInvHotFirst = kInvSlots - kHotbarSlots;   // 25
    static constexpr float kInvMaxKg = e0net::kInvMaxKg;
    static_assert(kInvSlots == e0net::kInvSlots && kInvHotFirst == e0net::kInvHotFirst, "inventario do cliente = do servidor");
    ItemStack inv[kInvSlots];
    bool invOpen = false, craftOpen = false; int craftSel = 0;

    // ---- MAPA e MINIMAPA (papel de pergaminho sujo, ilhas em preto; só uma área em volta do jogador; ver map_gen.h) ----
    // LISO: o papel é fixo, a tinta é gerada numa thread (MapWorker) e só é movida/ampliada pela GPU (UV), os marcadores são desenhados pela UI.
    // slots de textura: 0 papel mini, 1 tinta mini, 2 papel grande, 3 tinta grande.
    MapWorker mapWorker;
    struct InkTex { bool valid = false; float cx = 0, cz = 0, half = 1; float rx = 0, rz = 0; };   // rx/rz: centro da área revelada (a folha do mapa grande se centra aí)
    InkTex miniInk, bigInk;
    bool mapPaperAsked = false; float bigRevX = 1e9f, bigRevZ = 1e9f;
    bool mapOpen = false;
    float mapHalf = 1000.0f, mapPanX = 0, mapPanZ = 0;      // vista do mapa grande: meia-largura (m) e deslocamento do centro em relação ao jogador
    struct MapPt { int id = -1; float x = 0, y = 0, sx = 0, sy = 0, moved = 0; double t0 = 0; };
    bool mapMulti = false;                                   // 2 dedos tocaram nesse gesto: não vira "marcar rota"
    int wpShip = -1;                                          // ROTA que segue um navio chamado (NAVEGAÇÃO): tipo do navio, -1 = rota comum
    bool wpSet = false; float wpX = 0, wpZ = 0;              // ROTA marcada no mapa (só do cliente): label no mundo 3D + selo no minimapa
    // Carta náutica em tela cheia (estilo mesa de mapa Sea of Thieves / AC Black Flag)
    static constexpr float kMapGridStep = 250.0f;   // cada quadrante do grid náutico = 250 m x 250 m
    float mapPadOut() const { return 12.0f * std::min((float)h / 1080.0f, (float)w / 1900.0f); }
    float mapPadIn()  const { return 50.0f * std::min((float)h / 1080.0f, (float)w / 1900.0f); }
    Rect2D mapChartRect() const {
        const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
        return {22.0f * u, 20.0f * u, (float)w - 44.0f * u, (float)h - 40.0f * u};
    }
    Rect2D mapCloseRect() const {
        const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
        const float r = 34.0f * u;
        return {(float)w - 60.0f * u - r, 58.0f * u - r, r * 2.0f, r * 2.0f};
    }
    Rect2D mapZoomBtnRect(int k) const {   // k=0: "+", k=1: "-" (canto inferior direito interno)
        const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
        const Rect2D ch = mapChartRect();
        const float pad = mapPadIn() + 18.0f * u;
        const float totalW = 196.0f * u, gap = 10.0f * u, bw = (totalW - gap) * 0.5f, bh = 48.0f * u;
        const float x0 = ch.x + ch.w - pad - totalW;
        const float y0 = ch.y + ch.h - pad - bh;
        return {x0 + (float)k * (bw + gap), y0, bw, bh};
    }
    Rect2D mapCenterBtnRect() const {
        const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
        const Rect2D z0 = mapZoomBtnRect(0), z1 = mapZoomBtnRect(1);
        const float bh = 48.0f * u;
        return {z0.x, z0.y - bh - 10.0f * u, (z1.x + z1.w) - z0.x, bh};
    }
    Rect2D mapClearBtnRect() const {
        const float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
        const Rect2D c = mapCenterBtnRect();
        const float bh = 46.0f * u;
        return {c.x, c.y - bh - 10.0f * u, c.w, bh};
    }
    static void mapGridColLabel(int colIdx, char* out, size_t n) {
        if (colIdx >= 0 && colIdx < 26) { std::snprintf(out, n, "%c", 'A' + colIdx); return; }
        if (colIdx >= 26) { std::snprintf(out, n, "%c%c", 'A' + ((colIdx / 26) - 1) % 26, 'A' + (colIdx % 26)); return; }
        std::snprintf(out, n, "W%d", -colIdx);
    }
    static void mapGridCoord(float wx, float wz, char* out, size_t n) {
        int c = (int)std::floor(wx / kMapGridStep) + 13;
        int r = (int)std::floor(wz / kMapGridStep) + 13;
        char cs[8]; mapGridColLabel(c, cs, sizeof cs);
        std::snprintf(out, n, "%s-%d", cs, r);
    }
    MapPt mapPt[2];
    static constexpr int kMiniRes = 192, kBigRes = 768;
    static constexpr float kMiniHalf = 260.0f, kMiniView = 250.0f, kMiniTexHalf = 340.0f;
    static constexpr float kBigView = 1600.0f, kBigMinHalf = 220.0f, kBigMaxHalf = 1500.0f;
    float miniR() const { return 0.135f * (float)h; }
    float miniCX() const { return 10.0f * (float)uiScale() + miniR() * 1.08f; }
    float miniCY() const { return 9.0f * (float)uiScale() + miniR() * 1.08f; }
    bool inMini(float x, float y) const { float dx = x - miniCX(), dy = y - miniCY(), r = miniR(); return dx * dx + dy * dy <= r * r; }
    float mapSide() const { return std::max(1.0f, mapChartRect().h); }   // metros por pixel = 2*mapHalf / chart.h
    float mapHX() const { const Rect2D ch = mapChartRect(); return mapHalf * ch.w / std::max(1.0f, ch.h); }
    // posição de um ponto do mundo no mapa grande (px); a vista é centrada no centro da carta (ch.cx, ch.cy)
    void bigToScreen(float wx, float wz, float& sx, float& sy) const {
        const e0net::PlayerState me = net.visual();
        const Rect2D ch = mapChartRect();
        const float k = mapSide() / (2.0f * mapHalf);
        sx = ch.x + ch.w * 0.5f + (wx - (me.x + mapPanX)) * k;
        sy = ch.y + ch.h * 0.5f + (wz - (me.z + mapPanZ)) * k;
    }
    void clampMapView() {
        mapHalf = std::max(kBigMinHalf, std::min(kBigMaxHalf, mapHalf));
        float lim = kBigView * 0.85f, l = std::sqrt(mapPanX * mapPanX + mapPanZ * mapPanZ);   // não dá para arrastar o mapa para longe da área revelada
        if (l > lim) { mapPanX *= lim / l; mapPanZ *= lim / l; }
    }
    // chamado todo frame: barato. Pede camadas novas à thread (só quando a vista saiu do que a textura cobre) e sobe o que ficou pronto.
    void updateMap(float dt) {
        (void)dt;
        if (!net.worldReady() || selfDead) return;
        if (!mapPaperAsked) { mapPaperAsked = true; mapWorker.requestPaper(0, kMiniRes, true, false); mapWorker.requestPaper(2, 512, false, true); }
        MapView v; std::vector<uint8_t> px;
        for (int sl = 0; sl < 4; ++sl) if (mapWorker.take(sl, v, px)) {
            renderer.mapUpload(sl, v.res, px.data());
            if (sl == 1) miniInk = {true, v.cx, v.cz, v.half};
            if (sl == 3) bigInk = {true, v.cx, v.cz, v.half, bigRevX, bigRevZ};
        }
        const e0net::PlayerState me = net.visual();
        if (!mapOpen) {
            float dx = me.x - miniInk.cx, dz = me.z - miniInk.cz;
            if ((!miniInk.valid || std::max(std::fabs(dx), std::fabs(dz)) > 40.0f) && !mapWorker.busy(1)) {
                MapView mv; mv.cx = me.x; mv.cz = me.z; mv.half = kMiniTexHalf; mv.viewRadius = kMiniView + 60.0f; mv.res = kMiniRes;
                mapWorker.requestInk(1, mv, me.x, me.z, net.terrainSnapshot());   // o círculo revelado fica em volta de você; a margem de 60 m cobre o deslocamento entre pedidos
            }
        } else {
            float vcx = me.x + mapPanX, vcz = me.z + mapPanZ;
            float th = bigInk.half, hx = mapHX(), slack = th - hx;
            bool stale = !bigInk.valid || hx > th * 0.97f || hx < th / 2.4f || std::max(std::fabs(vcx - bigInk.cx), std::fabs(vcz - bigInk.cz)) > std::max(slack * 0.5f, 1.0f)
                         || std::hypot(me.x - bigRevX, me.z - bigRevZ) > 90.0f;
            if (stale && !mapWorker.busy(3)) {
                MapView mv; mv.cx = vcx; mv.cz = vcz; mv.half = std::min(3800.0f, mapHX() * 1.5f); mv.viewRadius = kBigView * 1.05f; mv.fadeFrac = 0.78f; mv.res = kBigRes;   // a tinta se apaga suave na borda da área explorada (a folha agora é a tela toda)
                mapWorker.requestInk(3, mv, me.x, me.z, net.terrainSnapshot()); bigRevX = me.x; bigRevZ = me.z;
            }
        }
    }
    void openMap() {
        mapOpen = true; mapPanX = mapPanZ = 0; mapHalf = 760.0f; mapPt[0].id = mapPt[1].id = -1; mapMulti = false; bigInk.valid = false;
        joyT.id = lookT.id = runPtr = collectPtr = actionPtr = -1; collectDown = false; input.moveX = input.moveY = 0;
    }
    bool mapOnBtn(float x, float y) const {
        return mapCloseRect().contains(x, y) || mapCenterBtnRect().contains(x, y) ||
               mapZoomBtnRect(0).contains(x, y) || mapZoomBtnRect(1).contains(x, y) ||
               (wpSet && mapClearBtnRect().contains(x, y));
    }
    // toques no mapa grande: 1 dedo = arrasta, 2 dedos = zoom (pinça) e arrasta; toque simples na carta = marcar rota
    void mapTouchDown(int pid, float x, float y) {
        if (mapCloseRect().contains(x, y) || invCloseRect().contains(x, y)) { mapOpen = false; return; }
        if (mapCenterBtnRect().contains(x, y)) { mapPanX = mapPanZ = 0; return; }
        if (mapZoomBtnRect(0).contains(x, y)) { mapHalf *= 0.72f; clampMapView(); return; }
        if (mapZoomBtnRect(1).contains(x, y)) { mapHalf *= 1.38f; clampMapView(); return; }
        if (wpSet && mapClearBtnRect().contains(x, y)) { wpSet = false; wpShip = -1; notify("ROTA REMOVIDA"); return; }
        if (!mapChartRect().contains(x, y)) return;
        double tn = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int act = 0; for (const MapPt& p : mapPt) if (p.id >= 0) ++act;
        if (act > 0) mapMulti = true;
        for (MapPt& p : mapPt) if (p.id < 0) { p = {pid, x, y, x, y, 0.0f, tn}; return; }
    }
    void mapTouchMove(int pid, float x, float y) {
        MapPt* me_ = nullptr; MapPt* other = nullptr;
        for (MapPt& p : mapPt) { if (p.id == pid) me_ = &p; else if (p.id >= 0) other = &p; }
        if (!me_) return;
        float k = 2.0f * mapHalf / mapSide();
        if (!other) { mapPanX -= (x - me_->x) * k; mapPanZ -= (y - me_->y) * k; }
        else {
            float ox = other->x, oy = other->y;
            float d0 = std::hypot(me_->x - ox, me_->y - oy), d1 = std::hypot(x - ox, y - oy);
            float mx0 = (me_->x + ox) * 0.5f, my0 = (me_->y + oy) * 0.5f, mx1 = (x + ox) * 0.5f, my1 = (y + oy) * 0.5f;
            mapPanX -= (mx1 - mx0) * k; mapPanZ -= (my1 - my0) * k;
            if (d0 > 8.0f && d1 > 8.0f) mapHalf *= d0 / d1;
        }
        me_->moved += std::fabs(x - me_->x) + std::fabs(y - me_->y);
        me_->x = x; me_->y = y;
        clampMapView();
    }
    void mapTouchUp(int pid) {
        for (MapPt& p : mapPt) if (p.id == pid) {
            double tn = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const Rect2D ch = mapChartRect();
            const float S = mapSide(), cx = ch.x + ch.w * 0.5f, cy = ch.y + ch.h * 0.5f;
            if (!mapMulti && p.moved < 0.025f * (float)h && tn - p.t0 < 0.6 && !mapOnBtn(p.sx, p.sy)
                && p.sx > ch.x + mapPadIn() && p.sx < ch.x + ch.w - mapPadIn() && p.sy > ch.y + mapPadIn() && p.sy < ch.y + ch.h - mapPadIn()) {
                const e0net::PlayerState me = net.visual(); float k = S / (2.0f * mapHalf);
                wpX = me.x + mapPanX + (p.sx - cx) / k; wpZ = me.z + mapPanZ + (p.sy - cy) / k; wpSet = true; wpShip = -1;
                notify("ROTA MARCADA");
            }
            p.id = -1;
        }
        bool any = false; for (const MapPt& p : mapPt) if (p.id >= 0) any = true;
        if (!any) mapMulti = false;
    }
    bool uiFull() const { return invOpen || craftOpen || mapOpen || chestUi || shopOpen || flagOpen || teamOpen; }   // inventário, craft, mapa, baú ou loja em tela cheia
    int invSel = -1;                                  // slot selecionado (detalhes)
    int invDragPtr = -1, invDragFrom = -1; float invDragX = 0, invDragY = 0, invDragX0 = 0, invDragY0 = 0; bool invDragMoved = false;

    // Cabe mais `n` do item? (pilhas incompletas ou slot vazio; peso máximo kInvMaxKg)
    bool invFits(int id, int n) const { return e0net::invFits(inv, id, n); }   // as MESMAS regras do servidor (shared/inventory.h)
    static int itemForObject(uint8_t objType) { return objType == e0net::ObjStick ? ItStick : (objType == e0net::ObjPebble ? ItPebble : (objType == e0net::ObjCloth ? ItCloth : ItNone)); }
    // Adiciona itens (completa pilhas, depois slots vazios). Devolve o que não coube. Usado pela coleta.
    int invAdd(int id, int n) { return e0net::invAdd(inv, id, n); }
    // Ferramenta na mão direita = item do slot selecionado da barra de acesso rápido (replicada aos outros jogadores via InputCmd/PlayerState)
    uint8_t heldTool() const {
        if (hotbarCur < 0) return e0net::HeldNone;
        uint8_t id = inv[kInvHotFirst + hotbarCur].id;
        return id == ItHatchet ? e0net::HeldHatchet : (id == ItPickaxe ? e0net::HeldPickaxe : (id == ItTorch ? e0net::HeldTorch : (id == ItDagger ? e0net::HeldDagger : (id == ItGun ? e0net::HeldGun : e0net::HeldNone))));
    }
#if E0_DEBUG_UI
    // Cheat do painel de debug (aba GOD): 1 unidade de cada item do jogo (ferramentas de pilha 1 só se ainda não tiver)
    void giveAllItems() {
        int added = 0;
        for (int id = 1; id < ItCount; ++id) {
            if (itemDef(id).maxStack == 1) { bool has = false; for (const auto& s : inv) if (s.id == id) has = true; if (has) continue; }
            if (invAdd(id, 1) == 0) ++added;
        }
        char t[40]; std::snprintf(t, sizeof t, "DEBUG: +%d ITENS", added); notify(t);
    }
#endif
    // Ferramentas guardadas no corpo foram removidas: sempre 0 (o campo `carried` do protocolo continua existindo)
    uint8_t carriedMask() const { return 0; }
    // Ícones da HUD (assets/icons.e0i, linhas depois das dos itens; tools/make_hud_icons.py; game-icons.net, CC BY 3.0). Mesma ordem do script.
    enum HudIcon { HiAxe = 12, HiPick, HiFist, HiGrab, HiBuild, HiBag, HiCraft, HiBoat, HiExit, HiWheel, HiCannon, HiChest, HiRescue, HiCarry,
                   HiFood, HiHeart, HiCoins, HiSpeed, HiBack, HiDrop, HiCloth, HiMap, HiRun, HiCamera,
                   HiShop, HiPirateHat, HiParrot, HiEyePatch, HiBandana, HiIdCard, HiGalleon, HiPirateFlag, HiSkull, HiCoinPile,   // LOJA
                   HiGhost, HiSabres, HiLockedChest, HiCoat, HiCaptain, HiCrown, HiCross, HiPlus, HiTwoCoins, HiGem,
                   HiGun, HiAim, HiShot, HiReload, HiPowder, HiClock, HiCompass, HiChat,   // BACAMARTE
                   // figuras da BANDEIRA (mesma ordem do fim da lista ICONS de tools/make_hud_icons.py)
                   HiFlAnchor, HiFlSkull2, HiFlCrownSkull, HiFlBones, HiFlSwords, HiFlOctopus, HiFlSerpent, HiFlShark, HiFlFin, HiFlRaven, HiFlHourglass, HiFlHeart,
                   HiFlRose, HiFlTrident, HiFlDragon, HiFlWolf, HiFlSnake, HiFlScorpion, HiFlSun, HiFlMoon, HiFlStar, HiFlBolt, HiFlLion, HiFlSkullCrack,
                   HiFlSeaDragon, HiFlJelly, HiFlShell, HiFlSunrise, HiFlStarSkull, HiFlFireTrident, HiFlCrowDive, HiFlBrokenHeart, HiFlSwordStone, HiFlWolfHowl,
                   HiFlSwirl, HiFlSeaStar, HiFlBlade, HiFlSkullBolt, HiFlEagle, HiFlClaws, HiFlEye, HiFlEvilMoon, HiFlScythe, HiFlReaper,
                   HiFlKrakenSkull, HiFlWhale, HiFlAngler, HiFlTurtle, HiFlCrab, HiFlSeahorse, HiFlMermaid, HiFlKey, HiFlAce, HiFlBat,
                   HiFlHydra, HiFlFireRing, HiFlHornSkull, HiFlDemon, HiFlTripleSkull, HiFlHarpoon, HiFlLighthouse, HiFlWreck, HiFlWave, HiFlWeb };   // atlas cheio: 512x4096
    struct HudIconReq { int idx; float x, y, half, r, g, b, a; };
    std::vector<HudIconReq> hudIcons;   // preenchida durante o passe de texto, desenhada depois dele (as caixas do texto cobririam)
    void hudIcon(int idx, float x, float y, float half, float r, float g, float b, float a) { hudIcons.push_back({idx, x, y, half, r, g, b, a}); }
    // Medalhão Fechar (X) rústico unificado: lacre carmesim + aro duplo de latão + X SDF nítido
    void closeBtnShapes(float cx, float cy, float r) {
        renderer.uiCloseBtn(cx, cy, r, 1.0f);
    }
    // ...ou na fila dos ícones da HUD (telas do passe de texto: inventário, craft, baú, mapa)
    void closeBtnQueue(const Rect2D& rc) {
        float cx = rc.x + rc.w * 0.5f, cy = rc.y + rc.h * 0.5f, r = rc.w * 0.5f;
        hudIcons.push_back({-1, cx, cy, r, 1.0f, 1.0f, 1.0f, 1.0f});
    }
    // Botão redondo: ícone em cima, rótulo embaixo (centro em (x,y), raio rr)
    // Botão redondo da HUD: SÓ o ícone, centralizado e grande (sem texto; `lab`/`tsc` ficam na assinatura por compatibilidade e são ignorados)
    void roundBtnLabel(int idx, float x, float y, float rr, const char* /*lab*/, int /*tsc*/, float cr, float cg, float cb, float ca) {
        hudIcon(idx, x, y, rr * 0.58f, cr, cg, cb, ca);
    }
    // FOME e VIDA: círculo com progresso circular e o ícone dentro (k 0 = fome, 1 = vida)
    float statR() const { return 15.0f * (float)uiScale(); }
    void statPos(int k, float& cx, float& cy) const {
        float s = (float)uiScale(), R = statR();   // canto superior DIREITO: VIDA na ponta, FOME à esquerda dela, MOEDAS à esquerda da fome
        cx = (float)w - 10.0f * s - R - (float)(1 - k) * (2.0f * R + 8.0f * s); cy = 9.0f * s + 9.0f * s;
    }
    // MOEDAS: círculo com o ícone (sem progresso) e, ao lado, a quantidade numa aba. cxC/cy = centro do círculo; aba vai de cxC até 'right'.
    static void coinText(char* out, size_t n, long long v) {   // 999 -> 999, 1500 -> 1.5K, 2300000 -> 2.3M, ... (nunca passa de 6 caracteres)
        if (v < 0) v = 0;
        static const char kU[] = {'K', 'M', 'B', 'T'};
        if (v < 1000) { std::snprintf(out, n, "%lld", v); return; }
        double d = (double)v; int u = -1;
        while (d >= 1000.0 && u < 3) { d /= 1000.0; ++u; }
        long long t10 = (long long)(d * 10.0);                       // trunca (não arredonda para cima)
        if (t10 >= 10000 && u < 3) { t10 /= 1000; ++u; }             // 999,95K -> 1.0M
        if (t10 % 10 == 0) std::snprintf(out, n, "%lld%c", t10 / 10, kU[u]);              // 23000 -> 23K
        else std::snprintf(out, n, "%lld.%lld%c", t10 / 10, t10 % 10, kU[u]);            // 23800 -> 23.8K (1 casa decimal, ate 999.9K)
    }
    void coinGeom(float& cxC, float& cy, float& right, float& tw) const {
        float s = (float)uiScale(), R = statR(), fx, fy; statPos(0, fx, fy);
        tw = Renderer::textWidth("0000000", uiScale());   // largura FIXA da aba (7 caracteres)
        right = fx - R - 8.0f * s; cxC = right - R - 12.0f * s - tw; cy = fy;
    }
    // RELÓGIO do jogo à esquerda das moedas (mesmo estilo). cycleSec 0 = 06:00 (nascer do sol), 450 = 12:00, 900 = 18:00, 1350 = 00:00.
    // ID do jogador ENTRE o relógio e as moedas (mesmo estilo)
    void idGeom(float& cxI, float& cy, float& right, float& tw) const {
        float s = (float)uiScale(), R = statR(), cxC, cyC, rc, tc; coinGeom(cxC, cyC, rc, tc);
        tw = Renderer::textWidth("000", uiScale());
        right = cxC - R - 8.0f * s; cxI = right - R - 12.0f * s - tw; cy = cyC;
    }
    void clockGeom(float& cxK, float& cy, float& right, float& tw) const {
        float s = (float)uiScale(), R = statR(), cxI, cyI, ri, ti; idGeom(cxI, cyI, ri, ti);
        tw = Renderer::textWidth("00:00", uiScale());
        right = cxI - R - 8.0f * s; cxK = right - R - 12.0f * s - tw; cy = cyI;
    }
    void clockHM(int& hh, int& mm) const {
        const float hrs = std::fmod(6.0f + net.timeOfDay() * (24.0f / e0net::kCycleSec), 24.0f);
        hh = (int)hrs % 24; mm = (int)((hrs - std::floor(hrs)) * 60.0f) % 60;
    }
    void drawStatWidgets() {
        const double tn = std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 1000.0);
        const float pulse = 0.65f + 0.35f * std::sin((float)tn * 8.0f), R = statR();
        for (int k = 0; k < 2; ++k) {
            float cx, cy; statPos(k, cx, cy);
            float f, r, g, b, al = 1.0f;
            if (k == 0) {   // fome: verde -> laranja -> vermelho, pisca < 25%
                f = hunger / 100.0f;
                r = f > 0.5f ? 0.35f + (1.0f - f) : 0.95f; g = f > 0.5f ? 0.85f : 0.25f + f * 1.2f; b = 0.15f;
                if (f < 0.25f) al = pulse;
            } else {        // vida: vermelha (pisca < 25% ou passando fome; mais clara regenerando)
                f = health / 100.0f;
                const bool regen = !godLife && hunger >= kRegenMinHunger && health < 99.5f, starve = hunger <= 0.0f && health > 0.0f;
                r = regen ? 0.95f : 0.90f; g = regen ? 0.40f : 0.18f; b = regen ? 0.40f : 0.18f;
                if (f < 0.25f || starve) al = pulse;
            }
            f = std::max(0.0f, std::min(1.0f, f));
            renderer.uiShape(Renderer::UiFill, cx, cy, R * 1.04f, 0.05f, 0.035f, 0.025f, 0.65f);
            renderer.uiShape(Renderer::UiFill, cx, cy, R, 0.10f, 0.075f, 0.055f, 0.88f);
            renderer.uiShape(Renderer::UiRing, cx, cy, R * 1.03f, theme::kBrassDim.r, theme::kBrassDim.g, theme::kBrassDim.b, 0.75f);
            renderer.uiArc(cx, cy, R * 0.96f, 0.16f, 1.0f, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.12f);
            if (f > 0.002f) renderer.uiArc(cx, cy, R * 0.96f, 0.16f, f, r, g, b, 0.95f * al);
            renderer.uiIconTint(k == 0 ? HiFood : HiHeart, cx, cy, R * 0.48f, r * 0.35f + 0.60f, g * 0.35f + 0.56f, b * 0.35f + 0.48f, 0.95f * (al < 1.0f ? 0.6f + 0.4f * al : 1.0f));
        }
        {   // MOEDAS: placa de carvalho + aro de latão envelhecido
            float cxC, cyC, right, tw; coinGeom(cxC, cyC, right, tw);
            const float hh = R * 0.64f;
            renderer.uiWoodPanel((cxC + right) * 0.5f, cyC, (right - cxC) * 0.5f, hh, hh, theme::PanelCard, 0.90f, 1.0f);
            renderer.uiShape(Renderer::UiFill, cxC, cyC, R, 0.11f, 0.085f, 0.060f, 0.94f);
            renderer.uiArc(cxC, cyC, R, 0.15f, 1.0f, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.92f);
            renderer.uiIconTint(HiCoins, cxC, cyC, R * 0.50f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        }
        {   // ID: placa de carvalho + aro de latão envelhecido
            float cxI, cyI, right, tw; idGeom(cxI, cyI, right, tw);
            const float hh = R * 0.64f;
            renderer.uiWoodPanel((cxI + right) * 0.5f, cyI, (right - cxI) * 0.5f, hh, hh, theme::PanelCard, 0.90f, 3.0f);
            renderer.uiShape(Renderer::UiFill, cxI, cyI, R, 0.11f, 0.085f, 0.060f, 0.94f);
            renderer.uiArc(cxI, cyI, R, 0.15f, 1.0f, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.92f);
            renderer.uiIconTint(HiIdCard, cxI, cyI, R * 0.50f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        }
        {   // RELÓGIO: placa de carvalho + aro de latão envelhecido
            float cxK, cyK, right, tw; clockGeom(cxK, cyK, right, tw);
            const float hh = R * 0.64f;
            renderer.uiWoodPanel((cxK + right) * 0.5f, cyK, (right - cxK) * 0.5f, hh, hh, theme::PanelCard, 0.90f, 2.0f);
            renderer.uiShape(Renderer::UiFill, cxK, cyK, R, 0.11f, 0.085f, 0.060f, 0.94f);
            renderer.uiArc(cxK, cyK, R, 0.15f, 1.0f, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.92f);
            renderer.uiIconTint(HiClock, cxK, cyK, R * 0.50f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        }
    }
    // Ícone de item (formas simples, sem textura): quadrado colorido; ferramentas = cabo de madeira + cabeça de metal. half = meia largura.
    void drawItemIcon(float cx, float cy, float half, int id, float a) {
        const ItemDef& d = itemDef(id);
        if (renderer.hasIcons()) {   // ícones desenhados (atlas assets/icons.e0i): 0 adaga 1 machadinha 2 picareta 3 tocha / 4 coco 5 folha 6 pedra 7 graveto / 8 diamante 9 ouro 10 madeira 11 corda
            static const int8_t kIcon[ItCount] = {-1, 7, 6, 10, 6, 4, 5, 1, 2, 9, 8, 3, 0, 11, 32, -1, 56, 60, -1, -1, -1, -1};   // 56 bacamarte e 60 pólvora (ícones da HUD)
            int ic = (id > 0 && id < ItCount) ? kIcon[id] : -1;
            if (ic >= 0) { renderer.uiIcon(ic, cx, cy, half * (id == ItPebble ? 0.62f : 1.12f), a); return; }
        }
        if (id == ItTorch) {
            renderer.uiRoundRect(cx - half * 0.10f, cy + half * 0.22f, half * 0.12f, half * 0.80f, half * 0.08f, 0.40f, 0.25f, 0.13f, a);
            renderer.uiRoundRect(cx, cy - half * 0.38f, half * 0.24f, half * 0.28f, half * 0.12f, 0.30f, 0.21f, 0.13f, a);
            renderer.uiRoundRect(cx + half * 0.04f, cy - half * 0.82f, half * 0.19f, half * 0.34f, half * 0.19f, 1.0f, 0.55f, 0.12f, a);
            renderer.uiRoundRect(cx + half * 0.04f, cy - half * 0.74f, half * 0.09f, half * 0.18f, half * 0.09f, 1.0f, 0.90f, 0.45f, a);
            return;
        }
        if (id == ItDagger) {
            renderer.uiRoundRect(cx, cy + half * 0.50f, half * 0.10f, half * 0.38f, half * 0.06f, 0.36f, 0.22f, 0.10f, a);
            renderer.uiRoundRect(cx, cy + half * 0.14f, half * 0.34f, half * 0.07f, half * 0.04f, 0.20f, 0.12f, 0.12f, a);
            renderer.uiRoundRect(cx, cy - half * 0.38f, half * 0.17f, half * 0.55f, half * 0.12f, d.r, d.g, d.b, a);
            return;
        }
        if (id == ItHatchet || id == ItPickaxe) {
            renderer.uiRoundRect(cx, cy + half * 0.12f, half * 0.13f, half * 0.95f, half * 0.08f, 0.62f, 0.44f, 0.24f, a);
            if (id == ItHatchet) {
                renderer.uiRoundRect(cx + half * 0.40f, cy - half * 0.62f, half * 0.52f, half * 0.34f, half * 0.12f, d.r, d.g, d.b, a);
                renderer.uiRoundRectBorder(cx + half * 0.40f, cy - half * 0.62f, half * 0.52f, half * 0.34f, half * 0.12f, 1.5f, 1, 1, 1, 0.55f * a);
            } else {
                renderer.uiRoundRect(cx, cy - half * 0.74f, half * 1.0f, half * 0.17f, half * 0.10f, d.r, d.g, d.b, a);
                renderer.uiRoundRectBorder(cx, cy - half * 0.74f, half * 1.0f, half * 0.17f, half * 0.10f, 1.5f, 1, 1, 1, 0.55f * a);
            }
            return;
        }
        if (id == ItIron) {   // lingote/minério de ferro rústico com brilho metálico
            renderer.uiGrad(cx, cy + half * 0.10f, half * 0.82f, half * 0.46f, half * 0.20f, 0.54f, 0.56f, 0.60f, a, 0.24f, 0.26f, 0.30f, a, 0.0f, 1.0f);
            renderer.uiGrad(cx, cy - half * 0.14f, half * 0.62f, half * 0.24f, half * 0.12f, 0.78f, 0.80f, 0.84f, a, 0.42f, 0.44f, 0.48f, a, 0.0f, 1.0f);
            renderer.uiRoundRectBorder(cx, cy + half * 0.10f, half * 0.82f, half * 0.46f, half * 0.20f, 1.8f, 0.12f, 0.11f, 0.10f, 0.85f * a);
            return;
        }
        if (id >= ItPotHeal && id <= ItPotShield) {   // frasco de vidro rústico com rolha de cortiça e líquido colorido
            // rolha de cortiça
            renderer.uiRoundRect(cx, cy - half * 0.76f, half * 0.20f, half * 0.16f, half * 0.05f, 0.58f, 0.40f, 0.22f, a);
            // gargalo de vidro
            renderer.uiRoundRect(cx, cy - half * 0.48f, half * 0.26f, half * 0.20f, half * 0.08f, 0.74f, 0.78f, 0.80f, 0.45f * a);
            renderer.uiRoundRectBorder(cx, cy - half * 0.48f, half * 0.26f, half * 0.20f, half * 0.08f, 1.5f, 0.88f, 0.90f, 0.86f, 0.75f * a);
            // bojo do frasco (vidro escuro)
            renderer.uiShape(Renderer::UiFill, cx, cy + half * 0.22f, half * 0.66f, 0.12f, 0.14f, 0.15f, 0.65f * a);
            // líquido (tom levemente enobrecido, nunca neon puro)
            const float lr = d.r * 0.85f + 0.08f, lg = d.g * 0.82f + 0.06f, lb = d.b * 0.82f + 0.06f;
            renderer.uiShape(Renderer::UiFill, cx, cy + half * 0.26f, half * 0.56f, lr, lg, lb, 0.92f * a);
            renderer.uiGlow(cx - half * 0.16f, cy + half * 0.14f, half * 0.28f, half * 0.28f, 1.0f, 0.98f, 0.90f, 0.45f * a);
            renderer.uiShape(Renderer::UiRing, cx, cy + half * 0.22f, half * 0.66f, 0.88f, 0.84f, 0.74f, 0.82f * a);
            return;
        }
        renderer.uiRoundRect(cx, cy, half, half, half * 0.33f, d.r, d.g, d.b, a);
        renderer.uiRoundRectBorder(cx, cy, half, half, half * 0.33f, 2.0f, d.r * 1.4f, d.g * 1.4f, d.b * 1.4f, a);
    }
    float invWeight() const { float t = 0; for (const auto& s : inv) t += itemDef(s.id).kg * s.n; return t; }
    int invUsed() const { int c = 0; for (const auto& s : inv) if (s.id) ++c; return c; }
    // Online (servidor com inventário): cada mudança feita aqui é PREVISTA e também mandada ao servidor (MsgAct), que aplica as mesmas regras.
    void invMove(int from, int to) {
        if (from == to || from < 0 || to < 0 || from >= kInvSlots || to >= kInvSlots || !inv[from].id) return;
        e0net::invMove(inv, from, to);
        if (invServer()) net.requestAct(e0net::ActInvMove, (uint16_t)(from | (to << 8)), 0);
        invSel = to;
    }
    void invSplit() {
        if (invSel < 0 || inv[invSel].n < 2) return;
        for (int i = 0; i < kInvSlots; ++i) if (!inv[i].id) {
            e0net::invSplit(inv, invSel, i);
            if (invServer()) net.requestAct(e0net::ActInvSplit, (uint16_t)(invSel | (i << 8)), 0);
            return;
        }
    }
    // DESCARTAR: o item cai no chão à frente do jogador (todos veem e podem pegar de novo). O slot esvazia na hora; se o servidor recusar, volta.
    void invDiscard() {
        if (invSel < 0 || !inv[invSel].id) return;
        net.requestAct(e0net::ActDrop, (uint16_t)(inv[invSel].id | ((invSel + 1) << 8)), inv[invSel].n);
        inv[invSel] = {};
    }
    // ENTREGAR: a pilha selecionada vai para o jogador mais próximo (o servidor escolhe, até kGiveMaxDist). Sem ninguém perto, o item volta.
    void invGive() {
        if (invSel < 0 || !inv[invSel].id) return;
        net.requestAct(e0net::ActGive, (uint16_t)(inv[invSel].id | ((invSel + 1) << 8)), inv[invSel].n);
        inv[invSel] = {};
    }
    void invToHotbar() {
        if (invSel < 0 || !inv[invSel].id) return;
        if (invSel >= kInvHotFirst) {                       // da hotbar para a mochila
            for (int i = 0; i < kInvHotFirst; ++i) if (!inv[i].id) { invMove(invSel, i); return; }
            return;
        }
        for (int i = kInvHotFirst; i < kInvSlots; ++i) if (!inv[i].id) { invMove(invSel, i); return; }
        invMove(invSel, kInvHotFirst + std::max(hotbarCur, 0));
    }
    void invSort() {   // organiza só a mochila (linhas 1..5): junta pilhas e ordena por tipo
        e0net::invSort(inv);
        if (invServer()) net.requestAct(e0net::ActInvSort, 0, 0);
        invSel = -1;
    }

    // Inventário em TELA CHEIA (sem borda, sem equipamento, sem painel de detalhes): grade 5x6 à esquerda/centro, botões em COLUNA à direita;
    // os detalhes do item selecionado aparecem num dropdown junto ao slot (nome, descrição, quantidade, peso, pilha máx.).
    struct InvGeo { float u, gx, gy, slot, gap, hotGap, margin, bx, bw, bh, bgap; };
    InvGeo invGeo() const {
        InvGeo g; g.u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
        float u = g.u;
        g.margin = 44 * u; g.gap = 10 * u; g.hotGap = 36 * u;
        g.bw = 320 * u; g.bh = 86 * u; g.bgap = 16 * u;
        g.gy = 112 * u;
        float avail = h - g.gy - 28 * u;
        g.slot = (avail - (kInvRows - 1) * g.gap - (g.hotGap - g.gap)) / kInvRows;
        float gw = kInvCols * g.slot + (kInvCols - 1) * g.gap, mid = 70 * u;
        g.gx = (w - (gw + mid + g.bw)) * 0.5f;      // grade + coluna de botões juntas, centralizadas na horizontal
        g.bx = g.gx + gw + mid;
        return g;
    }
    Rect2D invSlotRect(int i) const {
        InvGeo g = invGeo(); int c = i % kInvCols, r = i / kInvCols;
        return {g.gx + c * (g.slot + g.gap), g.gy + r * (g.slot + g.gap) + (r == kInvRows - 1 ? g.hotGap - g.gap : 0), g.slot, g.slot};
    }
    int invSlotAt(float x, float y) const {
        for (int i = 0; i < kInvSlots; ++i) if (invSlotRect(i).contains(x, y)) return i;
        return -1;
    }
    // BOTAO FECHAR (X vermelho redondo), o MESMO em inventário, craft, baú, mapa e loja (closeBtnDraw); canto superior direito
    Rect2D invCloseRect() const { InvGeo g = invGeo(); float s = 64 * g.u; return {w - g.margin * 0.6f - s, 14 * g.u, s, s}; }
    Rect2D invBtnRect(int k) const {   // coluna à direita, de cima para baixo: 0 DIVIDIR, 1 DESCARTAR, 2 P/ ACESSO, 3 ORGANIZAR, 4 ENTREGAR
        InvGeo g = invGeo();
        float total = 5 * g.bh + 4 * g.bgap;   // coluna centralizada na vertical (centro-direita)
        return {g.bx, (h - total) * 0.5f + k * (g.bh + g.bgap), g.bw, g.bh};
    }
    // Dropdown de detalhes do item selecionado (largura 0 = escondido): abaixo do slot, ou acima se não couber; preso às bordas da tela
    Rect2D invDropRect() const {
        if (invSel < 0 || !inv[invSel].id || (invDragPtr >= 0 && invDragMoved)) return {0, 0, 0, 0};
        InvGeo g = invGeo(); Rect2D s = invSlotRect(invSel);
        float dw = 430 * g.u, dh = 214 * g.u;
        float x = std::max(g.margin * 0.5f, std::min(s.x, (float)w - g.margin * 0.5f - dw));
        float y = s.y + s.h + 6 * g.u;
        if (y + dh > h - 8 * g.u) y = s.y - dh - 6 * g.u;
        return {x, std::max(8 * g.u, y), dw, dh};
    }
    Rect2D invOpenBtnRect() const {   // ao lado do CONSTRUIR
        float s = (float)uiScale();
        (void)s; return menuBtnRect(1);
    }
    void openInventory() {
        invOpen = true; invSel = -1; invDragPtr = -1;
        joyT.id = lookT.id = runPtr = collectPtr = actionPtr = -1; collectDown = false;
        input.moveX = input.moveY = 0;
    }
    void invDragEnd(float x, float y, bool cancel) {
        if (invDragPtr < 0) return;
        if (!cancel) {
            if (invDragMoved) {
                int t = invSlotAt(x, y);
                int k = -1; for (int i = 0; i < 5; ++i) if (invBtnRect(i).contains(x, y)) k = i;
                if (t >= 0) invMove(invDragFrom, t);
                else if (k >= 0) {   // soltou o item sobre um botão da coluna: executa a ação dele nesse item
                    invSel = invDragFrom;
                    if (k == 0) invSplit(); else if (k == 1) invDiscard(); else if (k == 2) invToHotbar(); else if (k == 3) invSort(); else invGive();
                }
            }
            else invSel = (invSel == invDragFrom) ? -1 : invDragFrom;
        }
        invDragPtr = -1; invDragFrom = -1; invDragMoved = false;
    }
    bool handleInvTouchDown(int pid, float x, float y) {
        if (invCloseRect().contains(x, y)) { invOpen = false; invDragPtr = -1; return true; }
        for (int k = 0; k < 5; ++k) if (invBtnRect(k).contains(x, y)) {
            if (k == 0) invSplit(); else if (k == 1) invDiscard(); else if (k == 2) invToHotbar(); else if (k == 3) invSort(); else invGive();
            return true;
        }
        if (invDropRect().contains(x, y)) return true;   // toque no dropdown: não faz nada (também não deseleciona)
        int s = invSlotAt(x, y);
        if (s >= 0) {
            if (inv[s].id && invDragPtr < 0) { invDragPtr = pid; invDragFrom = s; invDragX = invDragX0 = x; invDragY = invDragY0 = y; invDragMoved = false; }
            else invSel = s;
            return true;
        }
        invSel = -1;   // toque no vazio: fecha o dropdown
        return true;
    }

    // ================= BAÚ DO NAVIO (tela cheia): baú à esquerda, mochila à direita, botões no meio =================
    // O conteúdo é do servidor (compartilhado por quem está a bordo); aqui só se pede e se mostra. Tocar seleciona; tocar de novo no selecionado move tudo.
    struct ChestGeo { float u, s, gap, cx, cy, bx, bw, bh, bgap, ix, iy; };
    ChestGeo chestGeo() const {
        ChestGeo g; g.u = std::min((float)h / 1080.0f, (float)w / 1900.0f); const float u = g.u;
        g.s = 118 * u; g.gap = 10 * u; g.bw = 300 * u; g.bh = 86 * u; g.bgap = 16 * u;
        const float cw = 6 * g.s + 5 * g.gap, iw = kInvCols * g.s + (kInvCols - 1) * g.gap, mid = 56 * u;
        g.cx = ((float)w - (cw + mid + g.bw + mid + iw)) * 0.5f; g.cy = 150 * u;
        g.bx = g.cx + cw + mid; g.ix = g.bx + g.bw + mid; g.iy = 150 * u;
        return g;
    }
    Rect2D chestSlotRect(int i) const { ChestGeo g = chestGeo(); return {g.cx + (i % 6) * (g.s + g.gap), g.cy + (i / 6) * (g.s + g.gap), g.s, g.s}; }
    Rect2D chestInvRect(int i) const { ChestGeo g = chestGeo(); return {g.ix + (i % kInvCols) * (g.s + g.gap), g.iy + (i / kInvCols) * (g.s + g.gap), g.s, g.s}; }
    Rect2D chestBtnRect(int k) const { ChestGeo g = chestGeo(); return {g.bx, g.cy + 70 * g.u + k * (g.bh + g.bgap), g.bw, g.bh}; }
    void openChestUi() {
        chestUi = true; chestOpen = true; chestSelSide = 0; chestSelIdx = -1;
        joyT.id = lookT.id = runPtr = collectPtr = actionPtr = -1; collectDown = false; input.moveX = input.moveY = 0;
    }
    void chestSay(const char* t) { if (chestUi) { chestMsg = t; chestMsgT = 2.5f; } else notify(t); }
    bool chestSelValid() const {
        if (chestSelSide == 2) return chestSelIdx >= 0 && chestSelIdx < kInvSlots && inv[chestSelIdx].id;
        if (chestSelSide == 1) return chestSelIdx >= 0 && chestSelIdx < e0net::kChestSlots && net.chest(shipSel).s[chestSelIdx].id;
        return false;
    }
    void closeChestUi() { chestUi = false; chestOpen = false; chestSelSide = 0; chestSelIdx = -1; }
    // k: 0 = tudo, 1 = metade, 2 = 1 unidade; vale para o slot selecionado (mochila -> baú, baú -> mochila)
    void chestMove(int k) {
        if (!chestSelValid()) { chestSelSide = 0; chestSelIdx = -1; chestSay("TOQUE EM UM ITEM"); return; }
        const int ship = shipSel;
        if (chestSelSide == 2) {
            ItemStack& st = inv[chestSelIdx];
            if (!st.id) return;
            int n = k == 0 ? st.n : (k == 1 ? std::max(1, st.n / 2) : 1);
            e0net::Chest tmp = net.chest(ship); n = e0net::chestPut(tmp, st.id, n);   // quanto cabe no baú
            if (n <= 0) { chestSay("BAU CHEIO"); return; }
            net.requestAct(e0net::ActChestPut, (uint16_t)(st.id | (ship << 8)), (uint16_t)(n | ((chestSelIdx + 1) << 8)));
            st.n -= (uint16_t)n; if (!st.n) { st = {}; chestSelSide = 0; chestSelIdx = -1; }   // some na hora; se o servidor recusar, volta
        } else {
            const e0net::ChestSlot cs = net.chest(ship).s[chestSelIdx];
            if (!cs.id) return;
            int n = k == 0 ? cs.n : (k == 1 ? std::max(1, cs.n / 2) : 1);
            while (n > 0 && !invFits(cs.id, n)) --n;
            if (n <= 0) { chestSay("MOCHILA CHEIA OU MUITO PESADA"); return; }
            net.requestAct(e0net::ActChestTake, (uint16_t)(chestSelIdx | (ship << 8)), (uint16_t)n);
            if (n >= cs.n) { chestSelSide = 0; chestSelIdx = -1; }   // esvaziou o slot
        }
    }
    void chestTouchDown(float x, float y) {
        if (invCloseRect().contains(x, y)) { closeChestUi(); return; }
        for (int k = 0; k < 3; ++k) if (chestBtnRect(k).contains(x, y)) { chestMove(k); return; }
        for (int i = 0; i < e0net::kChestSlots; ++i) if (chestSlotRect(i).contains(x, y)) {
            if (!net.chest(shipSel).s[i].id) { chestSelSide = 0; chestSelIdx = -1; return; }
            if (chestSelSide == 1 && chestSelIdx == i) chestMove(0); else { chestSelSide = 1; chestSelIdx = i; }
            return;
        }
        for (int i = 0; i < kInvSlots; ++i) if (chestInvRect(i).contains(x, y)) {
            if (!inv[i].id) { chestSelSide = 0; chestSelIdx = -1; return; }
            if (chestSelSide == 2 && chestSelIdx == i) chestMove(0); else { chestSelSide = 2; chestSelIdx = i; }
            return;
        }
        chestSelSide = 0; chestSelIdx = -1;
    }
    void drawChestShapes() {
        ChestGeo g = chestGeo(); const float u = g.u; const e0net::Chest& C = net.chest(shipSel);
        renderer.uiWoodPanel(w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f, theme::PanelBgWood, 0.94f, 1.0f);
        // painel do BAÚ à esquerda (6x5) + painel da MOCHILA à direita (5x6)
        const int cRows = (e0net::kChestSlots + 5) / 6, iRows = (kInvSlots + kInvCols - 1) / kInvCols;
        const float cw = 6 * g.s + 5 * g.gap, ch = cRows * g.s + (cRows - 1) * g.gap;
        const float iw = kInvCols * g.s + (kInvCols - 1) * g.gap, ih = iRows * g.s + (iRows - 1) * g.gap;
        renderer.uiWoodPanel(g.cx + cw * 0.5f, g.cy + ch * 0.5f, cw * 0.5f + 22 * u, ch * 0.5f + 22 * u, 16 * u, theme::PanelWood, 0.97f, 2.0f);
        renderer.uiWoodPanel(g.ix + iw * 0.5f, g.iy + ih * 0.5f, iw * 0.5f + 22 * u, ih * 0.5f + 22 * u, 16 * u, theme::PanelWood, 0.97f, 3.0f);
        auto slot = [&](const Rect2D& r, int id, bool sel, bool hot) {
            float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hw = r.w * 0.5f;
            renderer.uiWoodPanel(cx, cy, hw, hw, 10 * u, sel ? theme::PanelCardSel : (hot ? theme::PanelSlotHot : theme::PanelSlot), 0.96f, cx * 0.01f);
            if (id) drawItemIcon(cx, cy - 10 * u, hw * 0.50f, id, 1.0f);
        };
        for (int i = 0; i < e0net::kChestSlots; ++i) slot(chestSlotRect(i), C.s[i].id, chestSelValid() && chestSelSide == 1 && chestSelIdx == i, false);
        for (int i = 0; i < kInvSlots; ++i) slot(chestInvRect(i), inv[i].id, chestSelValid() && chestSelSide == 2 && chestSelIdx == i, i >= kInvHotFirst);
        // botões centrais no passe de UI (carvalho & latão)
        const bool has = chestSelValid();
        for (int k = 0; k < 3; ++k) {
            Rect2D b = chestBtnRect(k);
            renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12 * u, !has ? theme::BtnDisabled : (k == 0 ? theme::BtnBrass : theme::BtnWood), has ? 1.0f : 0.72f);
        }
    }
    void drawChestText() {
        ChestGeo g = chestGeo(); const float u = g.u; const e0net::Chest& C = net.chest(shipSel);
        const int t1 = std::max(1, (int)std::lround(2.1f * u)), t2 = std::max(2, (int)std::lround(3.0f * u)), t3 = std::max(3, (int)std::lround(4.6f * u));
        auto boxLbl = [&](const Rect2D& r, const char* label, int ts, theme::Col fg, bool sh) {
            renderer.text(r.x + (r.w - Renderer::textWidth(label, ts)) * 0.5f, r.y + (r.h - 7.0f * ts) * 0.5f, ts, label, fg.r, fg.g, fg.b, 1.0f, sh);
        };
        auto ctr = [&](float cx, float y, int ts, const char* s, float r, float gg, float b, float a) { renderer.text(cx - Renderer::textWidth(s, ts) * 0.5f, y, ts, s, r, gg, b, a); };
        char buf[96];
        std::snprintf(buf, sizeof buf, "BA\xDA - %s", curDef().name);
        renderer.text(g.cx, 36 * u, t3, buf, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        std::snprintf(buf, sizeof buf, "SLOTS %d/%d  (COMPARTILHADO A BORDO)", e0net::chestCount(C), e0net::kChestSlots);
        renderer.text(g.cx, 36 * u + 7.0f * t3 + 12 * u, t1, buf, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);
        renderer.text(g.ix, 104 * u, t2, "MOCHILA", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        std::snprintf(buf, sizeof buf, "PESO %.1f/%.0f KG", invWeight(), kInvMaxKg);
        renderer.text(g.ix + 5 * g.s + 4 * g.gap - Renderer::textWidth(buf, t1), 108 * u, t1, buf, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.92f, false);
        closeBtnQueue(invCloseRect());
        auto cell = [&](const Rect2D& r, int id, int n) {
            if (!id) return;
            const ItemDef& d = itemDef(id);
            int nts = Renderer::textWidth(d.name, t2) <= r.w - 10 * u ? t2 : (Renderer::textWidth(d.name, t1) <= r.w - 8 * u ? t1 : 1);
            ctr(r.x + r.w * 0.5f, r.y + r.h - 12 * u - 7.0f * nts, nts, d.name, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f);
            if (n > 1) { char q[12]; std::snprintf(q, sizeof q, "X%d", n); renderer.text(r.x + r.w - 10 * u - Renderer::textWidth(q, t2), r.y + 9 * u, t2, q, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f); }
        };
        for (int i = 0; i < e0net::kChestSlots; ++i) cell(chestSlotRect(i), C.s[i].id, C.s[i].n);
        for (int i = 0; i < kInvSlots; ++i) {
            Rect2D r = chestInvRect(i);
            if (i >= kInvHotFirst) { char n[4]; std::snprintf(n, sizeof n, "%d", i - kInvHotFirst + 1); renderer.text(r.x + 12 * u, r.y + 11 * u, t2, n, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.85f); }
            cell(r, inv[i].id, inv[i].n);
        }
        const bool has = chestSelValid(), fromInv = chestSelSide == 2;
        const char* l0 = !has ? "TUDO" : (fromInv ? "GUARDAR TUDO" : "PEGAR TUDO");
        const char* l1 = !has ? "METADE" : (fromInv ? "GUARDAR METADE" : "PEGAR METADE");
        const char* l2 = !has ? "1 UNIDADE" : (fromInv ? "GUARDAR 1" : "PEGAR 1");
        boxLbl(chestBtnRect(0), l0, t2, has ? theme::kInk : theme::kCreamMuted, false);
        boxLbl(chestBtnRect(1), l1, t2, has ? theme::kCream : theme::kCreamMuted, has);
        boxLbl(chestBtnRect(2), l2, t2, has ? theme::kBrassBright : theme::kCreamMuted, has);
        if (chestMsgT > 0.0f) { Rect2D b2 = chestBtnRect(2); ctr(b2.x + b2.w * 0.5f, b2.y + b2.h + 150 * u, t2, chestMsg.c_str(), theme::kAmberText.r, theme::kAmberText.g, theme::kAmberText.b, std::min(1.0f, chestMsgT * 2.0f)); }
        if (has) {
            int id = fromInv ? inv[chestSelIdx].id : C.s[chestSelIdx].id, n = fromInv ? inv[chestSelIdx].n : C.s[chestSelIdx].n;
            if (id) {
                const ItemDef& d = itemDef(id); Rect2D b2 = chestBtnRect(2); float y = b2.y + b2.h + 30 * u;
                ctr(b2.x + b2.w * 0.5f, y, t2, d.name, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
                ctr(b2.x + b2.w * 0.5f, y + 7.0f * t2 + 12 * u, t1, d.d1, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.94f);
                std::snprintf(buf, sizeof buf, "X%d  %.2f KG", n, d.kg * n); ctr(b2.x + b2.w * 0.5f, y + 7.0f * t2 + 12 * u + 9.0f * t1 + 8 * u, t1, buf, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.92f);
            }
        } else {
            Rect2D b2 = chestBtnRect(2); ctr(b2.x + b2.w * 0.5f, b2.y + b2.h + 30 * u, t1, "TOQUE EM UM ITEM", theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.85f);
            ctr(b2.x + b2.w * 0.5f, b2.y + b2.h + 30 * u + 9.0f * t1 + 8 * u, t1, "TOQUE DE NOVO PARA MOVER TUDO", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.85f);
        }
    }

    // ================= CRAFT (tela cheia; só cliente, como o inventário) =================
    using Recipe = e0net::Recipe;   // a MESMA tabela do servidor (shared/inventory.h)
    static const Recipe* recipes(int& n) { return e0net::recipeTable(n); }
    int invCount(int id) const { int c = 0; for (const auto& st : inv) if (st.id == id) c += st.n; return c; }
    void invRemove(int id, int n) { e0net::invRemove(inv, id, n); }   // tira da mochila primeiro, depois do acesso rápido
    bool canCraft(const Recipe& r) const { for (int k = 0; k < 3; ++k) if (r.in[k] && invCount(r.in[k]) < r.inN[k]) return false; return true; }
    int craftBusy = -1; float craftT = 0.0f;   // receita em fabricação (-1 = nenhuma) e tempo decorrido
    void startCraft(int i) {
        int n; const Recipe* R = recipes(n); if (i < 0 || i >= n) return;
        if (craftBusy >= 0) { if (i != craftBusy) notify("TERMINE O CRAFT ATUAL PRIMEIRO"); return; }
        if (!canCraft(R[i])) { notify("FALTAM INGREDIENTES"); return; }
        craftBusy = i; craftT = 0.0f;   // os ingredientes só são gastos ao terminar (fechar a tela cancela sem custo)
    }
    void updateCraft(float dt) {
        if (craftBusy < 0) return;
        int n; const Recipe* R = recipes(n);
        craftT += dt;
        if (craftT >= R[craftBusy].time) { int i = craftBusy; craftBusy = -1; doCraft(i); }
    }
    void doCraft(int i) {
        int n; const Recipe* R = recipes(n); if (i < 0 || i >= n) return;
        const Recipe& r = R[i];
        const int res = e0net::invCraft(inv, i);   // prevê aqui; online o servidor confere e gasta de verdade (ActCraft)
        if (res == 1) { notify("FALTAM INGREDIENTES"); return; }
        if (res == 2) { notify("INVENTARIO CHEIO"); return; }
        if (res != 0) return;
        if (invServer()) net.requestAct(e0net::ActCraft, (uint16_t)i, 0);
        char t[48]; std::snprintf(t, sizeof t, "FABRICOU X%d %s", r.outN, itemDef(r.out).name); notify(t);
    }
    Rect2D craftOpenBtnRect() const {   // ao lado do INVENTARIO
        return menuBtnRect(2);
    }
    void openCraft() {
        craftOpen = true; if (craftBusy >= 0) craftSel = craftBusy;   // reabrir mostra a receita em andamento
        joyT.id = lookT.id = runPtr = collectPtr = actionPtr = -1; collectDown = false; input.moveX = input.moveY = 0;
    }
    struct CraftGeo { float u, margin, cx, cy, cw, ch, cgap, dx, dy; };
    CraftGeo craftGeo() const {
        CraftGeo g; g.u = std::min((float)h / 1080.0f, (float)w / 1900.0f); float u = g.u;
        g.margin = 44 * u; g.cgap = 14 * u; g.cw = 4 * 150 * u + 3 * g.cgap; g.ch = 168 * u;   // grade de 4 colunas (rola na vertical)
        float total = g.cw + 70 * u + 640 * u;
        g.cx = (w - total) * 0.5f; g.cy = 112 * u;
        g.dx = g.cx + g.cw + 70 * u; g.dy = g.cy;
        return g;
    }
    float craftScroll = 0.0f, craftScroll0 = 0.0f, craftY0 = 0.0f; int craftPtr = -1; bool craftMoved = false;
    float craftCellW() const { return 150.0f * craftGeo().u; }
    Rect2D craftListRect() const { CraftGeo g = craftGeo(); return {g.cx - 6 * g.u, g.cy, g.cw + 12 * g.u, (float)h - 40 * g.u - g.cy}; }
    float craftMaxScroll() const {
        int n; recipes(n); CraftGeo g = craftGeo(); int rows = (n + 3) / 4;
        return std::max(0.0f, rows * (g.ch + g.cgap) - g.cgap - craftListRect().h);
    }
    Rect2D craftCardRect(int i) const { CraftGeo g = craftGeo(); return {g.cx + (i % 4) * (craftCellW() + g.cgap), g.cy + (i / 4) * (g.ch + g.cgap) - craftScroll, craftCellW(), g.ch}; }
    Rect2D craftMakeBtnRect() const { CraftGeo g = craftGeo(); return {g.dx, g.dy + 650 * g.u, 640 * g.u, 92 * g.u}; }
    void handleCraftTouchDown(int pid, float x, float y) {
        if (invCloseRect().contains(x, y)) { craftOpen = false; craftPtr = -1; return; }
        if (craftMakeBtnRect().contains(x, y)) { startCraft(craftSel); return; }
        if (craftPtr < 0 && craftListRect().contains(x, y)) { craftPtr = pid; craftY0 = y; craftScroll0 = craftScroll; craftMoved = false; }
    }
    void craftTouchMove(int pid, float y) {
        if (pid != craftPtr) return;
        const float dy = y - craftY0;
        if (!craftMoved && std::fabs(dy) > 14.0f * craftGeo().u) craftMoved = true;
        if (craftMoved) craftScroll = std::max(0.0f, std::min(craftMaxScroll(), craftScroll0 - dy));
    }
    void craftTouchUp(int pid, float x, float y, bool cancel) {
        if (pid != craftPtr) return;
        craftPtr = -1;
        if (cancel || craftMoved || !craftListRect().contains(x, y)) return;
        int n; recipes(n);
        for (int i = 0; i < n; ++i) if (craftCardRect(i).contains(x, y)) { craftSel = i; return; }
    }
    void craftClipOn() const { const Rect2D l = craftListRect(); glEnable(GL_SCISSOR_TEST); glScissor((GLint)l.x, (GLint)((float)h - l.y - l.h), (GLsizei)l.w, (GLsizei)l.h); }
    void drawCraftShapes() {
        CraftGeo g = craftGeo(); const float u = g.u; int n; const Recipe* R = recipes(n);
        renderer.uiWoodPanel(w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f, theme::PanelBgWood, 0.92f, 2.0f);
        renderer.uiGlow(w * 0.5f, h * 0.45f, w * 0.55f, h * 0.60f, 0.92f, 0.64f, 0.26f, 0.07f);
        // 2 painéis principais em carvalho escuro + latão + rebites (receitas à esquerda, mesa de trabalho à direita)
        const Rect2D lr = craftListRect();
        renderer.uiWoodPanel(lr.x + lr.w * 0.5f, lr.y + lr.h * 0.5f, lr.w * 0.5f + 20 * u, lr.h * 0.5f + 20 * u, 18 * u, theme::PanelWood, 0.97f, 1.0f);
        renderer.uiWoodPanel(g.dx + 320 * u, lr.y + lr.h * 0.5f, 352 * u, lr.h * 0.5f + 20 * u, 18 * u, theme::PanelWood, 0.97f, 2.0f);
        renderer.uiDivider(g.margin + 170 * u, 88 * u, 170 * u, 6 * u, 0.72f);
        craftClipOn();
        for (int i = 0; i < n; ++i) {
            Rect2D r = craftCardRect(i); bool ok = canCraft(R[i]), sel = i == craftSel;
            if (r.y + r.h < lr.y || r.y > lr.y + lr.h) continue;
            float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
            renderer.uiWoodPanel(cx, cy, r.w * 0.5f, r.h * 0.5f, 12 * u, sel ? theme::PanelCardSel : theme::PanelSlot, 0.98f, (float)i);
            if (ok && !sel) renderer.uiRoundRectBorder(cx, cy, r.w * 0.5f, r.h * 0.5f, 12 * u, 2.0f * u, 0.42f, 0.72f, 0.46f, 0.78f);
            drawItemIcon(cx, r.y + 62 * u, 44 * u, R[i].out, ok ? 1.0f : 0.48f);
            if (i == craftBusy) {
                float f = std::min(1.0f, craftT / std::max(0.1f, R[i].time)), bw = r.w - 28 * u;
                renderer.uiRoundRect(r.x + 14 * u + bw * 0.5f, r.y + r.h - 12 * u, bw * 0.5f, 4 * u, 3 * u, 0.06f, 0.04f, 0.03f, 0.85f);
                if (f > 0.01f) renderer.uiRoundRect(r.x + 14 * u + bw * f * 0.5f, r.y + r.h - 12 * u, bw * f * 0.5f, 4 * u, 3 * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
            }
        }
        {   // barra de rolagem dourada fina
            const float mx = craftMaxScroll();
            if (mx > 0.0f) {
                const float th = std::max(40.0f * u, lr.h * lr.h / (lr.h + mx)), ty = lr.y + (lr.h - th) * (craftScroll / mx);
                renderer.uiRoundRect(lr.x + lr.w - 2 * u, ty + th * 0.5f, 2.5f * u, th * 0.5f, 2.5f * u, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.78f);
            }
        }
        glDisable(GL_SCISSOR_TEST);
        const Recipe& r = R[craftSel];
        renderer.uiWoodPanel(g.dx + 110 * u, g.dy + 110 * u, 100 * u, 100 * u, 14 * u, theme::PanelSlotHot, 0.98f, 7.0f);
        drawItemIcon(g.dx + 110 * u, g.dy + 110 * u, 76 * u, r.out, 1.0f);
        renderer.uiDivider(g.dx + 320 * u, g.dy + 236 * u, 280 * u, 6 * u, 0.65f);
        for (int k = 0; k < 3; ++k) if (r.in[k]) {
            float y = g.dy + 310 * u + k * 100 * u;
            renderer.uiWoodPanel(g.dx + 320 * u, y + 38 * u, 320 * u, 40 * u, 10 * u, theme::PanelSlot, 0.95f, (float)k + 3.0f);
            drawItemIcon(g.dx + 50 * u, y + 38 * u, 32 * u, r.in[k], 1.0f);
        }
        // botão FABRICAR (ou progresso)
        Rect2D b = craftMakeBtnRect();
        bool can = canCraft(r);
        if (craftBusy == craftSel) {
            float f = std::min(1.0f, craftT / std::max(0.1f, R[craftBusy].time));
            renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12 * u, theme::BtnWood, 1.0f);
            if (f > 0.01f) renderer.uiButton(b.x + b.w * f * 0.5f, b.y + b.h * 0.5f, std::max(12 * u, b.w * f * 0.5f), b.h * 0.5f, 12 * u, theme::BtnMoss, 0.95f);
        } else {
            renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12 * u, can ? theme::BtnBrass : theme::BtnDisabled, 1.0f);
        }
    }
    void drawCraftText() {
        CraftGeo g = craftGeo(); const float u = g.u; int n; const Recipe* R = recipes(n);
        const int t1 = std::max(1, (int)std::lround(2.1f * u)), t2 = std::max(2, (int)std::lround(3.1f * u)), t3 = std::max(3, (int)std::lround(4.8f * u));
        char buf[96];
        renderer.text(g.margin, 32 * u, t3, "OFICINA DE CRAFT", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        closeBtnQueue(invCloseRect());
        renderer.endText(); craftClipOn(); renderer.beginText();
        for (int i = 0; i < n; ++i) {
            Rect2D r = craftCardRect(i); bool ok = canCraft(R[i]); float a = ok ? 1.0f : 0.58f;
            if (r.y + r.h < craftListRect().y || r.y > craftListRect().y + craftListRect().h) continue;
            const char* nm = itemDef(R[i].out).name; int ns = t1;
            while (ns > 1 && Renderer::textWidth(nm, ns) > r.w - 10 * u) --ns;
            renderer.text(r.x + (r.w - Renderer::textWidth(nm, ns)) * 0.5f, r.y + 118 * u, ns, nm, theme::kCream.r, theme::kCream.g, theme::kCream.b, a);
            std::snprintf(buf, sizeof buf, "%.0f S", R[i].time);
            renderer.text(r.x + (r.w - Renderer::textWidth(buf, t1)) * 0.5f, r.y + 118 * u + 9.0f * ns + 6 * u, t1, buf, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, a * 0.90f, false);
        }
        renderer.endText(); glDisable(GL_SCISSOR_TEST); renderer.beginText();
        const Recipe& r = R[craftSel]; const ItemDef& d = itemDef(r.out);
        renderer.text(g.dx + 236 * u, g.dy + 26 * u, t3, d.name, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        renderer.text(g.dx + 236 * u, g.dy + 98 * u, t1, d.d1, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.95f, false);
        renderer.text(g.dx + 236 * u, g.dy + 126 * u, t1, d.d2, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.95f, false);
        std::snprintf(buf, sizeof buf, "X%d  -  %.2f KG  -  %.0f S", r.outN, d.kg * r.outN, r.time);
        renderer.text(g.dx + 236 * u, g.dy + 170 * u, t1, buf, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f, false);
        renderer.text(g.dx, g.dy + 262 * u, t2, "INGREDIENTES", theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.95f, false);
        for (int k = 0; k < 3; ++k) if (r.in[k]) {
            float y = g.dy + 310 * u + k * 100 * u; int have = invCount(r.in[k]); bool ok = have >= r.inN[k];
            renderer.text(g.dx + 110 * u, y + 38 * u - 3.5f * t2, t2, itemDef(r.in[k]).name, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.96f, false);
            std::snprintf(buf, sizeof buf, "%d / %d", have, r.inN[k]);
            const theme::Col rc2 = ok ? theme::kGreenText : theme::kRedText;
            renderer.text(g.dx + 616 * u - Renderer::textWidth(buf, t2), y + 38 * u - 3.5f * t2, t2, buf, rc2.r, rc2.g, rc2.b, 1.0f, false);
        }
        bool can = canCraft(r);
        Rect2D b = craftMakeBtnRect();
        if (craftBusy == craftSel) {
            float f = std::min(1.0f, craftT / std::max(0.1f, R[craftBusy].time));
            std::snprintf(buf, sizeof buf, "FABRICANDO %d%%", (int)(f * 100.0f));
            renderer.text(b.x + (b.w - Renderer::textWidth(buf, t3)) * 0.5f, b.y + (b.h - 7.0f * t3) * 0.5f, t3, buf, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f);
        } else {
            const theme::Col bc = can ? theme::kInk : theme::kCreamMuted;
            renderer.text(b.x + (b.w - Renderer::textWidth("FABRICAR", t3)) * 0.5f, b.y + (b.h - 7.0f * t3) * 0.5f, t3, "FABRICAR", bc.r, bc.g, bc.b, 1.0f, false);
        }
    }

    // Coletáveis e alvos de ação de uma ilha (gids): chamado quando a ilha da vez fica pronta (a inicial no slot 0).
    void registerIslandObjects(int slot) {
        if (slot < 0 || slot >= e0net::kIslandSlots) return;
        collectiblesBySlot[slot].clear();
        actionablesBySlot[slot].clear();
        const e0net::World* IW = net.island(slot);
        if (!IW) return;
        for (size_t i = 0; i < IW->objects.size(); ++i) {
            uint8_t t = IW->objects[i].type; uint32_t gid = e0net::makeGid(slot, (uint32_t)i);
            if (e0net::World::isCollectible(t)) collectiblesBySlot[slot].push_back(gid);
            else if (t == e0net::ObjPalm || t == e0net::ObjCoconut || t == e0net::ObjRock || t == e0net::ObjLog) actionablesBySlot[slot].push_back(gid);
        }
        lastTakenGen = lastHarvestGen = 0xFFFFFFFFu;
    }

    // Alvo de ação: o que está À FRENTE do personagem (±63°) ao alcance do braço (< 1.3 m da superfície). Histerese p/ não piscar.
    void updateAction(float dt, const e0net::PlayerState& me) {
        using namespace e0net;
        const World* W = net.world();
        const float fx = std::sin(me.yaw), fz = -std::cos(me.yaw);
        // ponto do objeto mais próximo do jogador: o centro (árvores, pedras) ou o ponto do eixo (toras, que são compridas)
        auto nearest = [&](const WorldObject& o, float& cx, float& cz, float& r) {
            World::Collider c = World::colliderOf(o);
            cx = o.xcm * 0.01f; cz = o.zcm * 0.01f; r = c.a;
            if (c.kind == 2) { float t = (me.x - cx) * c.dx + (me.z - cz) * c.dz; t = std::max(-c.b, std::min(c.b, t)); cx += c.dx * t; cz += c.dz * t; }
        };
        auto eval = [&](uint32_t i, float reachMax, float cosMin, float& score) -> bool {
            if (net.isTaken(i)) return false;
            const WorldObject* po = net.object(i); if (!po) return false;
            const WorldObject& o = *po;
            if (std::fabs(o.xcm * 0.01f - me.x) > 8.0f || std::fabs(o.zcm * 0.01f - me.z) > 8.0f) return false;
            float dy = o.ycm * 0.01f - me.y;
            if (dy > 2.0f || dy < -2.0f) return false;
            float cx, cz, r; nearest(o, cx, cz, r);
            float dx = cx - me.x, dz = cz - me.z;
            float dist = std::sqrt(dx * dx + dz * dz), reach = dist - r - kPlayerRadius;
            if (reach > reachMax) return false;
            float cosA = dist > 1e-3f ? (fx * dx + fz * dz) / dist : 1.0f;
            if (cosA < cosMin) return false;
            score = std::max(reach, 0.0f) + (1.0f - cosA) * 2.0f;
            return true;
        };
        int best = -1; float bestScore = 1e9f, curScore = 1e9f;
        const int curSlot = W ? terrain().slotAt(me.x, me.z) : -1;
        if (W && curSlot >= 0 && curSlot < kIslandSlots && !isSwimmingPos(me.x, me.y, me.z)) {
            for (uint32_t i : actionablesBySlot[curSlot]) { float sc; if (eval(i, 1.3f, 0.45f, sc) && sc < bestScore) { bestScore = sc; best = (int)i; } }
            if (actCur >= 0 && !eval((uint32_t)actCur, 1.7f, 0.30f, curScore)) actCur = -1;
            if (best >= 0 && (actCur < 0 || (best != actCur && bestScore + 0.25f < curScore))) actCur = best;
            if (actCur >= 0) {
                const WorldObject& o = *net.object((uint32_t)actCur);
                float ox, oz, rr; nearest(o, ox, oz, rr);
                float dx = me.x - ox, dz = me.z - oz, dist = std::sqrt(dx * dx + dz * dz);
                float r = rr * (o.type == ObjRock ? 0.85f : 1.0f) + 0.05f;    // a etiqueta fica na SUPERFÍCIE voltada ao jogador
                actType = o.type; actDist = dist;
                float oy = o.type == ObjLog ? terrain().heightAt(ox, oz) + 0.25f : o.ycm * 0.01f;
                actPos = dist > 1e-3f ? Vec3{ox + dx / dist * r, oy, oz + dz / dist * r} : Vec3{ox, oy, oz};
            }
        } else actCur = -1;
        float target = actCur >= 0 ? 1.0f : 0.0f;
        actAlpha += (target - actAlpha) * (1.0f - std::exp(-dt * 12.0f));
        if (target == 0.0f && actAlpha < 0.004f) actAlpha = 0.0f;
    }
    struct HudMark { float alpha; Vec3 pos; const char* name; bool line; };   // line: coletável = linha + ponto + etiqueta; ação = SÓ etiqueta
    int marks(HudMark* out) const {   // marcadores visíveis: coletável e alvo de ação (linha branca + etiqueta)
        int n = 0;
        if (collectAlpha > 0.02f) out[n++] = {collectAlpha, collectPos, collectLabel, true};
        if (actAlpha > 0.02f) out[n++] = {actAlpha, actPos, actionNameCur(), false};
        return n;
    }

    // Escolhe o coletável mais próximo (raio de entrada 2.0 m, saída 2.6 m e troca só se o novo for bem mais perto: sem piscar).
    void updateCollect(float dt, const e0net::PlayerState& me) {
        hudT += dt;
        for (Notif& n : notifs) n.age += dt;
        while (!notifs.empty() && notifs.front().age >= kNotifLife) notifs.erase(notifs.begin());
        const e0net::World* W = net.world();
        const float kEnter = 2.0f, kExit = 2.6f;
        int best = -1; float bestD = 1e9f, curD = 1e9f;
        // pedido pendente: na hora da "pegada" (mão no chão) envia ao servidor; desiste se o jogador se afastou
        if (collectPending >= 0 && net.object((uint32_t)collectPending)) {
            collectPendT -= dt;
            const e0net::WorldObject& po = *net.object((uint32_t)collectPending);
            float pdx = po.xcm * 0.01f - me.x, pdz = po.zcm * 0.01f - me.z;
            if (pdx * pdx + pdz * pdz > kExit * kExit || net.isTaken((uint32_t)collectPending)) collectPending = -1;
            else if (collectPendT <= 0) {
                net.requestCollect((uint32_t)collectPending);
                collectLocalHide[(uint32_t)collectPending] = kCollectWaitT;
                collectPending = -1;
            }
        } else collectPending = -1;
        // confirmações do servidor (ou locais, offline) -> inventário + notificação
        for (uint32_t ci; ; ) {
            uint8_t ct;
            if (!net.popCollected(ci, ct)) break;
            int item = itemForObject(ct);
            if (!item) continue;
            if (invServer()) notify(nullptr, item, 1);   // online: o item entra pelo inventário do servidor (MsgInvSync)
            else if (invAdd(item, 1) > 0) notify("INVENTARIO CHEIO");
            else notify(nullptr, item, 1);
            LOG("coleta: %s (obj %u)", itemDef(item).name, ci);
        }
        // itens escondidos: pegos (bitset do servidor) ou esperando confirmação (só re-sincroniza tudo quando o bitset muda)
        if (W) {
            for (auto it = collectLocalHide.begin(); it != collectLocalHide.end(); ) {
                uint32_t gid = it->first;
                it->second = net.isTaken(gid) ? 0.0f : std::max(0.0f, it->second - dt);
                bool hide = net.isTaken(gid) || it->second > 0.0f;
                renderer.setCollectibleHidden(gid, hide);
                if (it->second <= 0.0f) it = collectLocalHide.erase(it);
                else ++it;
            }
            if (net.takenGeneration() != lastTakenGen) {
                lastTakenGen = net.takenGeneration();
                for (int sl = 0; sl < e0net::kIslandSlots; ++sl) {
                    for (uint32_t i : collectiblesBySlot[sl]) {
                        auto lh = collectLocalHide.find(i);
                        bool local = lh != collectLocalHide.end() && lh->second > 0.0f;
                        renderer.setCollectibleHidden(i, net.isTaken(i) || local);
                    }
                }
            }
        }
        const int curSlot = W ? e0net::terrain().slotAt(me.x, me.z) : -1;
        if (W && curSlot >= 0 && curSlot < e0net::kIslandSlots && !e0net::isSwimmingPos(me.x, me.y, me.z)) for (uint32_t i : collectiblesBySlot[curSlot]) {
            const e0net::WorldObject* po = net.object(i);
            if (!po) continue;
            const e0net::WorldObject& o = *po;
            float dx = o.xcm * 0.01f - me.x, dz = o.zcm * 0.01f - me.z;
            if (std::fabs(dx) > kExit || std::fabs(dz) > kExit) continue;
            float dy = o.ycm * 0.01f - me.y;
            if (dy > 1.6f || dy < -1.6f) continue;
            { auto lh = collectLocalHide.find(i); if (net.isTaken(i) || (lh != collectLocalHide.end() && lh->second > 0)) continue; }
            float d = std::sqrt(dx * dx + dz * dz);
            if (d < bestD) { bestD = d; best = (int)i; }
            if ((int)i == collectCur) curD = d;
        } else collectCur = -1;
        if (collectCur >= 0 && curD > kExit) collectCur = -1;
        if (best >= 0 && bestD < kEnter && (collectCur < 0 || (best != collectCur && bestD + 0.35f < curD))) { collectCur = best; curD = bestD; }
        if (collectCur >= 0 && W) {
            const e0net::WorldObject& o = *net.object((uint32_t)collectCur);
            float cx = o.xcm * 0.01f, cz = o.zcm * 0.01f;
            float cy = e0net::terrain().heightAt(cx, cz) + 0.02f;
            collectType = o.type; collectPos = {cx, cy, cz}; collectDist = curD; collectLabel = collectName(o.type);
        }
        // itens SOLTOS no chão (descartados/dropados): mesmo indicador e botão COLETAR; vale o mais próximo (com histerese)
        {
            int bestDrop = -1; float bestDD = 1e9f, curDD = 1e9f; const NetClient::DropInfo* cur = nullptr;
            const bool swim = e0net::isSwimmingPos(me.x, me.y, me.z);
            for (const auto& d : net.drops()) {
                float dx = d.x - me.x, dz = d.z - me.z, dy = d.y - me.y;
                if (swim || dy > 1.6f || dy < -1.6f) continue;
                float dd = std::sqrt(dx * dx + dz * dz);
                if (dd < bestDD) { bestDD = dd; bestDrop = d.id; }
                if (d.id == collectDropId) { curDD = dd; cur = &d; }
            }
            if (!cur) collectDropId = -1;
            else if (curDD > kExit) { collectDropId = -1; cur = nullptr; }
            if (bestDrop >= 0 && bestDD < kEnter && (collectDropId < 0 || (bestDrop != collectDropId && bestDD + 0.35f < curDD))) { collectDropId = bestDrop; curDD = bestDD; cur = nullptr; }
            if (collectCur >= 0 && collectDropId >= 0) { if (curDD < collectDist) collectCur = -1; else collectDropId = -1; }
            if (collectDropId >= 0) {
                for (const auto& d : net.drops()) if (d.id == collectDropId) {
                    collectPos = {d.x, d.y + 0.02f, d.z}; collectDist = curDD; std::snprintf(collectLabelBuf, sizeof collectLabelBuf, "%s X%d", itemDef(d.item).name, (int)d.n); collectLabel = collectLabelBuf; break;
                }
            }
        }
        if (net.respawnGeneration() != lastRespawnGen) {
            if (lastRespawnGen != 0xFFFFFFFFu) notify("OS RECURSOS DO MAPA RENASCERAM");
            lastRespawnGen = net.respawnGeneration();
        }
        // árvores/pedras/toras que acabaram (bit do servidor): somem do terreno (com efeito se acontece perto e ao vivo) e perdem a colisão
        if (W) {
            harvestWarm += dt; if (net.connected()) harvestConnT += dt;
            if (!harvestInit && (harvestConnT > 1.0f || harvestWarm > 6.0f)) harvestInit = true;
            if (net.takenGeneration() != lastHarvestGen) {
                lastHarvestGen = net.takenGeneration();
                for (int sl = 0; sl < e0net::kIslandSlots; ++sl) {
                    for (uint32_t i : actionablesBySlot[sl]) {
                        if (!net.isTaken(i) && harvestDone.count(i)) {   // renasceu (meia-noite): volta a aparecer e a ter colisão
                            harvestDone.erase(i);
                            renderer.restoreWorldProp(i);
                            if (e0net::World* WM = net.islandMutable(e0net::gidSlot(i))) WM->clearRemoved(e0net::gidIdx(i));
                            continue;
                        }
                        if (!net.isTaken(i) || harvestDone.count(i)) continue;
                        harvestDone.insert(i);
                        const e0net::WorldObject* po = net.object(i);
                        if (!po) continue;
                        const e0net::WorldObject& o = *po;
                        float hdx = o.xcm * 0.01f - me.x, hdz = o.zcm * 0.01f - me.z;
                        renderer.removeWorldProp(i, harvestInit && hdx * hdx + hdz * hdz < 90.0f * 90.0f);
                        if (e0net::World* WM = net.islandMutable(e0net::gidSlot(i))) WM->setRemoved(e0net::gidIdx(i));
                    }
                }
            }
        }
        // pegada do item solto: na hora (mão no chão) pede ao servidor; desiste se o jogador se afastou ou o item sumiu
        if (dropPendId >= 0) {
            dropPendT -= dt;
            bool exists = false; float ddx = 0, ddz = 0;
            for (const auto& d : net.drops()) if (d.id == dropPendId) { exists = true; ddx = d.x - me.x; ddz = d.z - me.z; }
            if (!exists || ddx * ddx + ddz * ddz > kExit * kExit) dropPendId = -1;
            else if (dropPendT <= 0) { net.requestAct(e0net::ActPickupDrop, (uint16_t)dropPendId, 0); dropPendId = -1; }
        }
        // dano de outros jogadores (o servidor decide o acerto; a vida é minha): desconta, treme a câmera e pisca a tela
        for (NetClient::Damage d; net.popDamage(d); ) {
            const bool head = d.amount >= e0net::kGunHeadDamage;   // tiro na cabeça: morte certa
            if (!selfDead && !godLife) health = head ? 0.0f : std::max(0.0f, health - (float)d.amount);
            hitFlashT = head ? 0.9f : 0.40f; rig.addTrauma(head ? 0.7f : 0.30f);
            char t[48];
            if (head) std::snprintf(t, sizeof t, "TIRO NA CABECA! (ID %d)", (int)d.from);
            else std::snprintf(t, sizeof t, "LEVOU %d DE DANO (ID %d)", (int)d.amount, (int)d.from);
            notify(t);
        }
        updateChat(dt);
        updateTeamGame(dt);
        updateConquest(dt);
        updateIsle(dt);
        updateTeamCoop(dt);
        teamPushTick(dt);
        if (teamOpen) updateTeam(dt);
        updateNav(dt);
        updateFlag(dt);
        for (NetClient::HitConfirm hc; net.popHitConfirm(hc); ) {
            const bool head = hc.amount >= e0net::kGunHeadDamage;
            rig.addTrauma(0.08f);
            armaHitT = 0.35f; armaHitHead = head;
            audio.playClick(head ? 1.0f : 0.7f, head ? 1.9f : 1.5f, 0.0f);   // "tic" de acerto
            char t[48];
            if (head) std::snprintf(t, sizeof t, "TIRO NA CABECA! ID %d", (int)hc.target);
            else std::snprintf(t, sizeof t, "ACERTOU ID %d  -%d", (int)hc.target, (int)hc.amount);
            notify(t);
        }
        // resultados das ações de inventário em rede + entregas recebidas
        for (NetClient::ActResult ar; net.popActResult(ar); ) {
            char t[40];
            if (ar.kind == e0net::ActDrop) {
                if (ar.result) { std::snprintf(t, sizeof t, "SOLTOU X%d %s", (int)ar.b, itemDef(ar.item).name); notify(t); }
                else { if (!invServer()) invAdd(ar.item, ar.b); notify("NAO DA PARA SOLTAR AQUI"); }
            } else if (ar.kind == e0net::ActBuyTeamLicense) {   // LICENÇA DE TRIPULAÇÃO comprada na LOJA
                const char* m = ar.result ? "LICEN\xC7" "A COMPRADA: CRIE A TRIPULA\xC7\xC3" "O NO MENU" : e0net::teamResultText((uint8_t)ar.other);
                notify(m); if (shopOpen) shopSay(m);
            } else if (ar.kind == e0net::ActBuyShip) {
                if (ar.result) { std::snprintf(t, sizeof t, "COMPROU O %s! CHAME PELA NAVEGACAO", e0net::shipDef(ar.item).name); notify(t); navSay("NAVIO COMPRADO! TOQUE EM CHAMAR"); }
                else notify(ar.other == 3 ? "MOEDAS INSUFICIENTES" : (ar.other == 2 ? "VOCE JA TEM ESSE NAVIO" : "NAO DA PARA COMPRAR AGORA"));
            } else if (ar.kind == e0net::ActPotion) {
                if (!ar.result) {   // o servidor recusou: devolve a poção (online: volta com o inventário do servidor)
                    if (!invServer()) invAdd(ar.a == e0net::PotionSpeed ? ItPotSpeed : ItPotShield, 1);
                    (ar.a == e0net::PotionSpeed ? potSpeedT : potShieldT) = 0.0f; notify("POCAO RECUSADA");
                }
            } else if (ar.kind == e0net::ActPickupDrop) {
                if (ar.result && invServer()) notify(nullptr, ar.item, (int)ar.n);   // online: o que coube já está no inventário do servidor
                else if (ar.result) {
                    int left = invAdd(ar.item, ar.n);
                    if (left > 0) net.requestAct(e0net::ActDrop, ar.item, (uint16_t)left);
                    if ((int)ar.n - left > 0) notify(nullptr, ar.item, (int)ar.n - left);
                } else notify(ar.other == 1 ? "INVENTARIO CHEIO" : "ITEM NAO ESTA MAIS LA");
            } else if (ar.kind == e0net::ActChestPut) {   // guardar: o que não coube volta para a mochila
                const int stored = ar.result ? (int)ar.n : 0, left = (int)(ar.b & 0xFF) - stored;
                if (left > 0 && !invServer()) { int rest = invAdd(ar.item, left); if (rest > 0) net.requestAct(e0net::ActDrop, ar.item, (uint16_t)rest); }
                if (stored > 0) { std::snprintf(t, sizeof t, "GUARDOU X%d %s", stored, itemDef(ar.item).name); if (!chestUi) notify(t); }
                else chestSay("BAU CHEIO OU LONGE DEMAIS");
            } else if (ar.kind == e0net::ActChestTake) {  // pegar: o que não couber na mochila volta para o baú
                if (ar.result && invServer()) { if (!chestUi) notify(nullptr, ar.item, (int)ar.n); }   // online: o servidor já pôs na mochila
                else if (ar.result) {
                    int left = ar.n;
                    if (itemDef(ar.item).maxStack == 1)   // ferramentas voltam para a barra de acesso rápido, se houver lugar
                        for (int i = kInvHotFirst; i < kInvSlots && left > 0; ++i) if (!inv[i].id) { inv[i] = {ar.item, 1}; --left; }
                    left = invAdd(ar.item, left);
                    if (left > 0) net.requestAct(e0net::ActChestPut, (uint16_t)(ar.item | (ar.a & 0xFF00)), (uint16_t)left);
                    if ((int)ar.n - left > 0 && !chestUi) notify(nullptr, ar.item, (int)ar.n - left);
                } else chestSay("NAO DEU PARA PEGAR");
            } else if (ar.kind == e0net::ActHit) {
                if (ar.result) rig.addTrauma(ar.result == 2 ? 0.22f : 0.10f);
                else if (ar.other == 2 && toolWarnT <= 0) { notify("FERRAMENTA ERRADA"); toolWarnT = 1.5f; }
            } else if (ar.kind == e0net::ActRevive || ar.kind == e0net::ActCarry || ar.kind == e0net::ActDropCarry) {
                if (!ar.result) notify(ar.kind == e0net::ActDropCarry ? "NAO DA PARA LARGAR NA AGUA" : "NAO DA PARA AGORA");
            } else if (ar.kind == e0net::ActGive || ar.kind == e0net::ActGiveTo) {
                if (ar.result) { std::snprintf(t, sizeof t, "ENTREGOU X%d %s", (int)ar.b, itemDef(ar.item).name); notify(t); }
                else { if (!invServer()) invAdd(ar.item, ar.b); notify("NINGUEM PERTO PARA ENTREGAR"); }
            } else if (ar.kind == e0net::ActCraft) {
                if (!ar.result) notify(ar.other == 2 ? "INVENTARIO CHEIO (SERVIDOR)" : "FALTAM INGREDIENTES (SERVIDOR)");
            } else if (ar.kind == e0net::ActAdReward) {
                char m[48];
                if (ar.result) std::snprintf(m, sizeof m, "+%d MOEDAS PELO ANUNCIO", (int)ar.n);
                else std::snprintf(m, sizeof m, "%s", ar.other == 2 ? "LIMITE DE ANUNCIOS DE HOJE" : "ESPERE UM POUCO PARA O PROXIMO");
                if (shopOpen) shopSay(m); else notify(m);
            }
        }
        for (NetClient::Gift gf; net.popGift(gf); ) {
            if (!gf.item || gf.item >= ItCount) continue;
            if (!invServer()) { int left = invAdd(gf.item, gf.n); if (left > 0) net.requestAct(e0net::ActDrop, gf.item, (uint16_t)left); }   // sem espaço: cai aos pés (online o servidor já fez isso)
            char t[40]; std::snprintf(t, sizeof t, "JOGADOR %d ENTREGOU X%d %s", (int)gf.from + 1, (int)gf.n, itemDef(gf.item).name);
            notify(t);
        }
        {   // desenha os itens soltos
            std::vector<DropVis> dv;
            for (const auto& d : net.drops()) {
                const ItemDef& df = itemDef(d.item);
                Vec3 sz{0.20f, 0.20f, 0.20f}; int mdl = 0, shine = 0;
                switch (d.item) {
                    case ItStick: mdl = 1; break;
                    case ItPebble: mdl = 2; break;
                    case ItWood: mdl = 4; break;
                    case ItStone: mdl = 3; break;
                    case ItCoconut: mdl = 7; break;
                    case ItFrond: mdl = 8; break;
                    case ItHatchet: mdl = 5; break;
                    case ItPickaxe: mdl = 6; break;
                    case ItTorch: mdl = 9; break;
                    case ItDagger: mdl = 10; break;
                    case ItGun: mdl = 11; break;
                    case ItGold: mdl = 2; shine = 1; break;
                    case ItDiamond: mdl = 2; shine = 2; break;
                    case ItIron: mdl = 2; break;
                }
                dv.push_back({{d.x, d.y, d.z}, (float)(d.id % 16) * 0.39f, sz, {df.r, df.g, df.b}, mdl, d.id, shine, 1.5f});
            }
            addCannonballs(dv);
            renderer.setDrops(dv);
        }
        float target = (collectCur >= 0 || collectDropId >= 0) ? 1.0f : 0.0f;
        collectAlpha += (target - collectAlpha) * (1.0f - std::exp(-dt * 12.0f));
        if (target == 0.0f && collectAlpha < 0.004f) collectAlpha = 0.0f;
    }
    // ponto do mundo -> pixel (false se atrás da câmera)
    bool project(const Vec3& p, float& px, float& py) const {
        Mat4 vp = camera.projection(h > 0 ? (float)w / (float)h : 1.0f) * camera.view();
        const float* m = vp.m;
        float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (cw < 0.05f) return false;
        px = (cx / cw * 0.5f + 0.5f) * w; py = (1.0f - (cy / cw * 0.5f + 0.5f)) * h;
        return true;
    }
    bool inRunButton(float x, float y) const {
        float dx = x - runX(), dy = y - runY(), r = runR() * 1.35f;
        return dx * dx + dy * dy <= r * r;
    }

#ifdef E0_CINEMA
#include "engine_cinema.inc"
#endif
#include "engine_shop.inc"   // LOJA (só visual/fictícia)
#include "engine_character.inc"   // TELA PERSONAGEM (skins)
#include "engine_navigation.inc"   // NAVEGAÇÃO (chamar navio)
#include "engine_flag.inc"         // BANDEIRA PIRATA (editor)
#include "engine_push.inc"         // MENSAGENS PUSH dentro do jogo (banner do tema)
#include "engine_account.inc"      // CONTA + INVENTÁRIO/MOEDAS DO SERVIDOR (v42)
#include "engine_team.inc"         // TRIPULAÇÃO (tela do menu principal; serviço TCP do servidor)
#include "engine_conquest.inc"     // DOMINAÇÃO DE ILHAS (mastro, painel, bandeiras no mapa)
#include "engine_isle.inc"         // MELHORIAS DA ILHA (construir nas ilhas dominadas, vista de cima)

    bool initDisplay() {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(dpy, nullptr, nullptr);
#ifdef E0_CINEMA
        {   // a superfície do encoder exige EGL_RECORDABLE_ANDROID; MSAA 4x se houver
            const EGLint rec[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                  0x3142 /*EGL_RECORDABLE_ANDROID*/, 1, EGL_SAMPLE_BUFFERS, 1, EGL_SAMPLES, 4, EGL_NONE};
            const EGLint rec1[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                   0x3142, 1, EGL_NONE};
            EGLint nc = 0;
            eglChooseConfig(dpy, rec, &cfg, 1, &nc);
            LOG("cinema: EGLConfig gravavel com MSAA 4x: %s", nc > 0 ? "sim" : "nao");
            if (nc < 1) eglChooseConfig(dpy, rec1, &cfg, 1, &nc);
            if (nc > 0) {
                const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
                ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
                return ctx != EGL_NO_CONTEXT;
            }
        }
#endif
        const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
        EGLint n = 0;
        eglChooseConfig(dpy, attrs, &cfg, 1, &n);
        if (n < 1) { LOG("sem EGLConfig ES3"); return false; }
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
        return ctx != EGL_NO_CONTEXT;
    }

    // ---- TELA DE LOADING (assets/loading.e0i; loading.h): cobre tudo até o jogo estar de fato pronto ----
    LoadingScreen loadScr;
    bool loadActive = true; float loadT = 0, loadFade = 1.0f; int loadWarm = 0; const char* loadStage = "INICIANDO";
    void loadFrame(const char* stage) {   // um quadro imediato durante as etapas que bloqueiam a thread (o círculo avança um pouco a cada quadro)
        loadStage = stage; loadT += 0.12f;
        if (!loadScr.ready()) return;
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        loadScr.draw(w, h, stage, loadT, 1.0f);
        eglSwapBuffers(dpy, surf);
    }
    // Etapa real do carregamento, deduzida do estado da rede/mundo; termina quando o mundo está aplicado, a conexão pronta e alguns quadros já rodaram.
    void updateLoading(float dt) {
        if (!loadActive) return;
        loadT += dt;
        if (loadFade < 1.0f) { loadFade -= dt / 0.6f; if (loadFade <= 0.0f) { loadActive = false; loadScr.shutdown(); } return; }
        const int ph = net.worldPhase();
        bool done = false;
        if (!net.worldReady()) {
            loadWarm = 0;
            if (ph == 1) loadStage = "BAIXANDO O MUNDO";
            else if (ph == 5) loadStage = "SERVIDOR SEM RESPOSTA, TENTANDO DE NOVO";
            else loadStage = net.state() == NetClient::State::Offline ? "GERANDO A ILHA" : "CONECTANDO AO SERVIDOR";
        } else if (net.worldGeneration() != lastWorldGen) { loadStage = "CONSTRUINDO O TERRENO"; loadWarm = 0; }
        else if (net.state() == NetClient::State::Connecting) { loadStage = "ENTRANDO NO SERVIDOR"; loadWarm = 0; }
        else { loadStage = "PREPARANDO O MAR"; done = ++loadWarm >= 45 && loadT >= 1.5f; }
        if (loadT > 25.0f) done = true;   // nunca prende o jogador (servidor fora do ar etc.)
        if (done) {
            loadFade = 0.999f; LOG("loading concluido em %.1f s", loadT);
#ifndef E0_CINEMA
            startIntro();
#endif
        }
    }
    void drawLoading() { if (loadActive && loadScr.ready()) loadScr.draw(w, h, loadStage, loadT, loadFade); }

    // ---- MENU PRINCIPAL (main_menu.h; atlas assets/menu.e0i): aparece ANTES de conectar ao servidor. JOGAR inicia a conexão e a tela de loading;
    // CONFIGURAÇÕES (qualidade, filtro pirata, som) e SAIR ficam no menu. Não roda no modo cinema.
    MainMenu menu;
    bool menuActive = false, menuDone = false, gameStarted = false, soundOn = true;
    std::string serverOverride, cfgPath;   // servidor digitado no menu ("ip[:porta]", vazio = o embutido no app) e arquivo das configurações do menu
    static std::string builtinServer() {
#ifdef E0_SERVER_HOST
        return E0_SERVER_HOST;
#else
        return std::string();
#endif
    }
    void beginGame() {   // conecta ao servidor (ou abre o mundo offline)
        if (gameStarted) return;
        gameStarted = true;
        const std::string host = serverOverride.empty() ? builtinServer() : serverOverride;
        std::string ip; int port = e0net::kDefaultPort;
        if (!host.empty() && MainMenu::parseServer(host, ip, port) && net.start(ip.c_str(), (uint16_t)port)) {
            LOG("servidor: %s:%d", ip.c_str(), port);
            netGame = true; invEverAdopted = false; kickedShown = false; vitalsSentHp = vitalsSentHu = -1;
            configureLogin();   // a conta (Firebase/visitante) entra no servidor depois do Welcome
            return;
        }
        netGame = false;
        net.startOffline();   // sem servidor válido => offline (mundo local)
    }
    // Configurações do menu salvas em <dados internos>/menu.cfg (chave=valor): quality, grade, sound, server
    void loadCfg() {
        if (cfgPath.empty()) return;
        FILE* f = std::fopen(cfgPath.c_str(), "r");
        if (!f) return;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "quality") qMode = std::max(-1, std::min(2, std::atoi(v.c_str())));
            else if (k == "grade") gradeMode = std::max(0, std::min(2, std::atoi(v.c_str())));
            else if (k == "sound") soundOn = v != "0";
            else if (k == "skin") skinSel = std::max(0, std::min(e0net::kSkinCount - 1, std::atoi(v.c_str())));
            else if (k == "flag") { FlagDesign fd; if (flagFromCfg(v, fd)) flagSaved = flagEdit = fd; }
            else if (k == "flagsync") flagUnsynced = v == "0";
            else if (k == "walletmig") walletNoticeDone = v == "1";
            else if (k == "server") { std::string ip; int pt; if (v.empty() || MainMenu::parseServer(v, ip, pt)) serverOverride = v; }
        }
        std::fclose(f);
        if (qMode >= 0) qCur = qMode;
        net.setSkin(skinSel);
    }
    void saveCfg() {
        if (cfgPath.empty()) return;
        if (FILE* f = std::fopen(cfgPath.c_str(), "w")) {
            std::fprintf(f, "quality=%d\ngrade=%d\nsound=%d\nskin=%d\nserver=%s\nflag=%s\n", qMode, gradeMode, soundOn ? 1 : 0, skinSel, serverOverride.c_str(), flagToCfg(flagSaved).c_str());
            std::fprintf(f, "flagsync=%d\nwalletmig=%d\n", flagUnsynced ? 0 : 1, walletNoticeDone ? 1 : 0);
            std::fclose(f);
        }
    }
    // ---- CARTEIRA (moedas + recibos de compra), SÓ NESTE APARELHO: <dados internos>/wallet.cfg ----
    // Antes as moedas eram só memória (sumiam ao fechar o app e ao morrer). Agora: salvas a cada mudança (checagem barata por quadro
    // em updateWallet), com os recibos da Play (purchaseToken) para (1) nunca entregar a mesma compra 2x (app fechado entre a entrega e
    // o consume) e (2) mandar ao servidor (MsgAccount) quando houver validação de verdade (ver CLAUDE.md "Contas e recibos").
    // Não é seguro contra quem edita o arquivo (root): moedas valendo de verdade precisam morar no servidor.
    struct Receipt { std::string sku, token, order; bool test = false, sent = false, done = false; };
    std::vector<Receipt> receipts;
    std::string walletPath; int walletSaved = -1; bool walletLoaded = false;
    static uint32_t walletSum(int c) { uint32_t x = (uint32_t)c * 2654435761u ^ 0x5EA7C0DEu; x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; return x; }   // só pega corrupção/edição ingênua
    bool walletHasToken(const std::string& t) const { for (const auto& r : receipts) if (r.token == t) return true; return false; }
    void walletAddReceipt(const std::string& sku, const std::string& tok, const std::string& order, bool test) {
        receipts.push_back({sku, tok, order, test, false, false});
        if (receipts.size() > 200) receipts.erase(receipts.begin());
    }
    void loadWallet() {
        walletLoaded = true;
        if (walletPath.empty()) return;
        FILE* f = std::fopen(walletPath.c_str(), "r");
        if (!f) { walletSaved = coins; return; }
        char line[1024]; int c = 0; uint32_t sum = 0; bool hasC = false;
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "coins") { c = std::max(0, std::atoi(v.c_str())); hasC = true; }
            else if (k == "sum") sum = (uint32_t)std::strtoul(v.c_str(), nullptr, 16);
            else if (k == "receipt") {   // sku|token|pedido|teste|enviado
                std::vector<std::string> p; size_t a = 0;
                for (;;) { size_t b = v.find('|', a); p.push_back(v.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
                while (p.size() < 5) p.emplace_back();
                if (!p[1].empty()) receipts.push_back({p[0], p[1], p[2], p[3] == "1", p[4] == "1", false});
            }
        }
        std::fclose(f);
        if (hasC && sum == walletSum(c)) coins = c;
        else if (hasC) LOG("carteira: soma de verificacao invalida, moedas descartadas");
        walletSaved = coins;
        LOG("carteira carregada: %d moedas, %zu recibos", coins, receipts.size());
    }
    void saveWallet() {
        if (walletPath.empty() || !walletLoaded) return;
        const std::string tmp = walletPath + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        const int wc = netGame ? std::max(0, walletSaved) : coins;   // online o saldo da tela é o do servidor: o arquivo guarda o saldo OFFLINE
        std::fprintf(f, "coins=%d\nsum=%08x\n", wc, walletSum(wc));
        for (const auto& r : receipts) std::fprintf(f, "receipt=%s|%s|%s|%d|%d\n", r.sku.c_str(), r.token.c_str(), r.order.c_str(), r.test ? 1 : 0, r.sent ? 1 : 0);
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        if (ok) std::rename(tmp.c_str(), walletPath.c_str());   // gravação atômica
        walletSaved = wc;
    }
    void updateWallet() {   // 1 comparação por quadro: grava só quando o saldo muda. ONLINE as moedas são do servidor (wallet.cfg guarda só os recibos)
        if (walletLoaded && !netGame && coins != walletSaved) saveWallet();
    }
    void enterMenu() {   // chamado no fim de initSurface: o menu adota a textura da arte da tela de loading
        int iw = 0, ih = 0;
        unsigned tex = loadScr.takeImage(iw, ih);
        loadScr.shutdown(); loadActive = false;
        menu.opts.quality = qMode; menu.opts.grade = gradeMode; menu.opts.sound = soundOn; menu.opts.server = serverOverride; menu.optsDirty = false;
        if (tex && menu.init(app->activity->assetManager, app->activity, tex, iw, ih, builtinServer())) {
            menuActive = true;
            menu.account = auth.signedIn() ? (int)auth.account().provider : 0;
            if (!auth.signedIn()) { menu.showLogin(); LOG("tela de login aberta (sem conta salva)"); }   // LOGIN antes do menu
            else LOG("menu principal aberto (conta %s)", auth.account().provider == Auth::Google ? "GOOGLE" : auth.account().provider == Auth::Email ? "E-MAIL" : "VISITANTE");
            return;
        }
        LOG("menu indisponivel: seguindo direto para o jogo");
        menuDone = true; beginGame();
        loadActive = true; loadT = 0; loadFade = 1.0f; loadWarm = 0;
        loadScr.init(app->activity->assetManager);
    }
    void leaveMenu() {   // JOGAR: some o menu, conecta e mostra a tela de loading (mesma arte de fundo)
        menu.shutdown(); menuActive = false; menuDone = true;
        beginGame();
        loadActive = true; loadT = 0; loadFade = 1.0f; loadWarm = 0; loadStage = "CONECTANDO AO SERVIDOR";
        loadScr.init(app->activity->assetManager);
    }
    // Botão VOLTAR do Android: no jogo ou no loading volta ao MENU PRINCIPAL (desconecta do servidor; JOGAR conecta de novo e recomeça a
    // sessão: vida, fome, inventário inicial e a intro). No menu: sai de CONFIGURAÇÕES/CRÉDITOS; no menu principal deixa o Android fechar o app.
    bool backHandled() const {
#ifdef E0_CINEMA
        return false;
#else
        return ready && (!menuActive || charOpen || teamOpen || menu.canGoBack());
#endif
    }
    void doBack() {
        if (menuActive) { if (charOpen) closeCharScreen(); else if (teamOpen) { if (flagOpen) flagTryClose(); else closeTeam(); } else menu.goBack(); return; }
        if (navOpen) { closeNav(); return; }
        if (flagOpen) { flagTryClose(); return; }
        if (teamOpen) { closeTeam(); return; }
        returnToMenu();
    }
    void returnToMenu() {
        LOG("voltar: indo para o menu principal");
        net.stop(); gameStarted = false;   // manda MsgLeave e encerra a conexão/download do mundo
        selfDead = sawDeadFlag = false; deadT = wakeFadeT = 0.0f; health = 100.0f; hunger = 100.0f; hungerWarnLow = hungerWarnZero = false;
        gunnerMode = false; isleMode = false; invOpen = craftOpen = mapOpen = false; closeShop(); closeTeam(); teamUnread = 0; wpCrew = -1;
        for (ItemStack& st : inv) st = ItemStack{};
        inv[kInvHotFirst] = {ItHatchet, 1}; inv[kInvHotFirst + 1] = {ItPickaxe, 1}; inv[kInvHotFirst + 2] = {ItTorch, 1}; inv[kInvHotFirst + 3] = {ItDagger, 1}; inv[kInvHotFirst + 4] = {ItGun, 1}; inv[0] = {ItAmmo, 20}; armaLoaded = true; armaReloadT = -1.0f;
        introActive = false; introPlayed = false; renderer.setQuality(qCur);
        loadActive = false; menuDone = false;
        if (!loadScr.ready()) loadScr.init(app->activity->assetManager);   // o menu adota a textura da arte da tela de loading
        enterMenu();
    }
    void updateMenu(float dt) {
        updatePush(dt);
        if (charOpen) { updateCharScreen(dt); audio.setAmbience(0.0f, 0.0f); return; }   // tela PERSONAGEM por cima do menu (o menu fica parado)
        teamPushTick(dt);
        if (teamOpen) { if (flagOpen) updateFlag(dt); updateTeam(dt); audio.setAmbience(0.0f, 0.0f); return; }   // tela TRIPULAÇÃO (e o editor da bandeira dela)
        menu.update(dt);
        if (menu.optsDirty) {
            menu.optsDirty = false;
            qMode = menu.opts.quality;
            if (qMode >= 0) { qCur = qMode; renderer.setQuality(qCur); }
            slowCnt = fastCnt = 0;
            gradeMode = menu.opts.grade; renderer.setGrade(gradeMode == 0 ? 0.0f : (gradeMode == 1 ? 1.0f : 1.3f));
            soundOn = menu.opts.sound; audio.setMaster(soundOn ? 0.9f : 0.0f);
            serverOverride = menu.opts.server;
            saveCfg();
        }
        audio.setAmbience(0.0f, 0.0f);   // menu: só a trilha sonora
        {   // CONTA: resultado dos pedidos de login (Java/Firebase ou visitante local)
            Auth::Fail f;
            const Auth::Result r = auth.poll(f);
            if (r == Auth::SignedIn) { menu.account = (int)auth.account().provider; if (menu.onLogin()) menu.loginDone(); }
            else if (r == Auth::Failed) menu.loginMessage(f == Auth::FailNotConfigured ? 1 : f == Auth::FailCanceled ? 3 : f == Auth::FailNetwork ? 4
                                                          : f == Auth::FailCredential ? 5 : f == Auth::FailEmailInvalid ? 6 : f == Auth::FailTooMany ? 7 : 2);
        }
        switch (menu.poll()) {
            case MainMenu::Action::Play: leaveMenu(); break;
            case MainMenu::Action::LoginGoogle: menu.loginBusy(true); auth.signInGoogle(); break;
            case MainMenu::Action::LoginGuest: menu.loginBusy(true); auth.signInGuest(); break;
            case MainMenu::Action::LoginEmail: {   // e-mail e senha digitados nas caixas do Android: a senha vai direto ao Firebase e é apagada aqui
                std::string em, pw;
                if (menu.takeEmailLogin(em, pw)) { menu.loginBusy(true); auth.signInEmail(em, pw); }
                for (char& c : pw) c = 0;
                break;
            }
            case MainMenu::Action::Logout: auth.signOut(); menu.account = 0; menu.showLogin(); break;
            case MainMenu::Action::Character: openCharScreen(); break;
            case MainMenu::Action::Team: openTeam(); break;
            case MainMenu::Action::Quit: ANativeActivity_finish(app->activity); break;
            default: break;
        }
    }
    void drawMenu() {
        if (charOpen) { drawCharScreen(); return; }
        if (teamOpen) { drawTeamScreen(); drawPushOverlay(); return; }
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        menu.draw(w, h);
        drawPushOverlay();   // mensagem push / in-app por cima do menu
    }

    // ---- INTRO "QUEDA DO CEU" (estilo GTA V): ao entrar, a câmera começa a ~360 m de altura olhando para baixo, girando, e mergulha
    // até a câmera normal atrás do jogador. Para não pesar: a qualidade vai a BAIXA (sombra/reflexo/alcance mínimos) enquanto está longe e
    // volta ao normal em kIntroSwap da descida, ESCONDIDA por nuvens/névoa (véu branco) que passam pela tela; faixas de cinema entram e saem.
    // Joystick/ações travados; um toque pula (vai direto ao fim). Toca 1 vez por execução do app (não repete ao girar/recriar a janela).
    static constexpr float kIntroDur = 6.5f, kIntroSwap = 0.68f, kIntroHeight = 360.0f;
    bool introActive = false, introPlayed = false, introSwapped = false; float introT = 0.0f;
    void startIntro() {
        if (introPlayed) return;
        introPlayed = true; introActive = true; introSwapped = false; introT = 0.0f;
        renderer.setQuality(0);   // escondido pela tela de loading (ainda opaca neste instante)
        LOG("intro: queda do ceu (%.1f s)", kIntroDur);
    }
    void updateIntro(float dt) {
        if (!introActive) return;
        introT += dt;
        if (!introSwapped && introT >= kIntroDur * kIntroSwap) { renderer.setQuality(qCur); introSwapped = true; slowCnt = fastCnt = 0; }
        if (introT >= kIntroDur) { introActive = false; if (!introSwapped) renderer.setQuality(qCur); }
    }
    static float smoother(float x) { x = std::max(0.0f, std::min(1.0f, x)); return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
    void introCamera(Camera& c) const {   // mistura a câmera normal (c) com a vista aérea
        const float u = std::min(1.0f, introT / kIntroDur), e = smoother(u);
        Camera a = c;
        a.position = Vec3{c.position.x, c.position.y + kIntroHeight, c.position.z};
        a.yaw = c.yaw + 2.2f;
        a.pitch = -1.50f; a.fovY = 1.15f;
        c.position = a.position + (c.position - a.position) * e;
        c.yaw = c.yaw + 2.2f * (1.0f - e);
        c.pitch = a.pitch + (c.pitch - a.pitch) * e;
        c.fovY = a.fovY + (c.fovY - a.fovY) * e;
        c.roll += std::sin(u * 4.0f) * 0.035f * (1.0f - e);
        c.zFar = std::max(c.zFar, 900.0f);
    }
    void drawIntroOverlay() {
        renderer.beginUi();
        const float u = std::min(1.0f, introT / kIntroDur), d = (u - kIntroSwap) / 0.10f;
        const float bell = std::exp(-d * d);                          // pico exatamente na troca de qualidade
        const float strength = std::min(1.0f, 0.30f * (1.0f - u) * (1.0f - u) + 0.95f * bell);   // névoa de altitude + nuvens na troca
        for (int i = 0; i < 9; ++i) {   // nuvens: manchas macias que correm pela tela
            const float sx = (float)((i * 37 + 11) % 100) / 100.0f, sy = (float)((i * 53 + 29) % 100) / 100.0f, rr = (float)((i * 19 + 7) % 100) / 100.0f;
            const float span = w * 1.5f;
            const float cx = std::fmod(sx * span + introT * (40.0f + i * 14.0f) * ((float)h / 1080.0f), span) - w * 0.25f;
            const float hw = h * (0.55f + 0.6f * rr);
            renderer.uiGlow(cx, sy * h, hw, hw * 0.55f, 0.96f, 0.96f, 0.98f, strength * 0.55f);
        }
        if (bell > 0.01f) renderer.uiGlow(w * 0.5f, h * 0.5f, w * 0.8f, h * 0.9f, 0.97f, 0.97f, 1.0f, bell * 0.6f);   // véu branco no pico
        const float bar = h * 0.085f * smoother(introT / 0.6f) * (1.0f - smoother((u - 0.80f) / 0.20f));   // faixas de cinema
        if (bar > 0.5f) {
            renderer.uiRoundRect(w * 0.5f, bar * 0.5f, w * 0.5f, bar * 0.5f, 1.0f, 0, 0, 0, 0.95f);
            renderer.uiRoundRect(w * 0.5f, h - bar * 0.5f, w * 0.5f, bar * 0.5f, 1.0f, 0, 0, 0, 0.95f);
        }
        renderer.endUi();
    }

    bool initSurface() {
        surf = eglCreateWindowSurface(dpy, cfg, app->window, nullptr);
        if (!eglMakeCurrent(dpy, surf, surf, ctx)) { LOG("eglMakeCurrent falhou"); return false; }
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        loadActive = true; loadT = 0; loadFade = 1.0f; loadWarm = 0;
        loadScr.init(app->activity->assetManager);
        loadFrame("INICIANDO OS GRAFICOS");
        renderer.setAssetManager(app->activity->assetManager);
        renderer.setQuality(qCur);
        if (!renderer.init()) return false;
        loadFrame("CARREGANDO MODELOS E TEXTURAS");
        renderer.setGrade(gradeMode == 0 ? 0.0f : (gradeMode == 1 ? 1.0f : 1.3f));
        if (!audio.ready()) { loadFrame("CARREGANDO OS SONS"); audio.init(app->activity->assetManager); audio.startMusic(app->activity->assetManager); }
        audio.setMaster(soundOn ? 0.9f : 0.0f);
        // 60 fps fixos: carga previsível (o controle de qualidade depende disso), menos calor e bateria
        using SetFrameRateFn = int32_t (*)(ANativeWindow*, float, int8_t);  // API 30+: carrega em runtime
        if (auto fn = (SetFrameRateFn)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate")) fn(app->window, 60.0f, 1 /*FIXED_SOURCE*/);
        renderer.resize(w, h);
        for (int id = 1; id < ItCount; ++id) if (std::fabs(itemDef(id).kg - e0net::itemKg(id)) > 1e-4f) LOG("ERRO: peso do item %d diverge do servidor (%.2f x %.2f)", id, itemDef(id).kg, e0net::itemKg(id));
        for (int id = 1; id < ItCount; ++id) if (itemDef(id).maxStack != e0net::itemMaxStack(id)) LOG("ERRO: pilha maxima do item %d diverge do servidor (%d x %d)", id, itemDef(id).maxStack, (int)e0net::itemMaxStack(id));
        LOG("superfície %dx%d pronta", w, h);
        ready = true;
        inv[kInvHotFirst] = {ItDagger, 1}; inv[kInvHotFirst + 1] = {ItCoconut, 3};   // kit inicial (offline/antes do servidor): adaga + 3 cocos (o servidor manda o inventário real)
        if (!armaKitGiven) { armaKitGiven = true; armaLoaded = true; armaReloadT = -1.0f; }
#ifndef E0_CINEMA
        if (!menuDone) enterMenu();
#endif
        return true;
    }

    void termSurface() {
        ready = false;
        menu.shutdown(); menuActive = false;   // reabre no próximo initSurface se ainda não tinha dado JOGAR
        loadScr.shutdown();
        renderer.shutdown();
        if (dpy != EGL_NO_DISPLAY) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        }
        surf = EGL_NO_SURFACE;
    }

    void termAll() {
        termSurface();
        if (dpy != EGL_NO_DISPLAY) {
            if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
            eglTerminate(dpy);
        }
        dpy = EGL_NO_DISPLAY;
        ctx = EGL_NO_CONTEXT;
    }

#if E0_DEBUG_UI
    // Debug: calcula pontos de interesse do mundo (praia, montanha, floresta, construção, pedras) para o teleporte.
    void buildTeleTargets(const e0net::World* W) {
        tele.clear(); teleIdx = -1;
        if (!W) return;
        using namespace e0net;
        float sx = W->spawnX(), sz = W->spawnZ();
        tele.push_back({"SPAWN", sx, sz});
        tele.push_back({"NAVIO", sx, sz});   // posição real calculada ao teleportar (o navio se move): na água, a 6 m do casco
        tele.push_back({"GALEAO", sx, sz});
        tele.push_back({"CHALUPA", sx, sz});
        tele.push_back({"BERGANTIM", sx, sz});
        for (const WorldObject& o : W->objects) if (o.type == ObjPalm) { tele.push_back({"PALMEIRA", o.xcm * 0.01f, o.zcm * 0.01f + 3.0f}); break; }
        for (const WorldObject& o : W->objects) if (o.type == ObjCoconut) { tele.push_back({"COQUEIRO", o.xcm * 0.01f, o.zcm * 0.01f + 3.0f}); break; }
        for (int ore = 1; ore <= 3; ++ore)
            for (size_t oi = 0; oi < W->objects.size(); ++oi) if (W->objects[oi].type == ObjRock && W->oreOf((uint32_t)oi) == ore) {
                const WorldObject& o = W->objects[oi];
                tele.push_back({ore == 1 ? "OURO" : (ore == 2 ? "DIAMANTE" : "FERRO"), o.xcm * 0.01f, o.zcm * 0.01f + 0.85f * o.scale100 * 0.01f + 2.4f}); break;
            }
        for (const WorldObject& o : W->objects) if (o.type == ObjRock && o.scale100 >= 100) { tele.push_back({"PEDRA", o.xcm * 0.01f, o.zcm * 0.01f + 0.85f * o.scale100 * 0.01f + 2.4f}); break; }
        auto nearestVertex = [&](auto pred, float& ox, float& oz) {   // vértice mais próximo do spawn que satisfaz pred(ix,iz)
            double best = 1e30; bool found = false;
            for (int iz = 2; iz < World::kCells - 2; iz += 2)
                for (int ix = 2; ix < World::kCells - 2; ix += 2) {
                    if (!pred(ix, iz)) continue;
                    float x = ix * World::kCell - World::kHalf, z = iz * World::kCell - World::kHalf;
                    double d = (double)(x - sx) * (x - sx) + (double)(z - sz) * (z - sz);
                    if (d < best) { best = d; ox = x; oz = z; found = true; }
                }
            return found;
        };
        float x, z;
        auto bio = [&](int ix, int iz) { return W->biome[(size_t)iz * World::kVerts + ix]; };
        auto hv = [&](int ix, int iz) { return W->vertexHeight(ix, iz); };
        if (nearestVertex([&](int ix, int iz) { return bio(ix, iz) == BiomeSand && hv(ix, iz) > 0.4f && hv(ix, iz) < 1.2f; }, x, z)) tele.push_back({"PRAIA", x, z});
        else if (nearestVertex([&](int ix, int iz) { return hv(ix, iz) > 0.5f && hv(ix, iz) < 1.5f; }, x, z)) tele.push_back({"COSTA", x, z});
        if (nearestVertex([&](int ix, int iz) { return hv(ix, iz) < -2.2f && hv(ix, iz) > -4.5f; }, x, z)) tele.push_back({"MAR", x, z});
        if (nearestVertex([&](int ix, int iz) { return bio(ix, iz) == BiomeForest && hv(ix, iz) > 1.5f; }, x, z)) tele.push_back({"FLORESTA", x, z});
        float bestH = -1e9f; int bx = 0, bz = 0;
        for (int iz = 0; iz < World::kVerts; iz += 2) for (int ix = 0; ix < World::kVerts; ix += 2) if (hv(ix, iz) > bestH) { bestH = hv(ix, iz); bx = ix; bz = iz; }
        if (bestH > 14.0f) tele.push_back({"MONTANHA", bx * World::kCell - World::kHalf, bz * World::kCell - World::kHalf});
        for (const WorldObject& o : W->objects)
            if (World::isBuilding(o.type)) { tele.push_back({"CONSTRUCAO", o.xcm * 0.01f, o.zcm * 0.01f + 9.0f}); break; }
        for (const WorldObject& o : W->objects)
            if (o.type == ObjLog) { float dx, dz; World::logDir(o, dx, dz); tele.push_back({"TRONCO", o.xcm * 0.01f - dz * 2.8f, o.zcm * 0.01f + dx * 2.8f}); break; }
        for (const WorldObject& o : W->objects)
            if (o.type == ObjStick) { tele.push_back({"GRAVETOS", o.xcm * 0.01f + 0.9f, o.zcm * 0.01f + 1.2f}); break; }
        for (const WorldObject& o : W->objects)
            if (o.type == ObjPebble) { tele.push_back({"PEDRINHAS", o.xcm * 0.01f + 0.9f, o.zcm * 0.01f + 1.2f}); break; }
        for (const WorldObject& o : W->objects)
            if (o.type == ObjCloth) { tele.push_back({"TECIDOS", o.xcm * 0.01f + 0.9f, o.zcm * 0.01f + 1.2f}); break; }
        for (const WorldObject& o : W->objects)
            if (o.type == ObjRock && W->biomeAt(o.xcm * 0.01f, o.zcm * 0.01f) == BiomeRock) { tele.push_back({"PEDRAS", o.xcm * 0.01f + 3.0f, o.zcm * 0.01f}); break; }
        {   // OCEANO ABERTO: a ilha mais perto da inicial de cada tipo (o servidor põe você em terra firme; a ilha carrega no cliente em ~1 s)
            float bestD[IslandKindCount]; IslandSpec bestS[IslandKindCount];
            for (int k = 0; k < IslandKindCount; ++k) bestD[k] = 1e30f;
            for (int sl = 1; sl < kIslandSlots; ++sl) {
                int sx, sz; sectorOfSlot(sl, sx, sz);
                IslandSpec sp = islandSpec(W->seed, sx, sz);
                if (!sp.exists) continue;
                float d = (float)sp.oxcm * sp.oxcm + (float)sp.ozcm * sp.ozcm;
                if (d < bestD[sp.kind]) { bestD[sp.kind] = d; bestS[sp.kind] = sp; }
            }
            for (int k = 0; k < IslandKindCount; ++k) if (bestD[k] < 1e29f) tele.push_back({islandKindName((uint8_t)k), bestS[k].oxcm * 0.01f, bestS[k].ozcm * 0.01f});
        }
        tele.insert(tele.begin(), {"MASTRO", 0.0f, 0.0f});   // DOMINAÇÃO: mastro da próxima ilha do oceano (calculado no toque)
    }
    void teleportNext() {
        if (tele.empty()) return;
        teleIdx = (teleIdx + 1) % (int)tele.size();
        for (int i = 0; i < e0net::kMaxShips; ++i) if (std::strcmp(tele[teleIdx].name, e0net::shipDef(i).name) == 0) {
            const e0net::ShipState& S = e0net::shipState(i);
            if (!S.exists) { notify("NAO HA NAVIO (USE SPAWNAR NAVIO)"); return teleportNext(); }
            e0net::shipLocalToWorld(S, 0.0f, e0net::shipDef(i).hullHalfB + 2.2f, tele[teleIdx].x, tele[teleIdx].z);
        }
        if (std::strcmp(tele[teleIdx].name, "MASTRO") == 0) {   // mastro da ilha do oceano mais perto de mim (fora a ilha em que estou)
            using namespace e0net;
            const PlayerState me = net.predicted();
            const int here = terrain().slotAt(me.x, me.z);
            float bd = 1e30f; IslandSpec bs;
            for (int sl = 1; sl < kIslandSlots; ++sl) {
                int sx, sz; sectorOfSlot(sl, sx, sz);
                IslandSpec sp = islandSpec(e0net::terrain().mainSeed(), sx, sz);
                if (!sp.exists || sl == here) continue;
                const float d = std::hypot(sp.oxcm * 0.01f - me.x, sp.ozcm * 0.01f - me.z);
                if (d < bd) { bd = d; bs = sp; }
            }
            World iw; int32_t xc, zc;
            if (bd > 1e29f || !World::generateIsland(bs.seed, bs.kind, bs.oxcm, bs.ozcm, iw) || !e0net::conquestPoint(iw, xc, zc)) { notify("SEM MASTRO POR PERTO"); return; }
            tele[teleIdx].x = xc * 0.01f + 4.0f; tele[teleIdx].z = zc * 0.01f;
        }
        LOG("TELEPORT to %s at (%.2f, %.2f)", tele[teleIdx].name, tele[teleIdx].x, tele[teleIdx].z);
        net.requestTeleport(tele[teleIdx].x, tele[teleIdx].z);
        rig.snap();
    }

    // Setas de hora (debug): vai para o preset anterior/seguinte e pede ao servidor para mudar a hora do mundo.
    void stepTime(int dir) {
        if (timeIdx < 0) {  // primeira vez: parte do preset mais próximo da hora atual
            float t = net.timeOfDay(), best = 1e9f;
            for (int i = 0; i < 6; ++i) {
                float d = std::fabs(t - kTimePresets[i].cycleSec); d = std::min(d, e0net::kCycleSec - d);
                if (d < best) { best = d; timeIdx = i; }
            }
        }
        timeIdx = (timeIdx + dir + 6) % 6;
        net.requestTime(kTimePresets[timeIdx].cycleSec);
    }

    // Toque no painel de debug: devolve true se caiu dentro dele (e o consome).
    bool debugTouch(float x, float y) {
        if (wheelOpen || isleMode || dbgW <= 0 || x < dbgX || x > dbgX + dbgW || y < dbgY || y > dbgY + dbgH) return false;
        if (!dbgOpen) { dbgOpen = true; return true; }
        if (dbgTab == 5 && y >= dbgY + dbgPad + dbgBarH && y < dbgY + dbgPad + dbgBarH + dbgLineH) {
            qMode = qMode >= 2 ? -1 : qMode + 1;  // AUTO -> BAIXA -> MEDIA -> ALTA -> AUTO
            if (qMode >= 0) { qCur = qMode; renderer.setQuality(qCur); }
            slowCnt = fastCnt = 0;
            return true;
        }
        if (dbgTab == 5 && y >= dbgY + dbgPad + dbgBarH + dbgLineH && y < dbgY + dbgPad + dbgBarH + 2 * dbgLineH) {   // GRAF: filtro pirata
            gradeMode = (gradeMode + 1) % 3; renderer.setGrade(gradeMode == 0 ? 0.0f : (gradeMode == 1 ? 1.0f : 1.3f));
            return true;
        }
        if (dbgTab == 2 && y >= dbgY + dbgPad + dbgBarH && y < dbgY + dbgPad + dbgBarH + dbgLineH) {  // aba JOG: seletor de animação
            animDebugClip = animDebugClip >= ClipCount - 1 ? -1 : animDebugClip + 1;
            return true;
        }
        if (dbgTab == 3 && y >= dbgY + dbgPad + dbgBarH && y < dbgY + dbgPad + dbgBarH + dbgLineH) {  // aba MUNDO: setas de hora
            stepTime(+1);  // como os seletores de GRAF/CAM: cada toque avança para o próximo horário
            return true;
        }
        if (dbgTab == 7 && y >= dbgY + dbgPad + dbgBarH + dbgLineH && y < dbgY + dbgPad + dbgBarH + 2 * dbgLineH) {  // aba TERRA: linha 1 = teleporte
            teleportNext();
            return true;
        }
        if (dbgTab == 8) {  // aba GOD: modo god / cheats de debug
            int row = (int)((y - (dbgY + dbgPad + dbgBarH)) / dbgLineH);
            if (y >= dbgY + dbgPad + dbgBarH) {
                if (row == 1) {
                    godFly = !godFly;
                    flyAscendDist = flyDescendDist = 0.0f;
                    return true;
                } else if (row == 2) {
                    flyAscendDist += 5.0f;
                    return true;
                } else if (row == 3) {
                    flyDescendDist += 5.0f;
                    return true;
                } else if (row == 4) {
                    godFlyFast = !godFlyFast;
                    return true;
                } else if (row == 5) {
                    if (invServer()) { net.requestDebugItems(0, 0); notify("DEBUG: +1 DE CADA ITEM"); } else giveAllItems();
                    return true;
                } else if (row == 6) {   // materiais das MELHORIAS DA ILHA
                    if (invServer()) { net.requestDebugItems(e0net::item::Wood, 12); net.requestDebugItems(e0net::item::Stone, 4); net.requestDebugItems(e0net::item::Rope, 2); notify("DEBUG: MATERIAIS DE CONSTRU\xC7\xC3O"); }
                    return true;
                } else if (row == 7) {
                    if (invServer()) net.requestDebugCoins(100); else coins += 100;
                    return true;
                } else if (row == 8) {
                    godLife = !godLife;
                    return true;
                } else if (row == 9) {
                    health = std::min(100.0f, health + 100.0f);
                    return true;
                } else if (row == 10) {
                    health = std::max(0.0f, health - 100.0f);   // zera: o próximo updateHealth dispara a morte
                    return true;
                } else if (row == 11) {
                    hunger = std::min(100.0f, hunger + 100.0f);
                    return true;
                } else if (row == 12) {
                    hunger = std::max(0.0f, hunger - 100.0f);
                    return true;
                } else if (row == 13) {   // SPAWNAR NAVIO: nasce na água funda perto de você (servidor em --debug; offline: local)
                    const e0net::PlayerState m0 = net.predicted();
                    net.requestSpawnShip(m0.x, m0.z, e0net::ShipSloop);
                    notify("NAVIO SPAWNADO PERTO DE VOCE");
                    return true;
                } else if (row == 14) {
                    const e0net::PlayerState m0 = net.predicted();
                    net.requestSpawnShip(m0.x, m0.z, e0net::ShipGalleon);
                    notify("GALEAO SPAWNADO PERTO DE VOCE");
                    return true;
                } else if (row == 15) {
                    const e0net::PlayerState m0 = net.predicted();
                    net.requestSpawnShip(m0.x, m0.z, e0net::ShipBoat);
                    notify("CHALUPA SPAWNADA PERTO DE VOCE");
                    return true;
                } else if (row == 18) {
                    const e0net::PlayerState m0 = net.predicted();
                    net.requestSpawnShip(m0.x, m0.z, e0net::ShipBrig);
                    notify("BERGANTIM SPAWNADO PERTO DE VOCE");
                    return true;
                } else if (row == 19) {   // simula uma mensagem push (banner do tema, com ação)
                    PushBanner pb; pb.title = "PROMO\xC7\xC3O DOS MARES"; pb.body = "Hoje o gale\xE3o est\xE1 com desconto na loja! Aproveite antes da meia-noite, marujo."; pb.action = 1;
                    if (pushQ.size() < 4) pushQ.push_back(pb);
                    return true;
                } else if (row == 16 || row == 17) {   // vida do navio atual (a bordo ou o mais perto): servidor em --debug; offline: local
                    const int gi = hpShip(1e9f);   // o meu navio ou o mais perto
                    if (gi < 0 || !e0net::shipAfloat(e0net::shipState(gi))) { notify("NAVIO JA AFUNDOU / NAO EXISTE"); return true; }
                    net.requestShipHp(gi, row == 16 ? -50 : -10000);
                    char t[64]; std::snprintf(t, sizeof t, row == 16 ? "%s -50 DE VIDA" : "AFUNDANDO O %s", e0net::shipDef(gi).name); notify(t);
                    return true;
                }
            }
        }
        if (dbgTab == 6) {  // aba CAM: 3 primeiras linhas são botões
            int row = (int)((y - (dbgY + dbgPad + dbgBarH)) / dbgLineH);
            if (y >= dbgY + dbgPad + dbgBarH && row >= 0 && row <= 2) {
                if (row == 0) rig.cyclePreset(); else if (row == 1) rig.cycleShoulder(); else rig.cycleSensitivity();
                return true;
            }
        }
        if (y < dbgY + dbgBarH) {
            if (x < dbgToggleEnd) dbgOpen = false;
            else for (int i = 0; i < kTabs; ++i) if (x >= tabX[i] && x <= tabX[i] + tabW[i]) dbgTab = i;
        }
        return true;
    }
#else
    bool debugTouch(float, float) { return false; }   // build de produção: sem painel de debug
#endif

    // Botão CONSTRUIR no canto superior esquerdo (HUD normal) e layout da HUD do Modo Construção
    // CONSTRUIR / INVENTARIO / CRAFT / LOJA: só ícones, lado a lado ABAIXO do minimapa (k = 0..3)
    float menuBtnSide() const { float s = (float)uiScale(), gap = 6.0f * s; return std::min(30.0f * s, (2.0f * miniR() - 3.0f * gap) / 4.0f); }
    Rect2D menuBtnRect(int k) const {
        float s = (float)uiScale(), sq = menuBtnSide(), gap = 6.0f * s;
        return {miniCX() - miniR() + (float)k * (sq + gap), miniCY() + miniR() + 10.0f * s, sq, sq};
    }
    // RODA DE ACOES: botão redondo abaixo do minimapa; aberta, mostra CONSTRUIR / INVENTARIO / CRAFT / LOJA em leque e o X de fechar no centro
    bool wheelOpen = false;
    Rect2D wheelBtnRect() const { return menuBtnRect(0); }   // quadradinho (mesmo estilo dos antigos botões de ícone) abaixo do minimapa
    float wheelCX() const { return (float)w * 0.5f; }
    float wheelCY() const { return (float)h * 0.47f; }         // a roda abre no MEIO da tela
    float wheelCloseR() const { return 0.055f * (float)h; }
    float wheelItemR() const { return 0.078f * (float)h; }
    static constexpr int kWheelN = 5;   // CONSTRUIR, INVENTARIO, CRAFT, LOJA, NAVEGAÇÃO
    void wheelItemPos(int k, float& x, float& y) const {      // pentágono: o 0 em cima, depois em sentido horário
        const float ang = -1.5707963f + (float)k * (6.2831853f / (float)kWheelN), D = 0.24f * (float)h;
        x = wheelCX() + std::cos(ang) * D; y = wheelCY() + std::sin(ang) * D;
    }
    bool inWheelBtn(float x, float y) const { return wheelBtnRect().contains(x, y); }
    int wheelItemAt(float x, float y) const {
        for (int k = 0; k < kWheelN; ++k) { float ix, iy; wheelItemPos(k, ix, iy); float dx = x - ix, dy = y - iy, r = wheelItemR() * 1.05f; if (dx * dx + dy * dy <= r * r) return k; }
        return -1;
    }
    void handleTouch(AInputEvent* ev) {
        if (menuActive) {   // menu principal: só o primeiro dedo
            const int32_t mt = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
            const int ty = mt == AMOTION_EVENT_ACTION_DOWN ? 0 : mt == AMOTION_EVENT_ACTION_MOVE ? 1 : mt == AMOTION_EVENT_ACTION_UP ? 2 : mt == AMOTION_EVENT_ACTION_CANCEL ? 3 : -1;
            if (ty == 0 && !charOpen && pushTouch(AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0))) return;   // banner de mensagem push
            if (ty >= 0) {
                if (charOpen) charTouch(ty, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0));
                else if (teamOpen) teamTouch(ty, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0));
                else menu.touch(ty, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0));
            }
            return;
        }
        if (loadActive) return;   // tela de loading: sem toques
        if (introActive) {   // intro: um toque pula a descida
            if ((AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_DOWN) introT = std::max(introT, kIntroDur * 0.92f);
            return;
        }
        int32_t action = AMotionEvent_getAction(ev);
        int32_t type = action & AMOTION_EVENT_ACTION_MASK;
        size_t idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        auto id = [&](size_t i) { return AMotionEvent_getPointerId(ev, i); };

        if (type == AMOTION_EVENT_ACTION_DOWN || type == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float x = AMotionEvent_getX(ev, idx), y = AMotionEvent_getY(ev, idx);
            int pid = id(idx);
            if (navOpen) { navTouchDown(pid, x, y); return; }   // NAVEGAÇÃO (tela cheia): todo toque é dela
            if (flagOpen) { flagTouchDown(pid, x, y); return; }   // editor de BANDEIRA (tela cheia): todo toque é dele
            if (teamOpen) { if (teamTouchPtr < 0) { teamTouchPtr = pid; teamTouch(0, x, y); } return; }   // painel da TRIPULAÇÃO (1 dedo)
#ifdef E0_CINEMA
            if (cineTouch(x, y)) return;
#endif
            if (debugTouch(x, y)) {
                // toque no painel de debug: tratado lá; não inicia joystick/olhar
            } else if (pushTouch(x, y)) {
                // banner de mensagem push (toque abre a ação ou dispensa): consumido
            } else if (teamOfferTouch(x, y)) {
                // convite de TRIPULAÇÃO na tela (ACEITAR / RECUSAR): consumido
            } else if (rmOpen && rmTouchDown(x, y)) {
                // item do menu de resgate (SALVAR / CARREGAR / SOLTAR): consumido
            } else if (selfDead) {
                if (deathBtnVisible() && deathBtnRect().contains(x, y)) respawnSelf();
                else if (deathAdVisible() && deathBtnRect().contains(x, y)) pay.showRewarded(kAdTagRespawn);
            } else if (invOpen) {
                handleInvTouchDown(pid, x, y);
            } else if (craftOpen) {
                handleCraftTouchDown(pid, x, y);
            } else if (chestUi) {
                chestTouchDown(x, y);
            } else if (shopOpen) {
                shopTouchDown(pid, x, y);   // loja: todo toque é consumido (não vira joystick/olhar)
            } else if (mapOpen) {
                mapTouchDown(pid, x, y);
            } else if (gunnerMode) {
                gunnerTouch(x, y);   // tocar onde a bala deve cair (toque = disparo)
            } else if (isleMode) {
                isleTouchDown(pid, x, y);   // MELHORIAS DA ILHA: todo toque é da tela de cima
            } else if (wheelOpen) {   // roda aberta: todo toque é dela (item abre a tela; centro/fora fecha)
                const int k = wheelItemAt(x, y);
                wheelOpen = false;
                if (k == 0) openIsleMode();
                else if (k == 1) openInventory();
                else if (k == 2) openCraft();
                else if (k == 3) openShop();
                else if (k == 4) openNav();
            } else if (inWheelBtn(x, y)) {
                wheelOpen = true;
            } else if (chatBtnRect().contains(x, y)) {
                openChat();
            } else if (flagBtnRect().contains(x, y)) {
                openFlag();
            } else if (teamBtnRect().contains(x, y)) {
                openTeam();   // painel da TRIPULAÇÃO
            } else if (inMini(x, y)) {
                openMap();   // tocar no minimapa abre o mapa grande
            } else if (shipBtnAt(x, y) >= 0) {
                shipBtnPress(shipBtnAt(x, y), pid);
            } else if (inCamButton(x, y)) {
                rig.cyclePreset();  // NORMAL -> PERTO -> LONGE -> 1ª PESSOA
            } else if (inAimButton(x, y)) {
                armaAim = !armaAim;   // MIRAR: alterna
            } else if (armaMode() && inActionButton(x, y)) {
                if (actionPtr < 0) { actionPtr = pid; armaTrigger(); }
                if (lookT.id < 0) lookT = {pid, x, y, x, y};   // o dedo que atira continua girando a câmera (arrastar = mirar)
            } else if (inActionButton(x, y)) {
                if (actionPtr < 0) {
                    actionPtr = pid;
                    if (!localAnim.hasQueuedAction()) {
                        actionQueued = actCur < 0 ? nextPunchAction() : (actType == e0net::ObjRock ? e0net::ActMine : e0net::ActChop);
                    }
                }
            } else if (inCollectButton(x, y)) {
                if (collectPtr < 0) {
                    collectPtr = pid;
                    collectDown = true;
                    if (collectCur < 0 && collectDropId >= 0 && dropPendId < 0 && net.actBusy() < 3) {
                        for (const auto& d : net.drops()) if (d.id == collectDropId) {
                            if (!invFits(d.item, d.n)) notify("INVENTARIO CHEIO");
                            else { dropPendId = d.id; dropPendT = kCollectGrabT; dropPendItem = d.item; dropPendN = d.n; actionQueued = e0net::ActPickup; }
                            break;
                        }
                    } else if (collectCur >= 0 && collectPending < 0 && net.object((uint32_t)collectCur)) {
                        int item = itemForObject(net.object((uint32_t)collectCur)->type);
                        if (!invFits(item, 1)) notify("INVENTARIO CHEIO");
                        else {
                            collectPending = collectCur; collectPendT = kCollectGrabT;
                            actionQueued = e0net::ActPickup;
                        }
                    }
                }
            } else if (inRunButton(x, y) && shipCtx != 3) {
                if (runPtr < 0) {
                    runPtr = pid;
                    double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (t - lastRunTap < kDoubleTapSec) { jumpQueued = true; lastRunTap = -10; LOG("duplo toque: pulo"); }
                    else lastRunTap = t;
                }
            } else if (int s = inHotbar(x, y); s >= 0) {
                if (hotbarCur == s) hotbarTapSelected(); else hotbarCur = s;   // slot já selecionado: consumível = come; senão desseleciona (guarda o equipamento)
                LOG("hotbar slot selecionado: %d", hotbarCur + 1);
            } else if (int wb = worldBtnAt(x, y); wb >= 0) {
                worldBtnPress(wb);   // botão no cenário (LEME / CANHAO / ABRIR): consumido, não vira joystick/olhar
            } else if (x < w * 0.5f) {
                rmHoldBegin(pid, x, y);
                if (joyT.id < 0) joyT = {pid, x, y, x, y};
            } else if (lookT.id < 0) {
                rmHoldBegin(pid, x, y);
                lookT = {pid, x, y, x, y};
            }
        } else if (type == AMOTION_EVENT_ACTION_UP || type == AMOTION_EVENT_ACTION_CANCEL) {
            invDragEnd(AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), type == AMOTION_EVENT_ACTION_CANCEL);
            if (craftPtr >= 0) craftTouchUp(craftPtr, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), type == AMOTION_EVENT_ACTION_CANCEL);
            if (shopPtr >= 0) { if (type == AMOTION_EVENT_ACTION_CANCEL) shopPtr = -1; else shopTouchUp(shopPtr, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0)); }
            if (flagPtr >= 0) flagTouchUp(flagPtr, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), type == AMOTION_EVENT_ACTION_CANCEL);
            if (teamTouchPtr >= 0) { const int tp = teamTouchPtr; teamTouchPtr = -1; if (teamOpen) teamTouch(type == AMOTION_EVENT_ACTION_CANCEL ? 3 : 2, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0)); (void)tp; }
            if (navPtr >= 0) navTouchUp(navPtr, AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), type == AMOTION_EVENT_ACTION_CANCEL);
            if (isleMode) isleTouchUp(id(0), AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0), type == AMOTION_EVENT_ACTION_CANCEL);
            joyT.id = lookT.id = runPtr = collectPtr = actionPtr = shipThrPtr = shipRevPtr = rmHoldPtr = -1; collectDown = false;
            if (type == AMOTION_EVENT_ACTION_CANCEL) mapMulti = true;   // gesto cancelado não marca rota
            { int ids[2] = {mapPt[0].id, mapPt[1].id}; for (int i : ids) if (i >= 0) mapTouchUp(i); }
            mapPt[0].id = mapPt[1].id = -1;
        } else if (type == AMOTION_EVENT_ACTION_POINTER_UP) {
            int pid = id(idx);
            if (invDragPtr == pid) invDragEnd(AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), false);
            if (craftPtr == pid) craftTouchUp(pid, AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), false);
            if (shopPtr == pid) shopTouchUp(pid, AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx));
            if (flagPtr == pid) flagTouchUp(pid, AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), false);
            if (navPtr == pid) navTouchUp(pid, AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), false);
            if (isleMode) isleTouchUp(pid, AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), false);
            mapTouchUp(pid);
            if (joyT.id == pid) joyT.id = -1;
            if (lookT.id == pid) lookT.id = -1;
            if (rmHoldPtr == pid) rmHoldPtr = -1;
            if (runPtr == pid) runPtr = -1;
            if (shipThrPtr == pid) shipThrPtr = -1;
            if (shipRevPtr == pid) shipRevPtr = -1;
            if (collectPtr == pid) { collectPtr = -1; collectDown = false; }
            if (actionPtr == pid) actionPtr = -1;
        } else if (type == AMOTION_EVENT_ACTION_MOVE) {
            for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); ++i) {
                float x = AMotionEvent_getX(ev, i), y = AMotionEvent_getY(ev, i);
                int pid = id(i);
                if (pid == rmHoldPtr) { rmHoldX = x; rmHoldY = y; }
                if (mapOpen) mapTouchMove(pid, x, y);
                if (shopOpen) shopTouchMove(pid, x, y);
                if (flagOpen) flagTouchMove(pid, x, y);
                if (teamOpen && pid == teamTouchPtr) teamTouch(1, x, y);
                if (navOpen) navTouchMove(pid, x, y);
                if (craftOpen) craftTouchMove(pid, y);
                if (pid == invDragPtr) {
                    invDragX = x; invDragY = y;
                    if (std::fabs(x - invDragX0) + std::fabs(y - invDragY0) > 14.0f * invGeo().u) invDragMoved = true;
                }
                if (isleMode) isleTouchMove(pid, x, y);
                if (pid == joyT.id) { joyT.x = x; joyT.y = y; }
                if (pid == lookT.id) {
                    input.lookDX += x - lookT.x;
                    input.lookDY += y - lookT.y;
                    lookT.x = x; lookT.y = y;
                }
            }
        }
    }

    // Vetor do joystick em [-1,1] (já com zona morta); y positivo = frente.
    void joystickVector(float& ox, float& oy) const {
        ox = oy = 0;
        if (joyT.id < 0) return;
        float R = joyR();
        float dx = (joyT.x - joyT.sx) / R, dy = (joyT.y - joyT.sy) / R;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1) { dx /= len; dy /= len; len = 1; }
        const float dead = 0.12f;
        if (len <= dead) return;
        float k = (len - dead) / (1 - dead) / len;
        ox = dx * k;
        oy = -dy * k;
    }

    void update(float dt) {
        updateHunger(dt, !isleMode && !uiFull() && runPtr >= 0);
        updateHealth(dt);
        updateDeath(dt);
        updateRescueUi(dt);
        updateRescueMenu(dt);
        updateShipUi();
        updateChest(dt);
        updateWorldBtns(dt);
        updateGunner(dt);
        if (wpSet && std::hypot(wpX - net.visual().x, wpZ - net.visual().z) < 12.0f) { wpSet = false; notify("CHEGOU AO DESTINO"); }
        updateMap(dt);
        updateCraft(dt);
        updateShop(dt);
        updateMonetize(dt); updateWallet(); updateAccountNet(dt); updatePush(dt);
        updateIntro(dt);
        // Entrada: joystick + botão de correr
        joystickVector(input.moveX, input.moveY);
        if (gunnerMode || introActive) { input.moveX = input.moveY = 0; jumpQueued = false; actionQueued = 0; }

        // Mundo novo (baixado / gerado / trocado): entrega ao renderer e à colisão da câmera
        if (net.worldGeneration() != lastWorldGen) {
            lastWorldGen = net.worldGeneration();
            renderer.setTerrain(net.world());
            camWorld.w = net.world();
            if (net.world()) wstats = net.world()->stats();
#if E0_DEBUG_UI
            buildTeleTargets(net.world());
#endif
            renderer.clearIslands();
            for (int sl = 0; sl < e0net::kIslandSlots; ++sl) {
                collectiblesBySlot[sl].clear();
                actionablesBySlot[sl].clear();
            }
            collectCur = -1; actCur = -1;
            lastTakenGen = lastHarvestGen = 0xFFFFFFFFu;
            harvestDone.clear(); harvestInit = false; harvestWarm = harvestConnT = 0; for (HitPend& hp : hitPend) hp.idx = -1;
            collectPending = -1; collectLocalHide.clear();
            registerIslandObjects(0);
            rig.snap();
        }

        for (int isl; net.popUnloadedIsland(isl); ) {   // ilha distante descarregada (economia de RAM/VRAM em aparelhos de 4 GB)
            renderer.setIsland(isl, nullptr);
            if (isl >= 0 && isl < e0net::kIslandSlots) {
                collectiblesBySlot[isl].clear();
                actionablesBySlot[isl].clear();
            }
        }
        for (int isl; net.popNewIsland(isl); ) {   // ilha do oceano aberto recém-gerada: entrega ao renderer e registra o que dá para coletar/cortar/minerar
            renderer.setIsland(isl, net.island(isl));
            registerIslandObjects(isl);
        }

        // Olhar: arrasto na metade direita -> CameraRig
        float lookDX = input.lookDX, lookDY = input.lookDY;
        input.lookDX = input.lookDY = 0;
        rig.setVehicle(shipCtx == 3, curShip().yaw, curDef().camDist, curDef().camUp);   // no leme: câmera de veículo bem atrás do navio
        rig.setAim(armaMode() && armaAim && armaReloadT < 0.0f);
        rig.look(dt, lookDX, lookDY, lookT.id >= 0 && !isleMode);

        // O jogador é movido pela simulação compartilhada (predição local; servidor corrige).
        const e0net::PlayerState preStep = net.predicted();
        const bool wasSwimming = e0net::isSwimmingPos(preStep.x, preStep.y, preStep.z);
        weakWarnT = std::max(0.0f, weakWarnT - dt);
        if (tooWeak()) { if (jumpQueued || runPtr >= 0) warnWeak(); jumpQueued = false; }   // sem força: pulo cancelado
        if (wasSwimming || isleMode || dying() || actionLocked() || carryingNow()) { jumpQueued = false; actionQueued = 0; }
        else if (actionPtr >= 0 && actionQueued == 0 && !localAnim.inAction() && !armaMode()) {
            actionQueued = actCur < 0 ? nextPunchAction() : (actType == e0net::ObjRock ? e0net::ActMine : e0net::ActChop);
        }
        bool flyUp = false;
        bool flyDown = false;
        if (godFly) {
            if (flyAscendDist > 0.0f) {
                float step = std::min(flyAscendDist, 18.0f * dt);
                flyAscendDist -= step;
                flyUp = true;
            }
            if (flyDescendDist > 0.0f) {
                float step = std::min(flyDescendDist, 18.0f * dt);
                flyDescendDist -= step;
                flyDown = true;
            }
        } else {
            flyAscendDist = flyDescendDist = 0.0f;
        }

        e0net::InputCmd cmd;
        cmd.moveX = (isleMode || uiFull() || dying() || actionLocked()) ? 0.0f : input.moveX;
        cmd.moveY = (isleMode || uiFull() || dying() || actionLocked()) ? 0.0f : input.moveY;
        cmd.camYaw = rig.yaw(); cmd.camPitch = rig.pitch(); cmd.dt = dt;
        cmd.held = heldTool();
        cmd.carried = carriedMask();
        cmd.flags = (!isleMode && !uiFull() && !dying() && !actionLocked() && !tooWeak() && runPtr >= 0 ? e0net::InputRun : 0) | (jumpQueued ? e0net::InputJump : 0) |
                    ((actionQueued & 7u) << e0net::InputActShift);
        if (godFly) {
            cmd.flags |= e0net::InputFly;
            if (godFlyFast) cmd.flags |= e0net::InputFlyFast;
            if (flyUp) cmd.flags |= e0net::InputJump;
            if (flyDown) cmd.flags |= e0net::InputFlyDown;
        }
        if (armaAimingNow()) cmd.flags &= (uint8_t)~e0net::InputRun;   // mirando não corre
        cmd.aim = (uint8_t)((armaAimingNow() ? e0net::AimOn : 0) | (armaMode() && armaReloadT >= 0.0f ? e0net::AimReload : 0));
        cmd.shipEv = (uint8_t)((gunnerMode ? e0net::ShipGunWant : 0) | shipEvQueued | (shipCtx == 3 ? ((shipThrPtr >= 0 ? 1u : (shipRevPtr >= 0 ? 2u : 0u)) << e0net::ShipThrShift) : 0u));
        shipEvQueued = 0;
        if (shipCtx == 3) { cmd.flags &= (uint8_t)~(e0net::InputRun | e0net::InputJump); }
        uint8_t trigAct = actionQueued;
        jumpQueued = false;
        actionQueued = 0;
        net.step(cmd);
        net.tick();
        net.remotes(remotes);
        {   // do mais perto ao mais longe: os orçamentos de LOD/sombra/reflexo do renderer (e as TAGs) vão para quem está perto
            const e0net::PlayerState mv = net.visual();
            std::sort(remotes.begin(), remotes.end(), [&](const e0net::PlayerState& a, const e0net::PlayerState& b) {
                return (a.x - mv.x) * (a.x - mv.x) + (a.z - mv.z) * (a.z - mv.z) < (b.x - mv.x) * (b.x - mv.x) + (b.z - mv.z) * (b.z - mv.z); });
        }

        // Câmera: atualiza DEPOIS de mover o jogador (late update), sobre o estado visual (já sem saltos da rede).
        const e0net::PlayerState me = net.visual(), pr = net.predicted();
        CameraTarget ct;
        ct.pos = {me.x, me.y, me.z}; ct.yaw = me.yaw; ct.speed = pr.speed; ct.vy = pr.vy;
        if (rig.vehicle()) { const e0net::ShipState& SV = curShip(); ct.pos = {SV.x, curDef().deckY, SV.z}; ct.speed = 0; ct.vy = 0; }   // câmera de veículo: segue o casco, não o timoneiro
        float gh = e0net::supportHeightAt(pr.x, pr.z, pr.y);
        ct.grounded = (pr.y <= gh + 0.03f && pr.vy <= 0.0f) || godFly;
        rig.follow(dt, ct, h > 0 ? (float)w / (float)h : 1.0f, camWorld);
        if (isleMode) {
            camera = isleCamera(dt);   // MELHORIAS DA ILHA: vista de cima
        } else {
            camera = rig.camera();
            if (introActive) introCamera(camera);
            if (gunBlend > 0.001f) {   // câmera de canhoneiro: mistura suave para a vista de cima
                Camera g = gunCamera(); const float t = gunBlend;
                float dy = e0net::wrapPi(g.yaw - camera.yaw);
                camera.position = camera.position + (g.position - camera.position) * t;
                camera.yaw = camera.yaw + dy * t;
                camera.pitch += (g.pitch - camera.pitch) * t; camera.fovY += (g.fovY - camera.fovY) * t; camera.roll *= (1.0f - t);
                camera.zFar = std::max(camera.zFar, 700.0f * t);
            }
        }
        updateCollect(dt, me);
        updateAction(dt, me);

        // Animação esquelética: jogador local e remotos
        AnimInput ain;
        ain.speed = pr.speed; ain.vy = pr.vy; ain.grounded = ct.grounded && !godFly;
        ain.swimming = e0net::isSwimmingPos(pr.x, pr.y, pr.z) && !godFly;
        ain.dead = selfDead; ain.carried = selfDead && (pr.status & e0net::StCarried); ain.carrying = carryingNow(); ain.rescuing = rescuingNow(); ain.triggerAction = (ain.swimming || godFly || dying() || actionLocked()) ? 0 : (int)trigAct; ain.forceClip = animDebugClip;
        ain.tool = carryingNow() ? 0 : heldTool();   // carregando alguém: a ferramenta volta para o corpo
        ain.aiming = armaAimingNow(); ain.reloading = armaMode() && armaReloadT >= 0.0f; ain.aimPitch = rig.pitch();
        ain.fired = armaFiredEvt; armaFiredEvt = false;
        if (std::fabs(input.moveX) + std::fabs(input.moveY) > 0.05f) ain.moveAngle = e0net::wrapPi(rig.yaw() + std::atan2(input.moveX, input.moveY) - pr.yaw);
        localAnim.setSkeleton(renderer.skeletonOfSkin(net.skin()));   // esqueleto do MEU personagem
        localAnim.update(dt, ain, renderer.anims());
        updateArma(dt, me);
        {   // início de um golpe contra o alvo à frente: confere a ferramenta e marca o momento do impacto
            toolWarnT = std::max(0.0f, toolWarnT - dt);
            int sc = localAnim.popActionStart();
            hungerSwimming = ain.swimming;
            if (sc >= 0 && sc != ClipPickup) hungerActionT = 1.3f;   // golpe (bater/cortar/minerar) gasta mais
            if (sc >= 0 && sc != ClipPickup && actCur >= 0 && net.object((uint32_t)actCur) && !net.isTaken((uint32_t)actCur)) {
                const uint8_t ty = net.object((uint32_t)actCur)->type;
                const uint8_t need = ty == e0net::ObjRock ? e0net::HeldPickaxe : e0net::HeldHatchet;
                if (heldTool() != need && !(need == e0net::HeldHatchet && heldTool() == e0net::HeldDagger)) {   // a adaga também corta madeira (mais devagar)
                    if (toolWarnT <= 0) { notify(need == e0net::HeldPickaxe ? "PRECISA DA PICARETA" : "PRECISA DA MACHADINHA"); toolWarnT = 1.5f; }
                } else if (sc == ClipChop || sc == ClipMine || sc == ClipToolMine) {
                    for (HitPend& hp : hitPend) if (hp.idx < 0) { hp.idx = actCur; hp.type = ty; hp.t = sc == ClipToolMine ? 0.85f : (sc == ClipChop ? 0.50f : 0.55f); break; }
                }
            }
            for (HitPend& hp : hitPend) if (hp.idx >= 0 && (hp.t -= dt) <= 0) {
                if (hp.type == e0net::ObjRock) audio.playMine(1.0f, 0.0f); else audio.playChop(1.0f, 0.0f);   // som do impacto (no momento do golpe)
                if (!net.isTaken((uint32_t)hp.idx) && net.actBusy() < 4) net.requestAct(e0net::ActHit, (uint16_t)e0net::gidIdx((uint32_t)hp.idx), (uint16_t)e0net::gidSlot((uint32_t)hp.idx));
                hp.idx = -1;
            }
        }

        ++animFrame;
        const Vec3 camFwd = camera.forward();
        bool seenNow[e0net::kMaxPlayers] = {};
        for (const auto& p : remotes) {
            int id = p.id % e0net::kMaxPlayers;
            seenNow[id] = true;
            int rAct = 0;
            bool firstSeen = !remoteSeen[id];
            if (firstSeen) {
                remoteLastAct[id] = p.action;
                remoteAnims[id].reset();
            } else if (p.action != remoteLastAct[id]) {
                remoteLastAct[id] = p.action;
                rAct = (int)(p.action & 7u);
            }
            float dxMe = p.x - me.x, dzMe = p.z - me.z;
            float distMe2 = dxMe * dxMe + dzMe * dzMe;
            float dxCam = p.x - camera.position.x, dyCam = (p.y + 0.9f) - camera.position.y, dzCam = p.z - camera.position.z;
            float distCam2 = dxCam * dxCam + dyCam * dyCam + dzCam * dzCam;
            bool inShadowOrNear = distMe2 < 22.0f * 22.0f;
            bool inFrontOfCam = distCam2 < 360.0f * 360.0f && (dxCam * camFwd.x + dyCam * camFwd.y + dzCam * camFwd.z > -2.5f);
            bool computeMats = firstSeen || rAct != 0 || inShadowOrNear || (inFrontOfCam && (distCam2 < 26.0f * 26.0f || ((animFrame + (uint32_t)id) & 1u) == 0u));

            float rgh = distMe2 < 160.0f * 160.0f ? e0net::supportHeightAt(p.x, p.z) : p.y;
            bool rGrounded = (p.y <= rgh + 0.05f && std::fabs(p.vy) < 0.25f);
            bool rSwim = distMe2 < 160.0f * 160.0f ? e0net::isSwimmingPos(p.x, p.y, p.z) : (p.y < -0.5f);
            AnimInput rin;
            rin.speed = p.speed; rin.vy = p.vy;
            rin.grounded = rGrounded;
            rin.swimming = rSwim;
            rin.triggerAction = rin.swimming ? 0 : rAct;
            {   // velocidade estimada pela posição interpolada (direção do andar mirando: frente/trás/lados)
                if (firstSeen) { remotePX[id] = p.x; remotePZ[id] = p.z; remoteVX[id] = remoteVZ[id] = 0; }
                const float idt = 1.0f / std::max(dt, 1e-3f), k = 1.0f - std::exp(-dt * 8.0f);
                remoteVX[id] += ((p.x - remotePX[id]) * idt - remoteVX[id]) * k; remoteVZ[id] += ((p.z - remotePZ[id]) * idt - remoteVZ[id]) * k;
                remotePX[id] = p.x; remotePZ[id] = p.z;
                const float vf = remoteVX[id] * std::sin(p.yaw) - remoteVZ[id] * std::cos(p.yaw), vr = remoteVX[id] * std::cos(p.yaw) + remoteVZ[id] * std::sin(p.yaw);
                if (vf * vf + vr * vr > 0.04f) rin.moveAngle = std::atan2(vr, vf);
            }
            rin.aiming = (p.aim & e0net::AimOn) != 0; rin.reloading = (p.aim & e0net::AimReload) != 0; rin.aimPitch = (float)p.aimPitch * 0.0174533f;
            rin.fired = remoteFired[id]; remoteFired[id] = false;
            rin.tool = (p.status & e0net::StCarrying) ? 0 : p.held; rin.dead = (p.status & e0net::StDead) != 0; rin.carried = (p.status & e0net::StCarried) != 0; rin.carrying = (p.status & e0net::StCarrying) != 0; rin.rescuing = (p.status & e0net::StRescuing) != 0;
            remoteAnims[id].setSkeleton(renderer.skeletonOfSkin(p.skin));
            remoteAnims[id].update(dt, rin, renderer.anims(), computeMats);
            {   // golpe de ferramenta do outro jogador: ClipChop = madeira; picareta batendo por cima = pedra; ClipMine com machadinha = pedra, com picareta = madeira
                int sc = remoteAnims[id].popActionStart();
                if (distCam2 < 40.0f * 40.0f && (sc == ClipChop || sc == ClipMine || sc == ClipToolMine) && sfxPend.size() < 16) {
                    bool stone = sc == ClipToolMine || (sc == ClipMine && p.held == e0net::HeldHatchet);
                    sfxPend.push_back({sc == ClipToolMine ? 0.85f : (sc == ClipChop ? 0.50f : 0.55f), stone, p.x, p.z});
                }
            }
            if (distCam2 < 28.0f * 28.0f) {
                footAudio(footRemote[id], remoteAnims[id], p.x, p.z, p.speed, rGrounded, rSwim, false);
            }
        }
        for (int i = 0; i < e0net::kMaxPlayers; ++i) remoteSeen[i] = seenNow[i];
        footAudio(footLocal, localAnim, me.x, me.z, pr.speed, ain.grounded, ain.swimming, true);
        updateAmbience(dt, pr);
        updateHitSfx(dt);

        if (game) game->onUpdate(dt);
    }

    static bool blinkOff() { using namespace std::chrono; return ((long long)(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 110) & 1) != 0; }
    void drawWorld() {
        renderer.setEnvironment(makeEnvironment(net.timeOfDay()));
        renderer.setFogScale(isleMode ? 0.22f : 1.0f - 0.9f * gunBlend);   // vista de cima da ilha: quase sem névoa
        if (introActive) { const float e = smoother(introT / kIntroDur); renderer.setFogScale(0.06f + 0.94f * e * e); }   // lá em cima quase sem névoa (senão é só um muro laranja)
#ifdef E0_CINEMA
        if (cRun) renderer.setFogScale(cFog);
#endif
        const e0net::PlayerState me = net.visual(), pr = net.predicted();
        std::vector<CharInst> chars;
        chars.reserve(1 + remotes.size());
        tags.clear();
        CharInst local{{me.x, me.y, me.z}, me.yaw, playerColor(net.connected() ? net.myId() : 0), rig.hideLocal(), localAnim.boneRows(), pr.speed, carriedMask()};
        local.held = localAnim.toolVisible() ? heldTool() : 0;   // ao nadar a ferramenta some (guardada)
        local.attachRows = localAnim.attachRows();
        local.skin = net.skin();
        if (selfDead && (pr.status & e0net::StCarried)) attachCarried(local, (int)pr.link - 1, me);   // estou sendo carregado no ombro
#ifdef E0_CINEMA
        if (cRun) {   // o jogador some; quem aparece é a tripulação do roteiro (mesma pose, cores diferentes)
            local.hidden = true;
            uint32_t ci = 0;
            for (const cine::Crew& m : cCrew) {
                const e0net::ShipState& S = e0net::shipState(m.ship); const e0net::ShipDef& D = e0net::shipDef(m.ship);
                if (!S.exists) continue;
                float cx, cz; e0net::shipLocalToWorld(S, m.a, m.b, cx, cz);
                CharInst c{{cx, e0net::shipFloor(D, m.a, m.b), cz}, e0net::shipWrap(S.yaw + m.yaw), playerColor((int)(ci++ % 5)), false, localAnim.boneRows(), 0.0f, 0};
                c.attachRows = localAnim.attachRows(); c.skin = net.skin();
                chars.push_back(c);
            }
        }
#endif
        if ((pr.status & e0net::StProtected) && blinkOff()) local.hidden = true;   // protegido: pisca
        chars.push_back(local);
        for (const auto& p : remotes) {
            int id = p.id % e0net::kMaxPlayers;
            CharInst rc{{p.x, p.y, p.z}, p.yaw, playerColor(p.id), false, remoteAnims[id].boneRows(), p.speed, p.carried};
            rc.held = remoteAnims[id].toolVisible() ? p.held : 0;
            rc.attachRows = remoteAnims[id].attachRows();
            rc.skin = p.skin;
            if ((p.status & e0net::StProtected) && blinkOff()) rc.hidden = true;
            if (p.status & e0net::StCarried) attachCarried(rc, (int)p.link - 1, me);   // corpo no ombro de quem carrega
            chars.push_back(rc);
            collectTag(rc, p.id, (p.status & e0net::StDead) != 0);
        }
        {
            for (int i = 0; i < e0net::kMaxShips; ++i) {
                const e0net::ShipState& S = e0net::shipState(i);
                ShipDraw sd; sd.exists = S.exists; sd.x = S.x; sd.z = S.z; sd.yaw = S.yaw; sd.wheel = S.steer * 2.6f; sd.chestOpen = i == shipSel ? chestOpenT : 0.0f; sd.sinkT = S.sinkT;
                renderer.setShip(i, sd);
            }
        }
#ifdef E0_CINEMA
        if (cRun) { renderer.renderFrame(camera, chars, cFocus); return; }
#endif
        if (shopOpen || flagOpen || navOpen || teamOpen) {   // LOJA / BANDEIRA / NAVEGAÇÃO / TRIPULAÇÃO são opacas e cobrem a tela toda: não desenha o mundo 3D por baixo (60 fps estáveis, menos bateria)
            glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, w, h);
            glClearColor(0.03f, 0.03f, 0.035f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            return;
        }
        renderer.renderFrame(camera, chars, isleMode ? Vec3{isleCx, std::max(0.0f, e0net::terrain().heightAt(isleCx, isleCz)), isleCz} : Vec3{me.x, me.y, me.z});   // vista da ilha: sombras em volta do foco
        HudMark mk[3]; int nm = marks(mk);   // linha branca do item/alvo até a altura dos ombros do personagem
        for (int i = 0; i < nm; ++i) {
            if (!mk[i].line) continue;   // alvos de ação (árvore, pedra, tora): só a etiqueta
            Vec3 base{mk[i].pos.x, mk[i].pos.y + 0.03f, mk[i].pos.z};
            Vec3 top{mk[i].pos.x, std::max(me.y + 1.45f, mk[i].pos.y + 0.6f), mk[i].pos.z};
            renderer.drawWorldLine(camera, base, top, 0.95f * mk[i].alpha, 2.5f);
        }
        if (!isleMode && !uiFull() && !gunnerMode) for (const WorldBtn& b : wbtn) {   // linha dos botões do cenário (baú, canhão) até o botão
            if (b.alpha <= 0.02f || !b.line) continue;
            renderer.drawWorldLine(camera, {b.base.x, b.base.y + 0.03f, b.base.z}, b.top, 0.85f * b.alpha, 2.5f);
        }
    }

    void noteFrame(float rawDt) {  // chamado com o tempo real do frame (sem limite)
        fpsAcc += rawDt; ++fpsFrames; fpsWorst = std::max(fpsWorst, rawDt);
        if (fpsAcc >= 0.5f) {
            fps = fpsFrames / fpsAcc; frameMs = fpsAcc / fpsFrames * 1000.0f; worstMs = fpsWorst * 1000.0f;
            fpsAcc = 0; fpsFrames = 0; fpsWorst = 0;
            autoQuality();
        }
    }

    // Controle automático da qualidade. O tempo de GPU sozinho engana (o governador de frequência baixa o clock quando a carga
    // é leve: medido 10.4 ms em BAIXA e 8.3 ms em ALTA), então o critério combina FPS (vsync perdido) e GPU:
    //   desce  : fps < 52 OU gpu > 15.5 ms  por 2 s;     sobe: fps >= 57 E gpu < 8 ms  por 20 s.
    // Um nível que falhou não é tentado de novo por 90 s (evita fica-sobe-desce). Sem timer de GPU, só o FPS vale.
    void autoQuality() {
        if (qMode >= 0) return;
        if (introActive) { slowCnt = fastCnt = 0; return; }   // a intro força BAIXA de propósito
        if (shopOpen || flagOpen || navOpen || teamOpen) { slowCnt = fastCnt = 0; return; }   // loja em tela cheia: o custo é da UI, não do mundo (não mexe na qualidade)
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        float gpu = renderer.gfx().gpuMs;
        bool useGpu = renderer.hasGpuTimer() && gpu >= 0;
        bool slow = fps < 52.0f || (useGpu && gpu > 15.5f);
        bool fast = fps >= 57.0f && (!useGpu || gpu < 8.0f);
        if (slow) { ++slowCnt; fastCnt = 0; } else if (fast) { ++fastCnt; slowCnt = 0; } else { slowCnt = fastCnt = 0; }
        if (slowCnt >= 4 && qCur > 0) {
            lastBad[qCur] = t; --qCur; slowCnt = 0;
            renderer.setQuality(qCur);
            LOG("auto-qualidade: DESCEU para %s (gpu=%.1f ms fps=%.0f)", Renderer::qualityName(qCur), gpu, fps);
        } else if (fastCnt >= 40 && qCur < 2 && t - lastBad[qCur + 1] > 90.0) {
            ++qCur; fastCnt = 0;
            renderer.setQuality(qCur);
            LOG("auto-qualidade: SUBIU para %s (gpu=%.1f ms fps=%.0f)", Renderer::qualityName(qCur), gpu, fps);
        }
    }

#if E0_DEBUG_UI
    void drawDebug(float rawDt) {
        struct Line { char t[96]; float r, g, b; };
        std::vector<Line> L;
        auto add = [&](float r, float g, float b, const char* fmt, auto... a) {
            Line l; std::snprintf(l.t, sizeof l.t, fmt, a...); l.r = r; l.g = g; l.b = b; L.push_back(l);
        };
        const NetClient::Stats& st = net.stats();
        const e0net::PlayerState me = net.predicted();

        // hora do mundo: 06:00 = nascer do sol (cycleSec 0), 18:00 = pôr do sol
        float t = net.timeOfDay();
        float hr = std::fmod(6.0f + t / e0net::kCycleSec * 24.0f, 24.0f);
        int hh = (int)hr, mm = (int)((hr - hh) * 60.0f);
        bool day = t < e0net::kDayLengthSec;
        int rem = (int)((day ? e0net::kDayLengthSec - t : e0net::kCycleSec - t) + 0.5f);
        Environment env = makeEnvironment(t);

        const char* ns = net.state() == NetClient::State::Connected ? "CONECTADO"
                         : net.state() == NetClient::State::Connecting ? "CONECTANDO" : "OFFLINE";
        bool conn = net.connected();
        char ping[24];
        if (st.rttMs >= 0) std::snprintf(ping, sizeof ping, "%.0f MS", st.rttMs); else std::snprintf(ping, sizeof ping, "-- MS");
        float pr = st.rttMs < 0 ? 0.8f : (st.rttMs < 80 ? 0.45f : 1.0f);
        float pg = st.rttMs < 0 ? 0.8f : (st.rttMs < 80 ? 1.0f : (st.rttMs < 150 ? 0.85f : 0.35f));
        float fr = fps >= 55 ? 0.45f : 1.0f, fg = fps >= 55 ? 1.0f : (fps >= 30 ? 0.85f : 0.35f), fb = fps >= 55 ? 0.45f : 0.3f;
        bool badCorr = st.corrMax > 0.1f;

        static const char* kTabNames[kTabs] = {"GERAL", "REDE", "JOG", "MUNDO", "OUTROS", "GRAF", "CAM", "TERRA", "GOD"};
        if (dbgOpen) {
            switch (dbgTab) {
            case 0:  // GERAL: visão rápida de tudo
                add(fr, fg, fb, "FPS %.0f  (%.1f MS)  PIOR %.1f", fps, frameMs, worstMs);
                add(1.0f, 0.92f, 0.55f, "HORA %02d:%02d  %s", hh, mm, day ? "DIA" : "NOITE");
                add(conn ? 0.45f : 1.0f, conn ? 1.0f : 0.5f, conn ? 0.45f : 0.3f, "REDE %s  ID %d", ns, net.myId());
                add(pr, pg, 0.4f, "PING %s  JOG %d/%d", ping, st.players, e0net::kMaxPlayers);
                add(0.9f, 0.95f, 1.0f, "POS %.1f %.1f %.1f", me.x, me.y, me.z);
                add(0.8f, 0.85f, 1.0f, "TELA %dX%d  GRAF %s%s", w, h, Renderer::qualityName(renderer.gfx().quality), qMode < 0 ? " AUTO" : "");
                break;
            case 1:  // REDE
                add(conn ? 0.45f : 1.0f, conn ? 1.0f : 0.5f, conn ? 0.45f : 0.3f, "ESTADO %s  ID %d", ns, net.myId());
                add(0.9f, 0.95f, 1.0f, "JOGADORES %d/%d", st.players, e0net::kMaxPlayers);
                add(pr, pg, 0.4f, "PING %s  MIN %.0f MAX %.0f", ping, st.rttMin, st.rttMax);
                add(0.9f, 0.95f, 1.0f, "JITTER %.1f MS  INTERP %.0f MS", st.rttJit, st.interpMs);
                add(0.9f, 0.95f, 1.0f, "SNAPSHOTS %d/S (ALVO %d)", st.snapPerSec, e0net::kTickHz);
                add(0.8f, 0.85f, 1.0f, "RX %.1f  TX %.1f KB/S", st.rxKBs, st.txKBs);
                add(0.8f, 0.85f, 1.0f, "SRV %s", net.serverLabel());
                add(badCorr ? 1.0f : 0.8f, badCorr ? 0.45f : 0.85f, badCorr ? 0.35f : 1.0f, "CORRECAO MAX %.2f M (1S)", st.corrMax);
                add(0.8f, 0.85f, 1.0f, "CMDS PENDENTES %zu", net.pendingCount());
                break;
            case 2:  // JOG (eu)
                if (localAnim.hasQueuedAction())
                    add(1.0f, 0.92f, 0.55f, "ANIM %s %.0f%% (FILA %s) <TOQUE>", localAnim.activeName(), localAnim.activePhase() * 100.0f, localAnim.queuedName());
                else
                    add(1.0f, 0.92f, 0.55f, "ANIM %s %.0f%% [%s]  <TOQUE>", localAnim.activeName(), localAnim.activePhase() * 100.0f, animDebugClip < 0 ? "AUTO" : "FIXA");
                add(0.9f, 0.95f, 1.0f, "POS %.2f %.2f %.2f", me.x, me.y, me.z);
                add(0.9f, 0.95f, 1.0f, "VEL %.1f M/S  VY %.1f", me.speed, me.vy);
                add(0.9f, 0.95f, 1.0f, "CORPO YAW %.0f", me.yaw * 180.0f / e0net::kPi);
                add(0.9f, 0.95f, 1.0f, "CAMERA YAW %.0f  PITCH %.0f", camera.yaw * 180.0f / e0net::kPi, camera.pitch * 180.0f / e0net::kPi);
                add(0.9f, 0.95f, 1.0f, "STICK %.2f %.2f", input.moveX, input.moveY);
                if (e0net::isSwimmingPos(me.x, me.y, me.z))
                    add(0.45f, 0.85f, 1.0f, "%s  NA AGUA", runPtr >= 0 ? "NADA RAPIDO" : "NADA");
                else
                    add(0.9f, 0.95f, 1.0f, "%s  %s", runPtr >= 0 ? "CORRE" : "ANDA", me.y <= e0net::supportHeightAt(me.x, me.z) + 0.03f ? "NO CHAO" : "NO AR");
                {
                    const e0net::ShipState& SS = curShip();
                    if (SS.exists) {
                        add(0.55f, 0.82f, 1.0f, "%s %s  POS %.0f %.0f  VEL %.1f  LEME %.2f", curDef().name, me.ship == 2 ? "NO LEME" : (me.ship == 1 ? "A BORDO" : "FORA"), SS.x, SS.z, SS.speed, SS.steer);
                        if (me.ship) add(0.55f, 0.82f, 1.0f, "NO CONVES A %.1f B %.1f  (LEME A %.1f)", me.sa, me.sb, curDef().helmA);
                    } else add(0.55f, 0.82f, 1.0f, "%s", "NAVIO: NENHUM");
                }
                break;
            case 3: {  // MUNDO
                {   // linha 0: seletor de hora (mesmo estilo dos de GRAF/CAM): cada toque avança para o próximo horário
                    int bi = timeIdx;
                    if (bi < 0) {  // até o 1º toque, mostra o preset mais próximo da hora atual
                        float best = 1e9f; bi = 0;
                        for (int i = 0; i < 6; ++i) { float d = std::fabs(t - kTimePresets[i].cycleSec); d = std::min(d, e0net::kCycleSec - d); if (d < best) { best = d; bi = i; } }
                    }
                    add(1.0f, 0.92f, 0.55f, "HORA %s %02d:%02d  <TOQUE>", kTimePresets[bi].name, hh, mm);
                }
                if (net.timeReq() == NetClient::TimeReq::Refused) add(1.0f, 0.45f, 0.35f, "%s", "SERVIDOR RECUSOU (INICIE COM --DEBUG)");
                add(0.9f, 0.95f, 1.0f, "%s EM %d:%02d", day ? "NOITE" : "DIA", rem / 60, rem % 60);
                add(0.8f, 0.85f, 1.0f, "CICLO %.0f/%.0f S", t, e0net::kCycleSec);
                add(0.8f, 0.85f, 1.0f, "DIA %.0f S  NOITE %.0f S", e0net::kDayLengthSec, e0net::kNightLengthSec);
                add(0.9f, 0.95f, 1.0f, "SOL ALTURA %.2f", env.sunDir.y);
                add(0.9f, 0.95f, 1.0f, "LUZ %.2f %.2f %.2f", env.lightCol.x, env.lightCol.y, env.lightCol.z);
                add(0.9f, 0.95f, 1.0f, "AMBIENTE %.2f %.2f %.2f", env.ambient.x, env.ambient.y, env.ambient.z);
                add(0.9f, 0.95f, 1.0f, "NOITE %.0f%%  ESTRELAS %s", env.night * 100.0f, env.night > 0.5f ? "SIM" : "NAO");
                break;
            }
            case 5: {  // GRAF: sombra, reflexo, LOD e custo de desenho
                const GfxStats& g = renderer.gfx();
                add(1.0f, 0.92f, 0.55f, "QUALIDADE %s  [%s]  <TOQUE>", Renderer::qualityName(g.quality), qMode < 0 ? "AUTO" : "FIXA");
                add(1.0f, 0.78f, 0.35f, "FILTRO PIRATA  [%s]  <TOQUE>", gradeMode == 0 ? "OFF" : (gradeMode == 1 ? "SUAVE" : "FORTE"));
                if (g.shadowOn) add(0.9f, 0.95f, 1.0f, "SOMBRA %d PX  RAIO %.0f M  CASTERS %d", g.shadowRes, g.shadowRadius, g.shadowCasters);
                else add(0.7f, 0.75f, 0.9f, "%s", "SOMBRA OFF (SEM LUZ DIRETA)");
                add(0.9f, 0.95f, 1.0f, "TRIANGULOS %.1fK  DRAWS %d", g.tris / 1000.0f, g.draws);
                add(0.9f, 0.95f, 1.0f, "LOD0 %d  LOD1 %d  LOD2 %d", g.lod[0], g.lod[1], g.lod[2]);
                add(0.8f, 0.85f, 1.0f, "%s", "MALHA 55.3K / 11.1K / 2.8K TRIS");
                if (g.gpuMs >= 0) add(g.gpuMs > 15.5f ? 1.0f : 0.45f, g.gpuMs > 15.5f ? 0.45f : 1.0f, 0.45f, "GPU %.1f MS  (LIMITE 15.5)", g.gpuMs);
                else add(0.7f, 0.75f, 0.9f, "%s", "GPU TIMER INDISPONIVEL");
                add(0.8f, 0.85f, 1.0f, "AUTO: LENTO %d RAPIDO %d", slowCnt, fastCnt);
                break;
            }
            case 6: {  // CAM: sistema de câmera
                const CameraInfo& ci = rig.info();
                static const char* kSide[3] = {"ESQUERDO", "CENTRO", "DIREITO"};
                add(1.0f, 0.92f, 0.55f, "PRESET %s  <TOQUE>", CameraRig::presetName(rig.preset()));
                add(1.0f, 0.92f, 0.55f, "OMBRO %s  <TOQUE>", kSide[rig.shoulder() + 1]);
                add(1.0f, 0.92f, 0.55f, "SENSIBILIDADE %.1fX  <TOQUE>", rig.sensitivity());
                add(ci.collided ? 1.0f : 0.9f, ci.collided ? 0.6f : 0.95f, ci.collided ? 0.3f : 1.0f, "BRACO %.2f/%.2f M  COLISAO %s", ci.armActual, ci.armDesired, ci.collided ? "SIM" : "NAO");
                add(0.9f, 0.95f, 1.0f, "FOV H %.0f  V %.0f  (HOR+)", ci.fovH, ci.fovV);
                add(0.9f, 0.95f, 1.0f, "CORRIDA %.0f%%  1A PESSOA %.0f%%", ci.sprint * 100.0f, ci.fp * 100.0f);
                add(0.9f, 0.95f, 1.0f, "TREMOR %.2f  OLHAR %.1f RAD/S", ci.trauma, ci.lookVel);
                add(0.9f, 0.95f, 1.0f, "PIVO %.1f %.1f %.1f", ci.pivot.x, ci.pivot.y, ci.pivot.z);
                add(0.8f, 0.85f, 1.0f, "PERSONAGEM %s", rig.hideLocal() ? "OCULTO" : "VISIVEL");
                break;
            }
            case 7: {  // TERRA: o mundo gerado
                const e0net::World* W = net.world();
                const GfxStats& g = renderer.gfx();
                add(1.0f, 0.92f, 0.55f, "MUNDO %s", net.worldStatus());
                if (!W) { add(0.7f, 0.75f, 0.9f, "%s", "SEM TERRENO (CHAO PLANO / CARREGANDO)"); break; }
                static const char* kBiome[e0net::BiomeCount] = {"MAR", "AREIA", "TERRA", "GRAMA", "FLORESTA", "PEDRA", "PISO"};
                char feats[64] = "";
                if (W->features & e0net::FeatSand) std::strcat(feats, "AREIA ");
                if (W->features & e0net::FeatDirt) std::strcat(feats, "TERRA ");
                if (W->features & e0net::FeatForest) std::strcat(feats, "FLORESTA ");
                if (W->features & e0net::FeatRocks) std::strcat(feats, "PEDRAS ");
                if (W->features & e0net::FeatBuildings) std::strcat(feats, "CONSTR ");
                if (W->features & e0net::FeatCliffCoast) std::strcat(feats, "PENHASCOS ");
                add(1.0f, 0.92f, 0.55f, "IR PARA %s  <TOQUE>", teleIdx >= 0 ? tele[teleIdx].name : (tele.empty() ? "-" : "(PROXIMO)"));
                add(0.9f, 0.95f, 1.0f, "SEMENTE %u  HASH %08X", W->seed, W->hash);
                add(0.9f, 0.95f, 1.0f, "ILHA %.0f M  PICO %.0f M  (MAX 500 M)", wstats.extentM, wstats.maxH);
                add(0.8f, 0.85f, 1.0f, "TEM: %s", feats);
                add(0.8f, 0.85f, 1.0f, "AREIA %.1f TERRA %.1f GRAMA %.1f %%", wstats.pct[1], wstats.pct[2], wstats.pct[3]);
                add(0.8f, 0.85f, 1.0f, "FLORESTA %.1f PEDRA %.1f %%", wstats.pct[4], wstats.pct[5]);
                add(0.8f, 0.85f, 1.0f, "ARVORES %d PEDRAS %d CONSTR %d", wstats.count[1] + wstats.count[2], wstats.count[3], wstats.count[4] + wstats.count[5] + wstats.count[6]);
                const e0net::Terrain& TT = e0net::terrain();
                float px = me.x, pz = me.z, sx = (TT.heightAt(px + 1, pz) - TT.heightAt(px - 1, pz)) * 0.5f, sz = (TT.heightAt(px, pz + 1) - TT.heightAt(px, pz - 1)) * 0.5f;
                add(1.0f, 0.92f, 0.55f, "AQUI: %s  ALT %.1f M  INCL %.0f%%", kBiome[TT.biomeAt(px, pz) % e0net::BiomeCount], TT.heightAt(px, pz), std::sqrt(sx * sx + sz * sz) * 100.0f);
                {   // ilha sob os pés e quantas já foram geradas neste aparelho
                    int isl = TT.slotAt(px, pz), loaded = 0;
                    for (int k = 1; k < e0net::kIslandSlots; ++k) if (net.island(k)) ++loaded;
                    const e0net::World* IW = net.island(isl);
                    add(0.6f, 0.9f, 1.0f, "OCEANO: %s (ILHA %d)  %d ILHAS GERADAS", IW ? e0net::islandKindName(IW->kind) : "MAR ABERTO", isl, loaded + 1);
                }
                add(0.9f, 0.95f, 1.0f, "CHUNKS %d (FORA %d)  TRIS %.1fK", g.terrChunks, g.terrCulled, g.terrTris / 1000.0f);
                add(0.9f, 0.95f, 1.0f, "OBJETOS: %d CHUNKS  %.1fK TRIS", g.propChunks, g.propTris / 1000.0f);
                add(0.45f, 1.0f, 0.5f, "GRAMA: %d TUFOS  %.1fK TRIS", g.grassTufts, g.grassTufts * 7 / 1000.0f);
                add(1.0f, 0.85f, 0.45f, "COLETAVEIS: %d CHUNKS  %.1fK TRIS", g.smallChunks, g.smallTris / 1000.0f);
                break;
            }
            case 8: {  // GOD: modo god / cheats de debug
                add(godFly ? 0.45f : 0.85f, godFly ? 1.0f : 0.85f, 0.45f, "MODO GOD: %s", godFly ? "ATIVO" : "DESLIGADO");
                add(1.0f, 0.92f, 0.55f, "VOAR %s  <TOQUE>", godFly ? "[ATIVADO]" : "[DESATIVADO]");
                add(1.0f, 0.92f, 0.55f, "%s", "SUBIR +5M  <TOQUE>");
                add(1.0f, 0.92f, 0.55f, "%s", "DESCER -5M  <TOQUE>");
                add(1.0f, 0.92f, 0.55f, "VEL VOO %s (%.0f M/S)  <TOQUE>", godFlyFast ? "RAPIDO" : "NORMAL", godFlyFast ? 24.0f : 9.0f);
                add(1.0f, 0.78f, 0.35f, "%s", "DAR 1X DE TODOS OS ITENS  <TOQUE>");
                add(1.0f, 0.78f, 0.35f, "%s", "MATERIAIS: +12 MADEIRA +4 PEDRA +2 CORDA  <TOQUE>");   // MELHORIAS DA ILHA (teste)
                add(1.0f, 0.82f, 0.35f, "MOEDAS %d  +100  <TOQUE>", coins);
                add(1.0f, 0.92f, 0.55f, "VIDA INFINITA %s  <TOQUE>", godLife ? "[ATIVADA]" : "[DESATIVADA]");
                add(0.55f, 1.0f, 0.55f, "%s", "VIDA +100  <TOQUE>");
                add(1.0f, 0.45f, 0.45f, "%s", "VIDA -100 (MORRER)  <TOQUE>");
                add(0.55f, 1.0f, 0.55f, "%s", "FOME +100  <TOQUE>");
                add(1.0f, 0.65f, 0.35f, "%s", "FOME -100  <TOQUE>");
                add(0.45f, 0.82f, 1.0f, "%s", "SPAWNAR NAVIO  <TOQUE>");
                add(0.45f, 0.82f, 1.0f, "%s", "SPAWNAR GALEAO  <TOQUE>");
                add(0.45f, 0.82f, 1.0f, "%s", "SPAWNAR CHALUPA  <TOQUE>");
                {
                    const int gi = hpShip(1e9f); const e0net::ShipState& SS = e0net::shipState(gi < 0 ? 0 : gi); const e0net::ShipDef& GD = e0net::shipDef(gi < 0 ? 0 : gi);
                    add(1.0f, 0.65f, 0.35f, "DANO NO NAVIO -50 (%s %d/%d)  <TOQUE>", GD.name, gi >= 0 ? (int)SS.hp : 0, GD.maxHp);
                    add(1.0f, 0.45f, 0.45f, "AFUNDAR NAVIO (%s)%s  <TOQUE>", GD.name, SS.sinkT > 0.0f ? " AFUNDANDO" : "");
                }
                add(0.45f, 0.82f, 1.0f, "%s", "SPAWNAR BERGANTIM  <TOQUE>");
                add(1.0f, 0.85f, 0.45f, "%s", "TESTAR MENSAGEM PUSH  <TOQUE>");
                add(0.55f, 0.85f, 0.75f, "%s", "CONTROLE: MIRA 3D + ANALOGICO");
                add(0.55f, 0.85f, 0.75f, "%s", "CORRER = VOO RAPIDO | PULO = SUBIR");
                add(0.9f, 0.95f, 1.0f, "ALTURA %.1f M  CHAO %.1f M", me.y, e0net::supportHeightAt(me.x, me.z));
                break;
            }
            default:  // OUTROS jogadores (interpolados)
                add(0.9f, 0.95f, 1.0f, "REMOTOS %zu", remotes.size());
                {
                    size_t shown = 0;
                    for (const auto& p : remotes) {
                        if (shown >= 10) break;
                        ++shown;
                        float dx = p.x - me.x, dz = p.z - me.z;
                        int id = p.id % e0net::kMaxPlayers;
                        add(0.9f, 0.95f, 1.0f, "ID %d  POS %.1f %.1f %.1f  [%s]", p.id, p.x, p.y, p.z, remoteAnims[id].activeName());
                        add(0.7f, 0.75f, 0.9f, "   DIST %.1f M  VEL %.1f", std::sqrt(dx * dx + dz * dz), p.speed);
                    }
                    if (remotes.size() > shown) add(0.7f, 0.85f, 1.0f, "... E MAIS %zu JOGADORES", remotes.size() - shown);
                }
                if (remotes.empty()) add(0.7f, 0.75f, 0.9f, "%s", "NENHUM OUTRO JOGADOR");
                break;
            }
        }

        int sc = std::max(2, h / 360);
        float pad = 3.0f * sc, lineH = 9.0f * sc, margin = 6.0f * sc, tpad = 2.0f * sc, gap = 2.0f * sc;

        // largura: maior entre as linhas e a barra de abas (painel de debug continua usando fonte mono 5x7)
        float toggleW = Renderer::monoTextWidth(dbgOpen ? "-" : "+", sc) + tpad * 2;
        float barW = toggleW + gap;
        for (int i = 0; i < kTabs; ++i) barW += Renderer::monoTextWidth(kTabNames[i], sc) + tpad * 2 + gap;
        char collapsed[64];
        std::snprintf(collapsed, sizeof collapsed, "FPS %.0f  PING %s", fps, ping);
        float maxW = dbgOpen ? barW - gap : toggleW + gap + Renderer::monoTextWidth(collapsed, sc);
        for (const Line& l : L) maxW = std::max(maxW, Renderer::monoTextWidth(l.t, sc));
        dbgW = maxW + pad * 2;
        dbgPad = pad; dbgLineH = lineH;
        dbgBarH = lineH + sc;
        dbgH = dbgBarH + (dbgOpen ? L.size() * lineH + pad : 0) + pad - sc;
        dbgX = w - margin - dbgW; dbgY = std::max(margin * 1.5f, 38.0f * (float)uiScale()) + 10.0f * (float)uiScale();   // abaixo da linha de texto da dominação
        if (uiFull()) { dbgW = 0; return; }

        renderer.beginMonoText();
        renderer.monoRect(dbgX, dbgY, dbgW, dbgH, 0.0f, 0.0f, 0.0f, 0.55f);
        float x = dbgX + pad, ty = dbgY + pad;
        renderer.monoRect(x, ty - sc, toggleW, lineH, 0.25f, 0.28f, 0.35f, 0.9f);
        renderer.monoText(x + tpad, ty, sc, dbgOpen ? "-" : "+", 1, 1, 1, 1, false);
        dbgToggleEnd = x + toggleW + gap * 0.5f;
        x += toggleW + gap;
        if (dbgOpen) {
            for (int i = 0; i < kTabs; ++i) {
                float tw = Renderer::monoTextWidth(kTabNames[i], sc) + tpad * 2;
                bool sel = i == dbgTab;
                renderer.monoRect(x, ty - sc, tw, lineH, sel ? 0.25f : 0.12f, sel ? 0.55f : 0.14f, sel ? 0.95f : 0.18f, sel ? 0.95f : 0.8f);
                renderer.monoText(x + tpad, ty, sc, kTabNames[i], sel ? 1.0f : 0.7f, sel ? 1.0f : 0.75f, sel ? 1.0f : 0.8f, 1.0f, false);
                tabX[i] = x - gap * 0.5f; tabW[i] = tw + gap;
                x += tw + gap;
            }
            float y = dbgY + pad + dbgBarH;
            for (const Line& l : L) {
                renderer.monoText(dbgX + dbgW - pad - Renderer::monoTextWidth(l.t, sc), y, sc, l.t, l.r, l.g, l.b, 1.0f);
                y += lineH;
            }
        } else {
            renderer.monoText(x, ty, sc, collapsed, fr, fg, fb, 1.0f);
        }
        renderer.endMonoText();

        logAcc += rawDt;  // resumo no logcat a cada 5 s (facilita diagnóstico remoto)
        if (logAcc >= 2.0) {
            logAcc = 0;
            LOG("DEBUG gfx=%s gpu=%.1fms tris=%dk fps=%.0f ms=%.1f pior=%.1f hora=%02d:%02d %s rede=%s ping=%.0f min=%.0f max=%.0f jit=%.1f snap=%d/s rx=%.1f tx=%.1f corr=%.2f",
                Renderer::qualityName(renderer.gfx().quality), renderer.gfx().gpuMs, renderer.gfx().tris / 1000, fps, frameMs, worstMs, hh, mm, day ? "dia" : "noite", ns, st.rttMs, st.rttMin, st.rttMax, st.rttJit, st.snapPerSec, st.rxKBs, st.txKBs, st.corrMax);
        }
    }
#else
    void drawDebug(float) {}   // build de produção: sem painel de debug
#endif

    // MAPA DOS MARES: Carta Náutica em tela cheia sobre a mesa de carvalho (referência Sea of Thieves / AC Black Flag),
    // com grid alfanumérico preso ao mundo (A..Z x 1..26), trilhos de coordenadas vivos nas 4 bordas, oceano azul-petróleo
    // vintage com batimetria, rosa dos ventos + linhas de rumo e escala gravados direto na carta (zero dashboard).
    float mapU() const { return std::min((float)h / 1080.0f, (float)w / 1900.0f); }
    void mapRose(float& rx, float& ry, float& rr) const {
        const float u = mapU(); const Rect2D ch = mapChartRect();
        const float pad = mapPadIn();
        rr = 76.0f * u;
        rx = ch.x + pad + rr + 44.0f * u;
        ry = ch.y + ch.h - pad - rr - 44.0f * u;
    }
    // barra de escala náutica dinâmica (escolhe um passo redondo em metros que caiba em ~140..280 px)
    void mapScaleBar(float& barPx, float& meters) const {
        const float u = mapU(); const float pxPerM = mapSide() / (2.0f * mapHalf);
        meters = 500.0f;
        for (float c : {100.0f, 200.0f, 250.0f, 500.0f, 1000.0f}) {
            if (c * pxPerM >= 130.0f * u && c * pxPerM <= 290.0f * u) { meters = c; break; }
            meters = c;
        }
        barPx = std::max(90.0f * u, std::min(290.0f * u, meters * pxPerM));
    }
    void drawMapShapes() {
        const float mu = mapU();
        const Rect2D ch = mapChartRect();
        const float cx = ch.x + ch.w * 0.5f, cy = ch.y + ch.h * 0.5f;
        const float hw = ch.w * 0.5f, hh = ch.h * 0.5f;
        const float padOut = mapPadOut(), padIn = mapPadIn();
        const float inX0 = ch.x + padIn, inY0 = ch.y + padIn;
        const float inX1 = ch.x + ch.w - padIn, inY1 = ch.y + ch.h - padIn;
        const float inW = inX1 - inX0, inH = inY1 - inY0;
        const e0net::PlayerState me = net.visual();
        const float vcx = me.x + mapPanX, vcz = me.z + mapPanZ;
        const float hx = mapHX();

        // 1) Mesa de carvalho escura nas bordas da tela + moldura da carta em madeira & latão
        renderer.uiWoodPanel(w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f, theme::PanelBgWood, 1.0f, 8.0f);
        renderer.uiWoodPanel(cx, cy, hw + 10.0f * mu, hh + 10.0f * mu, 12.0f * mu, theme::PanelWood, 0.99f, 4.0f);

        // 2) Folha da Carta Náutica (pergaminho dobrado + aguada azul-petróleo do oceano + ilhas em aquarela/relevo/batimetria)
        float rx, ry, rr; mapRose(rx, ry, rr);
        const float pv = 0.5f * ch.h / std::max(1.0f, ch.w);
        renderer.uiMapSheet(2, true, cx, cy, hw + 6.0f * mu, hh + 6.0f * mu, cx, cy, hw - 2.0f * mu, rx, ry, rr * 1.06f, 0.0f, 0.0f, 0.5f - pv, 1.0f, 0.5f + pv, hh - 2.0f * mu);
        if (bigInk.valid) {
            float th = bigInk.half;
            float u0 = ((vcx - hx) - (bigInk.cx - th)) / (2.0f * th), u1 = ((vcx + hx) - (bigInk.cx - th)) / (2.0f * th);
            float v0 = ((vcz - mapHalf) - (bigInk.cz - th)) / (2.0f * th), v1 = ((vcz + mapHalf) - (bigInk.cz - th)) / (2.0f * th);
            renderer.uiMapSheet(3, false, cx, cy, hw, hh, cx, cy, hw - 2.0f * mu, rx, ry, rr * 1.06f, 0.0f, u0, v0, u1, v1, hh - 2.0f * mu);
        }

        // 3) GRID NÁUTICO ALFANUMÉRICO PRESO AO MUNDO (recortado estritamente dentro da área navegável inX0..inX1, inY0..inY1)
        const int cMin = (int)std::floor((vcx - hx) / kMapGridStep) + 13;
        const int cMax = (int)std::floor((vcx + hx) / kMapGridStep) + 13;
        const int rMin = (int)std::floor((vcz - mapHalf) / kMapGridStep) + 13;
        const int rMax = (int)std::floor((vcz + mapHalf) / kMapGridStep) + 13;
        const int myCol = (int)std::floor(me.x / kMapGridStep) + 13;
        const int myRow = (int)std::floor(me.z / kMapGridStep) + 13;

        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)inX0, (GLint)((float)h - inY1), (GLsizei)inW, (GLsizei)inH);
        {
            const float lw = std::max(1.0f, 1.35f * mu);
            // linhas verticais e horizontais do grid do mundo (acompanham pan e zoom!)
            for (int c = cMin; c <= cMax + 1; ++c) {
                float sx, sy; bigToScreen((float)(c - 13) * kMapGridStep, vcz, sx, sy);
                if (sx >= inX0 - 2.0f && sx <= inX1 + 2.0f)
                    renderer.uiRoundRect(sx, (inY0 + inY1) * 0.5f, lw * 0.5f, inH * 0.5f, 0.0f, 0.16f, 0.11f, 0.06f, 0.30f);
            }
            for (int r = rMin; r <= rMax + 1; ++r) {
                float sx, sy; bigToScreen(vcx, (float)(r - 13) * kMapGridStep, sx, sy);
                if (sy >= inY0 - 2.0f && sy <= inY1 + 2.0f)
                    renderer.uiRoundRect((inX0 + inX1) * 0.5f, sy, inW * 0.5f, lw * 0.5f, 0.0f, 0.16f, 0.11f, 0.06f, 0.30f);
            }
            // pequenas cruzes cartográficas (+) nos cruzamentos do grid
            const float arm = 6.0f * mu;
            for (int c = cMin; c <= cMax + 1; ++c) {
                float sx, dummyY; bigToScreen((float)(c - 13) * kMapGridStep, vcz, sx, dummyY);
                if (sx < inX0 - arm || sx > inX1 + arm) continue;
                for (int r = rMin; r <= rMax + 1; ++r) {
                    float dummyX, sy; bigToScreen(vcx, (float)(r - 13) * kMapGridStep, dummyX, sy);
                    if (sy < inY0 - arm || sy > inY1 + arm) continue;
                    renderer.uiRoundRect(sx, sy, arm, lw * 0.7f, 0.0f, 0.14f, 0.09f, 0.05f, 0.44f);
                    renderer.uiRoundRect(sx, sy, lw * 0.7f, arm, 0.0f, 0.14f, 0.09f, 0.05f, 0.44f);
                }
            }
        }
        glDisable(GL_SCISSOR_TEST);

        // 4) Trilho de Coordenadas Alfanuméricas nas 4 bordas (estilo Sea of Thieves) + moldura dupla sépia + 4 tachas de latão
        {
            const float rail = padIn - padOut;
            const float hwOut = hw - padOut, hhOut = hh - padOut;
            const float hwIn = hw - padIn, hhIn = hh - padIn;
            // faixa do trilho de coordenadas (pergaminho quente envelhecido nas 4 bordas, cobrindo o oceano nas margens)
            const float rR = 0.82f, rG = 0.75f, rB = 0.59f, rA = 0.98f;
            renderer.uiRoundRect(cx, ch.y + padOut + rail * 0.5f, hwOut, rail * 0.5f, 0.0f, rR, rG, rB, rA);
            renderer.uiRoundRect(cx, ch.y + ch.h - padOut - rail * 0.5f, hwOut, rail * 0.5f, 0.0f, rR, rG, rB, rA);
            renderer.uiRoundRect(ch.x + padOut + rail * 0.5f, cy, rail * 0.5f, hhIn, 0.0f, rR, rG, rB, rA);
            renderer.uiRoundRect(ch.x + ch.w - padOut - rail * 0.5f, cy, rail * 0.5f, hhIn, 0.0f, rR, rG, rB, rA);

            // destaque sutil na coluna e linha atual do jogador dentro do trilho
            {
                float sx0, sx1, sy0, sy1, dy, dx;
                bigToScreen((float)(myCol - 13) * kMapGridStep, vcz, sx0, dy);
                bigToScreen((float)(myCol - 12) * kMapGridStep, vcz, sx1, dy);
                float cl0 = std::max(inX0, sx0), cl1 = std::min(inX1, sx1);
                if (cl1 > cl0 + 2.0f) {
                    renderer.uiRoundRect((cl0 + cl1) * 0.5f, ch.y + padOut + rail * 0.5f, (cl1 - cl0) * 0.5f, rail * 0.44f, 0.0f, 0.58f, 0.14f, 0.09f, 0.18f);
                    renderer.uiRoundRect((cl0 + cl1) * 0.5f, ch.y + ch.h - padOut - rail * 0.5f, (cl1 - cl0) * 0.5f, rail * 0.44f, 0.0f, 0.58f, 0.14f, 0.09f, 0.18f);
                }
                bigToScreen(vcx, (float)(myRow - 13) * kMapGridStep, dx, sy0);
                bigToScreen(vcx, (float)(myRow - 12) * kMapGridStep, dx, sy1);
                float rl0 = std::max(inY0, sy0), rl1 = std::min(inY1, sy1);
                if (rl1 > rl0 + 2.0f) {
                    renderer.uiRoundRect(ch.x + padOut + rail * 0.5f, (rl0 + rl1) * 0.5f, rail * 0.44f, (rl1 - rl0) * 0.5f, 0.0f, 0.58f, 0.14f, 0.09f, 0.18f);
                    renderer.uiRoundRect(ch.x + ch.w - padOut - rail * 0.5f, (rl0 + rl1) * 0.5f, rail * 0.44f, (rl1 - rl0) * 0.5f, 0.0f, 0.58f, 0.14f, 0.09f, 0.18f);
                }
            }

            // traços separadores no trilho alinhados exatamente com cada linha do grid do mundo
            const float tk = std::max(1.2f, 1.6f * mu);
            for (int c = cMin; c <= cMax + 1; ++c) {
                float sx, dy; bigToScreen((float)(c - 13) * kMapGridStep, vcz, sx, dy);
                if (sx > inX0 + 2.0f && sx < inX1 - 2.0f) {
                    renderer.uiRoundRect(sx, ch.y + padOut + rail * 0.5f, tk * 0.5f, rail * 0.5f, 0.0f, 0.24f, 0.14f, 0.07f, 0.58f);
                    renderer.uiRoundRect(sx, ch.y + ch.h - padOut - rail * 0.5f, tk * 0.5f, rail * 0.5f, 0.0f, 0.24f, 0.14f, 0.07f, 0.58f);
                }
            }
            for (int r = rMin; r <= rMax + 1; ++r) {
                float dx, sy; bigToScreen(vcx, (float)(r - 13) * kMapGridStep, dx, sy);
                if (sy > inY0 + 2.0f && sy < inY1 - 2.0f) {
                    renderer.uiRoundRect(ch.x + padOut + rail * 0.5f, sy, rail * 0.5f, tk * 0.5f, 0.0f, 0.24f, 0.14f, 0.07f, 0.58f);
                    renderer.uiRoundRect(ch.x + ch.w - padOut - rail * 0.5f, sy, rail * 0.5f, tk * 0.5f, 0.0f, 0.24f, 0.14f, 0.07f, 0.58f);
                }
            }

            // moldura cartográfica dupla sépia + 4 quadrados de canto com tachas de latão
            const float th = std::max(1.5f, 2.2f * mu);
            renderer.uiRoundRectBorder(cx, cy, hwOut, hhOut, 4.0f * mu, th * 1.45f, 0.20f, 0.12f, 0.05f, 0.92f);
            renderer.uiRoundRectBorder(cx, cy, hwIn, hhIn, 2.0f * mu, th * 0.95f, 0.20f, 0.12f, 0.05f, 0.88f);
            const float ox[4] = {ch.x + padOut + rail * 0.5f, ch.x + ch.w - padOut - rail * 0.5f, ch.x + padOut + rail * 0.5f, ch.x + ch.w - padOut - rail * 0.5f};
            const float oy[4] = {ch.y + padOut + rail * 0.5f, ch.y + padOut + rail * 0.5f, ch.y + ch.h - padOut - rail * 0.5f, ch.y + ch.h - padOut - rail * 0.5f};
            for (int i = 0; i < 4; ++i) {
                renderer.uiRoundRect(ox[i], oy[i], rail * 0.5f, rail * 0.5f, 0.0f, 0.72f, 0.63f, 0.46f, 0.95f);
                renderer.uiRoundRectBorder(ox[i], oy[i], rail * 0.5f, rail * 0.5f, 0.0f, th * 0.7f, 0.20f, 0.12f, 0.05f, 0.85f);
                renderer.uiShape(Renderer::UiFill, ox[i] + 1.5f * mu, oy[i] + 2.0f * mu, 9.0f * mu, 0.0f, 0.0f, 0.0f, 0.32f);
                renderer.uiShape(Renderer::UiFill, ox[i], oy[i], 8.5f * mu, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.98f);
                renderer.uiShape(Renderer::UiRing, ox[i], oy[i], 8.5f * mu, 0.20f, 0.12f, 0.05f, 0.92f);
                renderer.uiShape(Renderer::UiFill, ox[i] - 1.8f * mu, oy[i] - 1.8f * mu, 2.8f * mu, 1.0f, 0.93f, 0.72f, 0.78f);
            }
        }

        // 5) Marcadores e Rota estilo Sea of Thieves (recortados estritamente na área interna inX0..inX1, inY0..inY1)
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)inX0, (GLint)((float)h - inY1), (GLsizei)inW, (GLsizei)inH);
        auto inside = [&](float sx, float sy, float m) { return sx > inX0 + m && sx < inX1 - m && sy > inY0 + m && sy < inY1 - m; };
        float sx, sy, mk = std::max(10.0f, 0.022f * ch.h);
        for (const auto& p : remotes) { if (crewMember(p.id)) continue; bigToScreen(p.x, p.z, sx, sy); if (inside(sx, sy, mk) && std::hypot(p.x - me.x, p.z - me.z) < kBigView) renderer.uiMarker(2, sx, sy, mk * 0.75f, 0.0f); }
        if (net.teamPosAge() < 4.0) for (const auto& p : net.teamPositions()) {
            float px = p.x, pz = p.z;
            for (const auto& q : remotes) if (q.id == p.id) { px = q.x; pz = q.z; break; }
            bigToScreen(px, pz, sx, sy); if (inside(sx, sy, mk)) renderer.uiMarker(3, sx, sy, mk * 0.95f, 0.0f);
        }
        for (int sl = 1; sl < e0net::kIslandSlots; ++sl) {   // DOMINAÇÃO: bandeira da tripulação dona no mastro de cada ilha
            if (!net.isleOwner(sl).team) continue;
            float fx, fy, fz; if (!islePoint(sl, fx, fy, fz)) continue;
            bigToScreen(fx, fz, sx, sy);
            if (inside(sx, sy, mk * 1.6f)) drawIsleMapFlag(sl, sx, sy, mk * 1.15f, 1.0f);
        }
        for (int si = 0; si < e0net::kMaxShips; ++si) {
            const e0net::ShipState& SH = e0net::shipState(si);
            if (SH.exists && std::hypot(SH.x - me.x, SH.z - me.z) < kBigView) {
                bigToScreen(SH.x, SH.z, sx, sy);
                if (inside(sx, sy, mk)) renderer.uiMarker(1, sx, sy, mk * (si == e0net::ShipGalleon ? 1.4f : (si == e0net::ShipBoat ? 0.8f : (si == e0net::ShipBrig ? 1.15f : 1.05f))), SH.yaw);
            }
        }
        if (wpSet) {
            float ax, ay, bx, by; bigToScreen(me.x, me.z, ax, ay); bigToScreen(wpX, wpZ, bx, by);
            const int dots = std::max(10, std::min(72, (int)(std::hypot(bx - ax, by - ay) / (16.0f * mu))));
            for (int i = 1; i < dots; ++i) {
                float t = (float)i / (float)dots, px = ax + (bx - ax) * t, py = ay + (by - ay) * t;
                renderer.uiShape(Renderer::UiFill, px, py, mk * 0.14f, 0.56f, 0.09f, 0.07f, 0.86f);
            }
            // marcação de lápis/carvão carmesim no destino (anel fino + X gravado)
            renderer.uiShape(Renderer::UiFill, bx, by, mk * 0.95f, 0.88f, 0.80f, 0.62f, 0.52f);
            renderer.uiShape(Renderer::UiRing, bx, by, mk * 0.95f, 0.58f, 0.10f, 0.07f, 0.94f);
            renderer.uiShape(Renderer::UiRing, bx, by, mk * 0.72f, 0.58f, 0.10f, 0.07f, 0.45f);
        }
        bigToScreen(me.x, me.z, sx, sy); if (inside(sx, sy, mk)) renderer.uiMarker(0, sx, sy, mk * 1.18f, me.yaw);
        glDisable(GL_SCISSOR_TEST);

        // 6) Rosa dos Ventos + Régua de Escala Gráfica GRAVADAS DIRETO NA CARTA (sem caixas pretas!)
        renderer.uiCompass(rx, ry, rr, 0.94f);
        {
            float barPx, meters; mapScaleBar(barPx, meters);
            const float bx0 = rx + rr + 54.0f * mu, bx1 = bx0 + barPx;
            const float by = ch.y + ch.h - padIn - 28.0f * mu;
            const float bh = 7.0f * mu;
            for (int s = 0; s < 4; ++s) {
                float sx0 = bx0 + barPx * (float)s / 4.0f, sw = barPx * 0.25f;
                renderer.uiRoundRect(sx0 + sw * 0.5f, by, sw * 0.5f, bh * 0.5f, 0.5f,
                                     (s & 1) ? 0.86f : 0.20f, (s & 1) ? 0.79f : 0.12f, (s & 1) ? 0.62f : 0.06f, 0.88f);
            }
            renderer.uiRoundRectBorder((bx0 + bx1) * 0.5f, by, barPx * 0.5f, bh * 0.5f, 0.5f, 1.4f * mu, 0.18f, 0.11f, 0.05f, 0.92f);
            // filete do título cartográfico gravado no canto superior esquerdo (sem fundo escuro!)
            const float tx0 = inX0 + 26.0f * mu, ty0 = inY0 + 62.0f * mu;
            renderer.uiRoundRect(tx0 + 115.0f * mu, ty0, 115.0f * mu, 1.0f * mu, 0.0f, 0.20f, 0.12f, 0.06f, 0.55f);
        }

        // 7) Controles tateis compactos apoiados no canto inferior direito + Selo de Fechar no canto superior direito
        {
            const Rect2D cb = mapCenterBtnRect();
            renderer.uiButton(cb.x + cb.w * 0.5f, cb.y + cb.h * 0.5f, cb.w * 0.5f, cb.h * 0.5f, 9.0f * mu, theme::BtnWood, 0.94f);
            for (int k = 0; k < 2; ++k) {
                const Rect2D zb = mapZoomBtnRect(k);
                renderer.uiButton(zb.x + zb.w * 0.5f, zb.y + zb.h * 0.5f, zb.w * 0.5f, zb.h * 0.5f, 9.0f * mu, theme::BtnWood, 0.94f);
            }
            if (wpSet) {
                const Rect2D lb = mapClearBtnRect();
                renderer.uiButton(lb.x + lb.w * 0.5f, lb.y + lb.h * 0.5f, lb.w * 0.5f, lb.h * 0.5f, 9.0f * mu, theme::BtnCrimson, 0.94f);
            }
            const Rect2D cl = mapCloseRect();
            closeBtnShapes(cl.x + cl.w * 0.5f, cl.y + cl.h * 0.5f, cl.w * 0.5f);
        }
    }
    void drawMapText(const std::function<void(const Rect2D&, const char*, int, float, float, float, float, float, float, float)>& /*drawBtnBox*/) {
        const float mu = mapU();
        const Rect2D ch = mapChartRect();
        const float padOut = mapPadOut(), padIn = mapPadIn(), rail = padIn - padOut;
        const float inX0 = ch.x + padIn, inY0 = ch.y + padIn;
        const float inX1 = ch.x + ch.w - padIn, inY1 = ch.y + ch.h - padIn;
        const float pxPerM = mapSide() / (2.0f * mapHalf);
        const e0net::PlayerState me = net.visual();
        const float vcx = me.x + mapPanX, vcz = me.z + mapPanZ, hx = mapHX();

        // helper: texto gravado em tinta sépia direta sobre a carta (com micro-halo claro de pergaminho para leitura impecável)
        auto inkText = [&](float x, float y, int ts, const char* s, float r, float g, float b, float a) {
            renderer.text(x + 1.2f * mu, y + 1.2f * mu, ts, s, 0.88f, 0.82f, 0.68f, a * 0.55f, false);
            renderer.text(x, y, ts, s, r, g, b, a, false);
        };

        // 1) COORDENADAS ALFANUMÉRICAS VIVAS NOS 4 TRILHOS DA MOLDURA (A..Z em cima/baixo, 1..26 na esquerda/direita)
        {
            const int cMin = (int)std::floor((vcx - hx) / kMapGridStep) + 13;
            const int cMax = (int)std::floor((vcx + hx) / kMapGridStep) + 13;
            const int rMin = (int)std::floor((vcz - mapHalf) / kMapGridStep) + 13;
            const int rMax = (int)std::floor((vcz + mapHalf) / kMapGridStep) + 13;
            const int myCol = (int)std::floor(me.x / kMapGridStep) + 13;
            const int myRow = (int)std::floor(me.z / kMapGridStep) + 13;
            const int gt = std::max(2, (int)std::lround(2.3f * mu));
            const float topY = ch.y + padOut + (rail - 7.0f * gt) * 0.5f;
            const float botY = ch.y + ch.h - padOut - rail + (rail - 7.0f * gt) * 0.5f;
            for (int c = cMin; c <= cMax; ++c) {
                float sx0, sx1, dy;
                bigToScreen((float)(c - 13) * kMapGridStep, vcz, sx0, dy);
                bigToScreen((float)(c - 12) * kMapGridStep, vcz, sx1, dy);
                float mx = (sx0 + sx1) * 0.5f;
                if (mx < inX0 + 18.0f * mu || mx > inX1 - 18.0f * mu) continue;
                char lb[8]; mapGridColLabel(c, lb, sizeof lb);
                float tw = Renderer::textWidth(lb, gt);
                bool cur = (c == myCol);
                float cr = cur ? 0.56f : 0.18f, cg = cur ? 0.09f : 0.11f, cb = cur ? 0.07f : 0.05f;
                renderer.text(mx - tw * 0.5f, topY, gt, lb, cr, cg, cb, cur ? 0.98f : 0.88f, false);
                renderer.text(mx - tw * 0.5f, botY, gt, lb, cr, cg, cb, cur ? 0.98f : 0.88f, false);
            }
            const float leftCX = ch.x + padOut + rail * 0.5f;
            const float rightCX = ch.x + ch.w - padOut - rail * 0.5f;
            for (int r = rMin; r <= rMax; ++r) {
                float dx, sy0, sy1;
                bigToScreen(vcx, (float)(r - 13) * kMapGridStep, dx, sy0);
                bigToScreen(vcx, (float)(r - 12) * kMapGridStep, dx, sy1);
                float my = (sy0 + sy1) * 0.5f;
                if (my < inY0 + 16.0f * mu || my > inY1 - 16.0f * mu) continue;
                char lb[8]; std::snprintf(lb, sizeof lb, "%d", r);
                float tw = Renderer::textWidth(lb, gt);
                bool cur = (r == myRow);
                float cr = cur ? 0.56f : 0.18f, cg = cur ? 0.09f : 0.11f, cb = cur ? 0.07f : 0.05f;
                renderer.text(leftCX - tw * 0.5f, my - 3.5f * gt, gt, lb, cr, cg, cb, cur ? 0.98f : 0.88f, false);
                renderer.text(rightCX - tw * 0.5f, my - 3.5f * gt, gt, lb, cr, cg, cb, cur ? 0.98f : 0.88f, false);
            }
        }

        // 2) Cartucho de Título Cartográfico gravado direto no pergaminho (canto superior esquerdo interno, sem caixa!)
        {
            const int tt = std::max(3, (int)std::lround(4.0f * mu));
            const int ts = std::max(2, (int)std::lround(2.2f * mu));
            const float tx = inX0 + 26.0f * mu, ty = inY0 + 22.0f * mu;
            inkText(tx, ty, tt, "MAPA DOS MARES", 0.18f, 0.10f, 0.04f, 0.92f);
            char qbuf[32], qc[12];
            mapGridCoord(me.x, me.z, qc, sizeof qc);
            std::snprintf(qbuf, sizeof qbuf, "QUADRANTE %s", qc);
            inkText(tx, inY0 + 70.0f * mu, ts, qbuf, 0.30f, 0.16f, 0.08f, 0.88f);
        }

        // 3) Nomes das ilhas gravados em tinta sépia direta sobre a carta (zero retângulo bege!)
        const int sc2 = std::max(2, (int)std::lround(2.5f * mu));
        for (int sl = 0; sl < e0net::kIslandSlots; ++sl) {
            int sx, sz; e0net::sectorOfSlot(sl, sx, sz);
            e0net::IslandSpec sp = e0net::islandSpec(e0net::terrain().mainSeed(), sx, sz);
            if (!sp.exists) continue;
            float ox = sp.oxcm * 0.01f, oz = sp.ozcm * 0.01f;
            if (std::hypot(ox - me.x, oz - me.z) > kBigView * 0.95f) continue;
            const char* nm = sl == 0 ? "ILHA INICIAL" : e0net::islandKindName(sp.kind);
            float scx, scy; bigToScreen(ox, oz, scx, scy);
            float rr = (sp.kind == e0net::IslandBig ? 185.0f : 62.0f) * pxPerM;
            float tw = Renderer::textWidth(nm, sc2), tx = scx - tw * 0.5f, ty = scy + rr + 8.0f * mu;
            if (tx < inX0 + 16.0f * mu || tx + tw > inX1 - 16.0f * mu || ty < inY0 + 16.0f * mu || ty + 7.0f * sc2 > inY1 - 16.0f * mu) continue;
            inkText(tx, ty, sc2, nm, 0.14f, 0.08f, 0.04f, 0.96f);
        }

        // 4) Pontos cardeais na rosa dos ventos + Escala gráfica gravada direto na carta
        {
            float rx, ry, rr; mapRose(rx, ry, rr);
            const int cn = std::max(2, (int)std::lround(3.0f * mu)), co = std::max(2, (int)std::lround(2.2f * mu));
            inkText(rx - Renderer::textWidth("N", cn) * 0.5f, ry - rr - 7.0f * cn - 5.0f * mu, cn, "N", 0.58f, 0.10f, 0.08f, 0.98f);
            inkText(rx - Renderer::textWidth("S", co) * 0.5f, ry + rr + 5.0f * mu, co, "S", 0.18f, 0.11f, 0.05f, 0.90f);
            inkText(rx + rr + 8.0f * mu, ry - 3.5f * co, co, "L", 0.18f, 0.11f, 0.05f, 0.90f);
            inkText(rx - rr - Renderer::textWidth("O", co) - 8.0f * mu, ry - 3.5f * co, co, "O", 0.18f, 0.11f, 0.05f, 0.90f);

            float barPx, meters; mapScaleBar(barPx, meters);
            const float bx0 = rx + rr + 54.0f * mu, bx1 = bx0 + barPx;
            const float by = ch.y + ch.h - padIn - 28.0f * mu;
            char scTxt[32];
            if (meters >= 1000.0f) std::snprintf(scTxt, sizeof scTxt, "%.0f KM", meters / 1000.0f);
            else std::snprintf(scTxt, sizeof scTxt, "%.0f M", meters);
            inkText(bx0, by - 7.0f * co - 8.0f * mu, co, "0", 0.18f, 0.11f, 0.05f, 0.90f);
            inkText(bx1 - Renderer::textWidth(scTxt, co), by - 7.0f * co - 8.0f * mu, co, scTxt, 0.18f, 0.11f, 0.05f, 0.95f);
        }

        // 5) Marcação X carmesim da rota + distância e quadrante do destino + rótulos dos controles compactos
        {
            const int bt = std::max(2, (int)std::lround(2.4f * mu));
            const int btSm = std::max(2, (int)std::lround(2.15f * mu));
            const int zt = std::max(3, (int)std::lround(4.2f * mu));
            const Rect2D cb = mapCenterBtnRect();
            renderer.text(cb.x + (cb.w - Renderer::textWidth("CENTRAR", bt)) * 0.5f, cb.y + (cb.h - 7.0f * bt) * 0.5f, bt, "CENTRAR", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
            const char* zl[2] = {"+", "-"};
            for (int k = 0; k < 2; ++k) {
                const Rect2D zb = mapZoomBtnRect(k);
                renderer.text(zb.x + (zb.w - Renderer::textWidth(zl[k], zt)) * 0.5f, zb.y + (zb.h - 7.0f * zt) * 0.5f, zt, zl[k], theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f);
            }
            if (wpSet) {
                const Rect2D lb = mapClearBtnRect();
                renderer.text(lb.x + (lb.w - Renderer::textWidth("LIMPAR ROTA", btSm)) * 0.5f, lb.y + (lb.h - 7.0f * btSm) * 0.5f, btSm, "LIMPAR ROTA", theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f);
                float bx, by; bigToScreen(wpX, wpZ, bx, by);
                if (bx > inX0 + 16.0f * mu && bx < inX1 - 16.0f * mu && by > inY0 + 16.0f * mu && by < inY1 - 16.0f * mu) {
                    const int xt = std::max(3, (int)std::lround(3.4f * mu));
                    inkText(bx - Renderer::textWidth("X", xt) * 0.5f, by - 3.5f * xt, xt, "X", 0.58f, 0.08f, 0.06f, 0.98f);
                    char dt[24], wc[12], full[48];
                    fmtDist(dt, sizeof dt, std::hypot(wpX - me.x, wpZ - me.z));
                    mapGridCoord(wpX, wpZ, wc, sizeof wc);
                    std::snprintf(full, sizeof full, "%s  (%s)", dt, wc);
                    const float dw = Renderer::textWidth(full, btSm);
                    inkText(bx - dw * 0.5f, by + 24.0f * mu, btSm, full, 0.44f, 0.09f, 0.06f, 0.98f);
                }
            }
        }
    }

    void drawInvShapes() {
        InvGeo g = invGeo(); const float u = g.u;
        renderer.uiWoodPanel(w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f, theme::PanelBgWood, 0.92f, 5.0f);
        renderer.uiGlow(w * 0.5f, h * 0.48f, w * 0.52f, h * 0.62f, 0.92f, 0.64f, 0.26f, 0.07f);
        // painel principal englobando a mochila + acesso rápido
        const float gw = kInvCols * g.slot + (kInvCols - 1) * g.gap;
        const Rect2D r0 = invSlotRect(0), rLast = invSlotRect(kInvSlots - 1);
        const float gy0 = r0.y - 18 * u, gy1 = rLast.y + rLast.h + 18 * u;
        renderer.uiWoodPanel(g.gx + gw * 0.5f, (gy0 + gy1) * 0.5f, gw * 0.5f + 22 * u, (gy1 - gy0) * 0.5f, 18 * u, theme::PanelWood, 0.97f, 1.0f);
        // barra de peso (trilho carvalho + preenchimento verde-musgo/latão/carmesim)
        float fill = std::min(1.0f, invWeight() / kInvMaxKg);
        float bw = 560 * u, bx = g.gx + gw * 0.5f - bw * 0.5f, by = 66 * u;
        renderer.uiRoundRect(bx + bw * 0.5f, by + 9 * u, bw * 0.5f, 9 * u, 6 * u, 0.07f, 0.05f, 0.035f, 0.92f);
        renderer.uiRoundRectBorder(bx + bw * 0.5f, by + 9 * u, bw * 0.5f, 9 * u, 6 * u, 1.4f * u, theme::kBrassDim.r, theme::kBrassDim.g, theme::kBrassDim.b, 0.72f);
        if (fill > 0.01f) renderer.uiGrad(bx + bw * fill * 0.5f, by + 9 * u, bw * fill * 0.5f, 8 * u, 5 * u,
                                          fill > 0.85f ? 0.82f : (fill > 0.60f ? theme::kBrassBright.r : 0.42f),
                                          fill > 0.85f ? 0.26f : (fill > 0.60f ? theme::kBrassBright.g : 0.72f),
                                          fill > 0.85f ? 0.20f : (fill > 0.60f ? theme::kBrassBright.b : 0.44f), 0.96f,
                                          fill > 0.85f ? 0.54f : (fill > 0.60f ? 0.66f : 0.24f),
                                          fill > 0.85f ? 0.14f : (fill > 0.60f ? 0.46f : 0.48f),
                                          fill > 0.85f ? 0.10f : (fill > 0.60f ? 0.16f : 0.26f), 0.96f, 1.0f, 0.0f);
        renderer.uiDivider(g.margin + 145 * u, 88 * u, 145 * u, 6 * u, 0.72f);
        for (int i = 0; i < kInvSlots; ++i) {
            Rect2D r = invSlotRect(i); float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hw = r.w * 0.5f;
            bool hot = i >= kInvHotFirst, sel = (i == invSel), cur = hot && (i - kInvHotFirst == hotbarCur);
            const int st = (sel || cur) ? theme::PanelCardSel : (hot ? theme::PanelSlotHot : theme::PanelSlot);
            renderer.uiWoodPanel(cx, cy, hw, hw, 10 * u, st, 0.96f, (float)i);
            if (inv[i].id) {
                float a = (invDragMoved && invDragFrom == i) ? 0.30f : 1.0f;
                drawItemIcon(cx, cy - 10 * u, hw * 0.50f, inv[i].id, a);
            }
        }
        // botões laterais rústicos unificados
        bool has = invSel >= 0 && inv[invSel].id, canSplit = has && inv[invSel].n >= 2;
        if (invDragPtr >= 0 && invDragMoved) has = canSplit = true;
        const int btnSt[5] = {
            canSplit ? theme::BtnWood : theme::BtnDisabled,
            has ? theme::BtnCrimson : theme::BtnDisabled,
            has ? theme::BtnWood : theme::BtnDisabled,
            theme::BtnBrass,
            has ? theme::BtnMoss : theme::BtnDisabled,
        };
        for (int k = 0; k < 5; ++k) {
            Rect2D b = invBtnRect(k);
            const bool over = invDragPtr >= 0 && invDragMoved && b.contains(invDragX, invDragY);
            renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12 * u, over ? theme::BtnBrass : btnSt[k], 1.0f);
        }
        if (invDragPtr >= 0 && invDragMoved && inv[invDragFrom].id) drawItemIcon(invDragX, invDragY, g.slot * 0.42f, inv[invDragFrom].id, 0.92f);
    }
    void drawInvText() {
        InvGeo g = invGeo(); const float u = g.u;
        const int t1 = std::max(1, (int)std::lround(2.1f * u)), t2 = std::max(2, (int)std::lround(3.0f * u)), t3 = std::max(3, (int)std::lround(4.8f * u));
        auto boxLbl = [&](const Rect2D& r, const char* label, int ts, theme::Col fg, bool sh) {
            renderer.text(r.x + (r.w - Renderer::textWidth(label, ts)) * 0.5f, r.y + (r.h - 7.0f * ts) * 0.5f, ts, label, fg.r, fg.g, fg.b, 1.0f, sh);
        };
        auto ctr = [&](float cx, float y, int ts, const char* s, float r, float gg, float b, float a) {
            renderer.text(cx - Renderer::textWidth(s, ts) * 0.5f, y, ts, s, r, gg, b, a);
        };
        char buf[96];
        renderer.text(g.margin, 32 * u, t3, "INVENT\xC1RIO", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        float bw = 560 * u, bx = g.gx + (kInvCols * g.slot + (kInvCols - 1) * g.gap) * 0.5f - bw * 0.5f;
        std::snprintf(buf, sizeof buf, "PESO %.1f/%.0f KG   SLOTS %d/%d", invWeight(), kInvMaxKg, invUsed(), kInvSlots);
        ctr(bx + bw * 0.5f, 34 * u, t2, buf, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f);
        closeBtnQueue(invCloseRect());

        Rect2D drop = invDropRect();
        Rect2D hr = invSlotRect(kInvHotFirst);
        renderer.text(g.gx, hr.y - 26 * u, t1, "ACESSO R\xC1PIDO", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f);
        for (int i = 0; i < kInvSlots; ++i) {
            Rect2D r = invSlotRect(i);
            bool covered = drop.w > 0 && i != invSel && r.x < drop.x + drop.w && r.x + r.w > drop.x && r.y < drop.y + drop.h && r.y + r.h > drop.y;
            if (covered) continue;
            if (i >= kInvHotFirst) { char n[4]; std::snprintf(n, sizeof n, "%d", i - kInvHotFirst + 1); renderer.text(r.x + 12 * u, r.y + 11 * u, t2, n, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.85f); }
            if (!inv[i].id) continue;
            const ItemDef& d = itemDef(inv[i].id);
            float a = (invDragMoved && invDragFrom == i) ? 0.35f : 1.0f;
            { int nts = Renderer::textWidth(d.name, t2) <= r.w - 10 * u ? t2 : (Renderer::textWidth(d.name, t1) <= r.w - 8 * u ? t1 : 1);
              ctr(r.x + r.w * 0.5f, r.y + r.h - 12 * u - 7.0f * nts, nts, d.name, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f * a); }
            if (inv[i].n > 1) { std::snprintf(buf, sizeof buf, "X%d", inv[i].n); renderer.text(r.x + r.w - 10 * u - Renderer::textWidth(buf, t2), r.y + 9 * u, t2, buf, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, a); }
        }

        bool has = invSel >= 0 && inv[invSel].id;
        bool canSplit = has && inv[invSel].n >= 2;
        if (invDragPtr >= 0 && invDragMoved) has = canSplit = true;
        boxLbl(invBtnRect(0), "DIVIDIR",   t2, canSplit ? theme::kCream : theme::kCreamMuted, canSplit);
        boxLbl(invBtnRect(1), "DESCARTAR", t2, has ? theme::kCream : theme::kCreamMuted, has);
        boxLbl(invBtnRect(2), invSel >= kInvHotFirst ? "P/ MOCHILA" : "P/ ACESSO", t2, has ? theme::kCream : theme::kCreamMuted, has);
        boxLbl(invBtnRect(3), "ORGANIZAR", t2, theme::kInk, false);
        boxLbl(invBtnRect(4), "ENTREGAR",  t2, has ? theme::kCream : theme::kCreamMuted, has);

        // dropdown de detalhes em carvalho + moldura latão
        if (drop.w > 0) {
            const ItemDef& d = itemDef(inv[invSel].id); const ItemStack& st = inv[invSel];
            renderer.rect(drop.x - 3.0f, drop.y - 3.0f, drop.w + 6.0f, drop.h + 6.0f, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.92f);
            renderer.rect(drop.x, drop.y, drop.w, drop.h, theme::kWoodDeep.r, theme::kWoodDeep.g, theme::kWoodDeep.b, 0.97f);
            float tx = drop.x + 22 * u, ty = drop.y + 20 * u;
            renderer.text(tx, ty, t2, d.name, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
            renderer.text(tx, ty + 14 * u + 7.0f * t2, t1, d.d1, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.95f, false);
            renderer.text(tx, ty + 14 * u + 7.0f * t2 + 9.0f * t1 + 8 * u, t1, d.d2, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.95f, false);
            float sy = ty + 14 * u + 7.0f * t2 + 2 * (9.0f * t1 + 8 * u) + 14 * u;
            renderer.rect(tx, sy - 8 * u, drop.w - 44 * u, 2.0f, theme::kBrassDim.r, theme::kBrassDim.g, theme::kBrassDim.b, 0.60f);
            std::snprintf(buf, sizeof buf, "QUANTIDADE: %d", st.n); renderer.text(tx, sy + 4 * u, t1, buf, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f, false);
            std::snprintf(buf, sizeof buf, "PESO: %.2f KG", d.kg * st.n); renderer.text(tx, sy + 4 * u + 9.0f * t1 + 10 * u, t1, buf, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.95f, false);
            std::snprintf(buf, sizeof buf, "PILHA M\xC1X: %d", d.maxStack); renderer.text(tx, sy + 4 * u + 2 * (9.0f * t1 + 10 * u), t1, buf, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.90f, false);
        }
    }

    // Notificações: canto central esquerdo, encostadas na borda; cada uma = 1 linha de tamanho FIXO, fundo preto que
    // esvanece na ponta direita, quadradinho com a cor do item e o texto (PEGOU X1 GRAVETO). Entram deslizando e somem suaves.
    // POUCA VIDA (vermelho, pulso rápido) e POUCA FOME (laranja, pulso lento): nos 4 CANTOS da tela, no formato de um "border-radius para DENTRO" (o canto é
    // preenchido e o limite é um arco côncavo), com degradê macio e discreto (shader de UI, modo 6). Passe de formas, por baixo da HUD. Some ao morrer.
    void drawLowGlow() {
        if (selfDead) return;
        double tn = std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 1000.0);
        auto glow = [&](float sev, float speed0, float speedK, float r, float g, float b) {
            float pulse = 0.5f + 0.5f * std::sin((float)tn * (speed0 + speedK * sev));
            float a = (0.16f + 0.30f * sev) * (0.35f + 0.65f * pulse);
            float s = std::min(w, h) * (0.30f + 0.14f * sev), hs = s * 0.5f;    // lado do quadrado do canto
            renderer.uiCornerGlow(hs, hs, hs, -1.0f, -1.0f, r, g, b, a);                               // topo-esquerda
            renderer.uiCornerGlow((float)w - hs, hs, hs, 1.0f, -1.0f, r, g, b, a);                     // topo-direita
            renderer.uiCornerGlow(hs, (float)h - hs, hs, -1.0f, 1.0f, r, g, b, a);                     // base-esquerda
            renderer.uiCornerGlow((float)w - hs, (float)h - hs, hs, 1.0f, 1.0f, r, g, b, a);           // base-direita
        };
        if (hunger < kLowHunger) glow(1.0f - hunger / kLowHunger, 2.5f, 3.5f, 0.95f, 0.50f, 0.05f);   // fome
        if (health < kLowHealth) glow(1.0f - health / kLowHealth, 4.0f, 6.0f, 0.85f, 0.0f, 0.0f);     // vida (por cima)
    }
    // ===================== CHAT de texto =====================
    // Botão (ao lado do botão da roda, abaixo do minimapa) abre uma caixa de texto Java com o teclado do sistema. O texto vai ao servidor, que o repassa
    // a quem está perto (inclusive a mim); cada mensagem vira um BALÃO (caixa preta) preso à cabeça de quem falou, logo acima do id, por alguns segundos.
    struct ChatBubble { char text[e0net::kChatMaxLen + 1]; float age, life; };
    std::unordered_map<int, ChatBubble> chatBubbles;   // por id do jogador (eu = net.myId(), ou 0 offline)
    bool chatInputOpen = false;                        // a caixa Java está aberta (esperando o resultado)
    Rect2D chatBtnRect() const { return menuBtnRect(1); }
    void openChat() {
        if (chatInputOpen) return;
        joyT.id = -1; lookT.id = -1;
        if (e0::chatInputOpen(app->activity, "Digite sua mensagem...")) chatInputOpen = true;
        else notify("NAO FOI POSSIVEL ABRIR O TECLADO");
    }
    static std::string utf8ToLatin1(const std::string& u) {   // UTF-8 (JNI) -> 1 byte por caractere; o que não cabe em Latin-1 sai
        std::string o;
        for (size_t i = 0; i < u.size();) {
            unsigned char c = (unsigned char)u[i];
            if (c < 0x80) { o += (char)c; ++i; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < u.size()) { unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)u[i + 1] & 0x3Fu); if (cp < 0x100) o += (char)cp; i += 2; }
            else if ((c & 0xF0) == 0xE0) i += 3; else if ((c & 0xF8) == 0xF0) i += 4; else ++i;
        }
        return o;
    }
    void updateChat(float dt) {
        if (chatInputOpen) {
            std::string r;
            if (e0::chatInputPoll(app->activity, r)) {
                chatInputOpen = false;
                if (!r.empty() && r != "\x01") net.sendChat(utf8ToLatin1(r).c_str());
            }
        }
        for (NetClient::Chat c; net.popChat(c); ) {
            if ((int)c.from == (net.connected() ? net.myId() : 0)) continue;   // o jogador não vê a própria mensagem
            ChatBubble b; std::snprintf(b.text, sizeof b.text, "%s", c.text); b.age = 0.0f;
            b.life = std::min(e0net::kChatShowMax, e0net::kChatShowSec + 0.06f * (float)std::strlen(c.text));
            chatBubbles[(int)c.from] = b;
        }
        for (auto it = chatBubbles.begin(); it != chatBubbles.end();) {
            it->second.age += dt;
            if (it->second.age >= it->second.life) it = chatBubbles.erase(it); else ++it;
        }
    }
    // ícone do botão: balão de fala (corpo + rabinho + 3 pontinhos) desenhado com retângulos
    void drawChatBtn() {
        const Rect2D r = chatBtnRect();
        hudIcon(HiChat, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.h * 0.46f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    }
    // CONTADOR DA LIMPEZA DE NAVIOS (canto inferior esquerdo): relógio UNIVERSAL do servidor que nunca para (60 -> 0 e recomeça); ao zerar o servidor
    // verifica TODOS os navios e some com os que ninguém usa (leme, canhão, a bordo ou encostado no casco). Só um texto pequeno e cinza.
    void drawShipCleanTimer() {
        const float left = net.shipCleanIn();
        if (left < 0.0f) return;
        const int sec = (int)std::ceil(std::max(0.0f, left));
        char t[48]; std::snprintf(t, sizeof t, "LIMPEZA DE NAVIOS EM %d:%02d", sec / 60, sec % 60);
        const int ts = std::max(2, uiScale() - 1);
        renderer.text(8.0f * (float)uiScale(), (float)h - 6.0f * (float)uiScale() - 7.0f * (float)ts, ts, t, 0.62f, 0.62f, 0.62f, 0.75f, false);
    }
    // TAG de TODOS os outros jogadores: "ID n" + VIVO/MORTO, presa ao OSSO DA CABEÇA (mesma pose e mesma matriz do desenho do personagem, calculadas no mesmo
    // frame em drawWorld) => acompanha a cabeça sem atraso, também deitado ou carregado. Tamanho constante na tela.
    struct TagInfo { Vec3 pos; int id; bool dead; bool self; };
    std::vector<TagInfo> tags;
    void collectTag(const CharInst& c, int id, bool dead, bool self = false) {
        if (!c.boneRows) return;
        const Vec4* ar = c.attachRows ? c.attachRows : c.boneRows;   // espaço do Y Bot -> cabeça deste personagem
        float dx = c.pos.x - camera.position.x, dz = c.pos.z - camera.position.z;
        if (dx * dx + dz * dz > 65.0f * 65.0f) return;
        const Vec3 fwd = camera.forward();
        if (dx * fwd.x + dz * fwd.z < -2.0f) return;
        const Vec4* r = ar + 5 * 3;                                           // osso 5 = Head; ponto de bind 22 cm acima da cabeça (0, 1.82, 0.02)
        const float bx = 0.0f, by = 1.86f, bz = 0.02f;   // logo acima da cabeça (os piratas não usam mais chapéu: topo a ~1,80 m)
        Vec3 q{r[0].x * bx + r[0].y * by + r[0].z * bz + r[0].w, r[1].x * bx + r[1].y * by + r[1].z * bz + r[1].w, r[2].x * bx + r[2].y * by + r[2].z * bz + r[2].w};
        Mat4 m = Mat4::translate(c.pos) * Mat4::rotY(-c.yaw);
        if (c.hasLocal) m = m * c.local;
        tags.push_back({{m.m[0] * q.x + m.m[4] * q.y + m.m[8] * q.z + m.m[12], m.m[1] * q.x + m.m[5] * q.y + m.m[9] * q.z + m.m[13] + 0.12f, m.m[2] * q.x + m.m[6] * q.y + m.m[10] * q.z + m.m[14]}, id, dead, self});
    }
    // Caixa preta com o texto numa ÚNICA linha; a base fica em `bottom`, centrada em cx. As marcas de acento sobem 3 px de fonte acima da letra.
    void drawChatBubble(const ChatBubble& b, float cx, float bottom, int ts, float al) {
        const float tw = Renderer::textWidth(b.text, ts);
        const float padX = 4.0f * ts, padT = 5.0f * ts, padB = 3.0f * ts;
        const float bw = tw + 2.0f * padX, bh = 7.0f * ts + padT + padB;
        const float fade = std::min(1.0f, std::min(b.age / 0.12f, (b.life - b.age) / 0.45f)) * al;
        const float x0 = cx - bw * 0.5f, y0 = bottom - bh - 3.0f * ts;   // parte do CENÁRIO: segue o personagem e sai da tela com ele (nada de grudar na borda)
        if (x0 + bw < 0.0f || x0 > (float)w || y0 + bh < 0.0f || y0 > (float)h) return;
        renderer.rect(x0 - 1.0f, y0 - 1.0f, bw + 2.0f, bh + 2.0f, 1.0f, 1.0f, 1.0f, 0.18f * fade);   // fio claro: lê sobre o céu e sobre o mar
        renderer.rect(x0, y0, bw, bh, 0.0f, 0.0f, 0.0f, 0.82f * fade);
        renderer.rect(cx - 2.0f * ts, y0 + bh, 4.0f * ts, 2.0f * ts, 0.0f, 0.0f, 0.0f, 0.82f * fade);   // pontinha apontando para o personagem
        renderer.text(x0 + padX, y0 + padT, ts, b.text, 1.0f, 1.0f, 1.0f, fade);
    }
    void drawTags() {
        const int ts = std::max(2, uiScale() - 1);
        const e0net::PlayerState me = net.visual();
        const float maxD2 = tags.size() > 20 ? 32.0f * 32.0f : 80.0f * 80.0f;
        int drawn = 0;
        for (const auto& t : tags) {
            if (drawn >= 16) break;
            float dx = t.pos.x - me.x, dz = t.pos.z - me.z; if (dx * dx + dz * dz > maxD2) continue;
            float px, py;
            if (!project(t.pos, px, py)) continue;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float al = std::min(1.0f, std::max(0.0f, (80.0f - dist) / 45.0f));   // tamanho fixo; some com a distância (cheio até 35 m, some aos 80 m)
            if (al <= 0.01f) continue;
            ++drawn;
            float y0 = py - 7.0f * ts;
            if (!t.self) {
                char l1[16]; std::snprintf(l1, sizeof l1, "%03d", t.id);
                const float w1 = Renderer::textWidth(l1, ts), x0 = px - w1 * 0.5f;
                renderer.text(x0 + ts, y0 + ts, ts, l1, 0.0f, 0.0f, 0.0f, 0.6f * al);   // sombra fina (sem caixa de fundo)
                const bool crew = crewMember(t.id);   // tripulante: id em verde
                renderer.text(x0, y0, ts, l1, crew ? 0.55f : 1.0f, 1.0f, crew ? 0.55f : 1.0f, al);
                const NetClient::TeamInfo& ti = net.teamInfo(t.id);
                if (ti.team) {   // bandeira da TRIPULAÇÃO à esquerda do id (desenhada no passe de UI, depois do texto)
                    const float fh = 11.0f * (float)ts, fw = fh * 1.5f;   // ~1,6x a altura do texto
                    TagFlagReq fr; fr.x = x0 - 4.0f * (float)ts - fw; fr.y = y0 + 3.5f * (float)ts - fh * 0.5f; fr.w = fw; fr.h = fh; fr.a = al; std::memcpy(fr.flag, ti.flag, e0net::kFlagBytes);
                    tagFlags.push_back(fr);
                }
            }
            auto bi = chatBubbles.find(t.id);   // balão do chat: caixa preta logo ACIMA do id, presa à cabeça
            if (bi != chatBubbles.end()) drawChatBubble(bi->second, px, y0 - 2.0f * ts, ts + 1, al);
        }
    }
    void drawNotifs() {
        if (notifs.empty() || invOpen || chestUi || shopOpen) return;   // no CRAFT as notificações aparecem (FABRICOU X1 ...)
        const float lineH = std::max(30.0f, h * 0.046f), gap = lineH * 0.18f, boxW = lineH * 12.5f;
        const int ts = std::max(2, (int)std::lround(lineH * 0.52f / 7.0f));
        const float total = notifs.size() * lineH + (notifs.size() - 1) * gap;
        float y = h * 0.50f - total * 0.5f;
        for (const Notif& n : notifs) {
            float in = std::min(1.0f, n.age / 0.18f); in = 1.0f - (1.0f - in) * (1.0f - in);
            float a = std::min(1.0f, (kNotifLife - n.age) / 0.6f) * std::min(1.0f, n.age / 0.10f);
            float x0 = -(1.0f - in) * boxW * 0.35f;
            const float solid = boxW * 0.46f;
            renderer.rect(x0, y, 4.0f, lineH, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.92f * a);
            renderer.rect(x0 + 4.0f, y, solid - 4.0f, lineH, theme::kWoodDeep.r, theme::kWoodDeep.g, theme::kWoodDeep.b, 0.82f * a);
            const int strips = 16;   // ponta esvanecendo (degradê em faixas)
            for (int k = 0; k < strips; ++k) {
                float t = (k + 0.5f) / strips;
                renderer.rect(x0 + solid + (boxW - solid) * k / strips, y, (boxW - solid) / strips + 0.5f, lineH, theme::kWoodDeep.r, theme::kWoodDeep.g, theme::kWoodDeep.b, 0.82f * a * (1.0f - t) * (1.0f - 0.35f * t));
            }
            float tx = x0 + lineH * 0.40f;
            if (n.item) {
                const ItemDef& d = itemDef(n.item);
                float q = lineH * 0.40f;
                renderer.rect(tx, y + (lineH - q) * 0.5f, q, q, d.r, d.g, d.b, a);
                tx += q + lineH * 0.30f;
            }
            renderer.text(tx, y + (lineH - 7.0f * ts) * 0.5f, ts, n.text, theme::kCream.r, theme::kCream.g, theme::kCream.b, a);
            y += lineH + gap;
        }
    }

    // ---- ROTA marcada: posição do selo no minimapa e do label no mundo 3D ----
    void wpMiniPos(float cyaw, float& sx, float& sy) const {
        const float R = miniR(), cx = miniCX(), cy = miniCY();
        const e0net::PlayerState mv = net.visual();
        float dx = (wpX - mv.x) / kMiniHalf, dz = (wpZ - mv.z) / kMiniHalf;
        float a = dx * std::sin(cyaw) - dz * std::cos(cyaw), b = dx * std::cos(cyaw) + dz * std::sin(cyaw);   // a = à frente, b = à direita
        float den = 0.5f / (0.5f + 0.5f * a);
        float px = cx + b * den / 0.6f * R, py = cy - a / (0.5f + 0.5f * a) * R;
        if (a > -0.9f && std::hypot(px - cx, py - cy) < R * 0.78f) { sx = px; sy = py; return; }   // dentro do alcance: no lugar certo do mapa
        float rel = e0net::wrapPi(std::atan2(wpX - mv.x, -(wpZ - mv.z)) - cyaw);                  // fora: na borda, na direção dela (como o N)
        sx = cx + std::sin(rel) * R * 0.84f; sy = cy - std::cos(rel) * R * 0.84f;
    }
    bool wpLabelPos(float& px, float& py, float& dist) const {
        const e0net::PlayerState me = net.visual();
        dist = std::hypot(wpX - me.x, wpZ - me.z);
        float gy = std::max(0.0f, e0net::supportHeightAt(wpX, wpZ)) + 7.0f;
        const float mg = 40.0f * (float)uiScale() * 0.5f;
        bool ok = project({wpX, gy, wpZ}, px, py);
        if (ok && px > mg && px < w - mg && py > mg && py < h - mg) return true;
        if (ok) { px = std::max(mg, std::min(w - mg, px)); py = std::max(mg, std::min(h - mg, py)); return true; }
        float rel = e0net::wrapPi(std::atan2(wpX - me.x, -(wpZ - me.z)) - camera.yaw);   // atrás da câmera: encosta na lateral
        px = rel > 0 ? w - mg : mg; py = h * 0.4f; return true;
    }
    static void fmtDist(char* o, size_t n, float d) { if (d >= 1000.0f) std::snprintf(o, n, "%.1f KM", d / 1000.0f); else std::snprintf(o, n, "%d M", (int)std::lround(d)); }

    void drawGunUi() {
        renderer.beginUi();
        const e0net::ShipState& S = curShip(); const float rangeMax = curDef().rangeMax;
        const float usc0 = (float)uiScale();
        float px, py;
        for (int i = 0; i < 90; ++i) {   // círculo de alcance máximo em volta do navio
            float a = (float)i / 90.0f * 6.2831853f;
            if (project({S.x + std::sin(a) * rangeMax, 0.1f, S.z - std::cos(a) * rangeMax}, px, py)) renderer.uiShape(Renderer::UiFill, px, py, 2.2f * usc0 * 0.5f, 1.0f, 0.85f, 0.35f, 0.55f);
        }
        if (gunHasTarget && project({gunTx, 0.1f, gunTz}, px, py)) {
            float qx, qy, rad = 30.0f;
            if (project({gunTx + e0net::kCannonRadius, 0.1f, gunTz}, qx, qy)) rad = std::max(10.0f, std::hypot(qx - px, qy - py));
            renderer.uiShape(Renderer::UiRing, px, py, rad, 1.0f, 0.25f, 0.15f, 0.9f);
            renderer.uiShape(Renderer::UiFill, px, py, rad, 1.0f, 0.25f, 0.15f, 0.12f);
            renderer.uiShape(Renderer::UiFill, px, py, 3.5f * usc0 * 0.5f, 1.0f, 0.9f, 0.5f, 1.0f);
        }
        renderer.endUi();
        renderer.beginText();
        const int usc = uiScale();
        char title[64]; { int n = (net.predicted().status & e0net::StGunner) ? 1 : 0; for (const auto& r : remotes) if (r.status & e0net::StGunner) ++n;
            std::snprintf(title, sizeof title, "CANHAO %d/%d: TOQUE ONDE A BALA VAI CAIR", net.connected() ? n : 1, e0net::kMaxGunners); }
        float tw = Renderer::textWidth(title, usc);
        renderer.rect(w * 0.5f - tw * 0.5f - 12.0f, 18.0f, tw + 24.0f, 7.0f * usc + 16.0f, 0, 0, 0, 0.55f);
        renderer.text(w * 0.5f - tw * 0.5f, 26.0f, usc, title, 1.0f, 0.9f, 0.55f, 1.0f);
        {   // barra de recarga
            float bw = 360.0f * usc * 0.5f, bh = 9.0f * usc, bx = w * 0.5f - bw * 0.5f, by = h - bh - 26.0f;
            float f = 1.0f - gunReload / e0net::kCannonReload;
            renderer.rect(bx - 3, by - 3, bw + 6, bh + 6, 0, 0, 0, 0.6f);
            renderer.rect(bx, by, bw * f, bh, f >= 1.0f ? 0.4f : 0.95f, f >= 1.0f ? 0.9f : 0.55f, 0.25f, 0.95f);
            const char* t = f >= 1.0f ? "PRONTO" : "RECARREGANDO";
            renderer.text(w * 0.5f - Renderer::textWidth(t, usc) * 0.5f, by + (bh - 7.0f * usc) * 0.5f, usc, t, 1, 1, 1, 1);
        }
        if (gunMsgT > 0.0f) {
            float mw = Renderer::textWidth(gunMsg, usc);
            renderer.text(w * 0.5f - mw * 0.5f, h * 0.22f, usc, gunMsg, 1.0f, 0.6f, 0.4f, std::min(1.0f, gunMsgT * 2.0f));
        }
        Rect2D r = gunExitRect();
        renderer.rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 0.6f, 0.5f, 0.2f, 0.85f);
        renderer.rect(r.x, r.y, r.w, r.h, 0.18f, 0.13f, 0.08f, 0.9f);
        const char* bt = "SAIR";
        renderer.text(r.x + (r.w - Renderer::textWidth(bt, usc)) * 0.5f, r.y + (r.h - 7.0f * usc) * 0.5f, usc, bt, 1.0f, 0.85f, 0.35f, 1.0f);
        renderer.endText();
    }

    // RODA DE AÇÕES ABERTA: medalhões em carvalho + aro duplo latão + medalhão X rústico no centro
    void drawWheelOnly() {
        renderer.beginUi();
        renderer.uiShape(Renderer::UiFill, (float)w * 0.5f, (float)h * 0.5f, (float)w, 0.03f, 0.02f, 0.015f, 0.48f);
        static const int kWIcon[kWheelN] = {HiBuild, HiBag, HiCraft, HiShop, HiWheel};
        const float cx0 = wheelCX(), cy0 = wheelCY(), ir = wheelItemR(), u = (float)uiScale();
        for (int k = 0; k < kWheelN; ++k) {   // pontinhos de latão ligando o centro a cada medalhão
            float ix, iy; wheelItemPos(k, ix, iy);
            for (int d = 1; d <= 4; ++d) {
                const float dist = std::max(1.0f, std::hypot(ix - cx0, iy - cy0));
                const float t = wheelCloseR() / dist * 1.25f + (1.0f - wheelCloseR() / dist * 1.25f - ir / dist * 1.2f) * (float)d / 5.0f;
                renderer.uiShape(Renderer::UiFill, cx0 + (ix - cx0) * t, cy0 + (iy - cy0) * t, 2.4f * u, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.72f);
            }
        }
        for (int k = 0; k < kWheelN; ++k) {
            float ix, iy; wheelItemPos(k, ix, iy);
            renderer.uiShape(Renderer::UiFill, ix, iy, ir + 5.0f * u, 0.0f, 0.0f, 0.0f, 0.48f);
            renderer.uiShape(Renderer::UiFill, ix, iy, ir, 0.13f, 0.095f, 0.068f, 0.96f);
            renderer.uiGlow(ix, iy - ir * 0.18f, ir * 0.85f, ir * 0.65f, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.16f);
            renderer.uiShape(Renderer::UiRing, ix, iy, ir, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.96f);
            renderer.uiShape(Renderer::UiRing, ix, iy, ir * 0.88f, theme::kBrassDim.r, theme::kBrassDim.g, theme::kBrassDim.b, 0.60f);
            renderer.uiIconTint(kWIcon[k], ix, iy, ir * 0.50f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        }
        renderer.uiCloseBtn(cx0, cy0, wheelCloseR(), 1.0f);
        renderer.endUi();
        renderer.beginText();
        const int sc = std::max(2, h / 360) + 1;
        static const char* kWLab[kWheelN] = {"CONSTRUIR", "INVENT\xC1RIO", "CRAFT", "LOJA", "NAVEGA\xC7\xC3" "O"};
        for (int k = 0; k < kWheelN; ++k) {
            float ix, iy; wheelItemPos(k, ix, iy);
            const int ls = std::max(2, sc - 1);
            const float tx = ix - Renderer::textWidth(kWLab[k], ls) * 0.5f, ty = iy + wheelItemR() + 10.0f * (float)uiScale();
            renderer.text(tx, ty, ls, kWLab[k], theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f);
        }
        renderer.endText();
    }
    void drawUi() {
        if (introActive) { drawIntroOverlay(); return; }
        if (gunnerMode && gunBlend > 0.5f) { drawGunUi(); return; }
        if (wheelOpen) { drawWheelOnly(); return; }
        if (flagOpen) { drawFlagScreen(); return; }   // editor de BANDEIRA em tela cheia (sem HUD)
        if (teamOpen) { drawTeamScreen(); return; }   // painel da TRIPULAÇÃO em tela cheia
        if (navOpen) { drawNavScreen(); return; }     // NAVEGAÇÃO em tela cheia (sem HUD)
        if (isleMode) { drawIsleScreen(); return; }   // MELHORIAS DA ILHA (vista de cima, sem a HUD do jogo)
        renderer.beginUi();
        drawLowGlow();
        // Joystick: base flutuante onde o dedo tocou (oculto quando uma tela cheia está aberta!)
        float R = joyR();
        float bx = joyT.id >= 0 ? joyT.sx : joyHomeX(), by = joyT.id >= 0 ? joyT.sy : joyHomeY();
        float tx = bx, ty = by;
        if (joyT.id >= 0) {
            float dx = joyT.x - joyT.sx, dy = joyT.y - joyT.sy;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > R) { dx *= R / len; dy *= R / len; }
            tx += dx; ty += dy;
        }
        if (!uiFull()) {
            renderer.uiShape(Renderer::UiFill, bx, by, R, 0.08f, 0.06f, 0.04f, 0.24f);
            renderer.uiShape(Renderer::UiRing, bx, by, R, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.55f);
            renderer.uiShape(Renderer::UiRing, bx, by, R * 0.90f, theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.16f);
            renderer.uiShape(Renderer::UiFill, tx, ty, R * 0.40f, theme::kCream.r, theme::kCream.g, theme::kCream.b, joyT.id >= 0 ? 0.52f : 0.28f);
            renderer.uiShape(Renderer::UiRing, tx, ty, R * 0.40f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, joyT.id >= 0 ? 0.82f : 0.48f);
        }

        HudMark mk[3]; int nm = (isleMode || uiFull()) ? 0 : marks(mk);
        float mpx[3] = {0, 0, 0}, mpy[3] = {0, 0, 0}; bool mok[3] = {false, false, false};
        float wbx[WbCount] = {}, wby[WbCount] = {}; bool wbok[WbCount] = {};   // botões do cenário projetados (LEME / CANHAO / ABRIR)
        if (!isleMode && !uiFull() && !gunnerMode) for (int k = 0; k < WbCount; ++k) {
            if (wbtn[k].alpha <= 0.02f || !project(wbtn[k].top, wbx[k], wby[k])) continue;
            const float m = worldBtnR();   // fora da tela: some
            wbok[k] = wbx[k] > -m && wbx[k] < w + m && wby[k] > -m && wby[k] < h + m;
        }
        const e0net::PlayerState visMe = net.visual();
        const bool swimUi = e0net::isSwimmingPos(visMe.x, visMe.y, visMe.z);

        if (!isleMode && !uiFull()) {
            if (net.worldReady()) {   // minimapa redondo no canto superior esquerdo (abaixo dos botões)
                const float R = miniR(), cx = miniCX(), cy = miniCY();
                const e0net::PlayerState mv = net.visual();
                renderer.uiShape(Renderer::UiFill, cx, cy, R * 1.05f, 0.06f, 0.04f, 0.02f, 0.55f);
                const float cyaw = rig.yaw();
                renderer.uiMapTilt(0, cx, cy, R, 1.0f, 0.5f, 0.5f, 0.25f, cyaw, std::fmod(std::fabs(mv.x), 50.0f) / 50.0f * (mv.x < 0 ? -1.0f : 1.0f) + (mv.x < 0 ? 1.0f : 0.0f), std::fmod(std::fabs(mv.z), 50.0f) / 50.0f * (mv.z < 0 ? -1.0f : 1.0f) + (mv.z < 0 ? 1.0f : 0.0f), true);   // papel
                if (miniInk.valid) {   // tinta: centro do recorte segue você; rotação/inclinação no shader
                    float th = miniInk.half;
                    float uc = (mv.x - (miniInk.cx - th)) / (2 * th), vc = (mv.z - (miniInk.cz - th)) / (2 * th);
                    renderer.uiMapTilt(1, cx, cy, R, 1.0f, uc, vc, kMiniHalf / (2 * th), cyaw);
                    // chão (dx,dz) -> tela, mesma perspectiva do shader (inverso)
                    auto proj = [&](float wx, float wz, float& sx, float& sy, float& sc) {
                        float dx = (wx - mv.x) / kMiniHalf, dz = (wz - mv.z) / kMiniHalf;
                        float a = dx * std::sin(cyaw) - dz * std::cos(cyaw), b = dx * std::cos(cyaw) + dz * std::sin(cyaw);   // a = à frente, b = à direita
                        float den = 0.5f / (0.5f + 0.5f * a);   // inverso do shader (sy=0.5, sx=0.6, k=0.5)
                        sx = cx + b * den / 0.6f * R; sy = cy - a / (0.5f + 0.5f * a) * R; sc = den;
                        if (std::hypot(sx - cx, sy - cy) > R * 0.9f) sc = -1.0f;   // fora do disco
                    };
                    float sx, sy, sc;
                    for (int si = 0; si < e0net::kMaxShips; ++si) {
                        const e0net::ShipState& SH = e0net::shipState(si);
                        if (SH.exists && std::hypot(SH.x - mv.x, SH.z - mv.z) < kMiniView * 0.96f) { proj(SH.x, SH.z, sx, sy, sc); if (sc > 0) renderer.uiMarker(1, sx, sy, R * (si == e0net::ShipGalleon ? 0.115f : (si == e0net::ShipBoat ? 0.06f : (si == e0net::ShipBrig ? 0.095f : 0.085f))) * sc, SH.yaw - cyaw); }
                    }
                    for (const auto& p : remotes) if (!crewMember(p.id) && std::hypot(p.x - mv.x, p.z - mv.z) < kMiniView * 0.96f) { proj(p.x, p.z, sx, sy, sc); if (sc > 0) renderer.uiMarker(2, sx, sy, R * 0.06f * sc, 0.0f); }
                    if (net.teamPosAge() < 4.0) for (const auto& p : net.teamPositions()) {   // TRIPULANTES (verde), qualquer distância: longe = na borda
                        float px = p.x, pz = p.z;
                        for (const auto& q : remotes) if (q.id == p.id) { px = q.x; pz = q.z; break; }
                        const float d = std::hypot(px - mv.x, pz - mv.z), lim = kMiniView * 0.90f;
                        if (d > lim) { px = mv.x + (px - mv.x) * lim / d; pz = mv.z + (pz - mv.z) * lim / d; }
                        proj(px, pz, sx, sy, sc); if (sc > 0) renderer.uiMarker(3, sx, sy, R * (d > lim ? 0.055f : 0.075f) * sc, 0.0f);
                    }
                    for (int sl = 1; sl < e0net::kIslandSlots; ++sl) {   // DOMINAÇÃO: ilhas dominadas mostram a bandeira da dona
                        if (!net.isleOwner(sl).team) continue;
                        float fx, fy, fz; if (!islePoint(sl, fx, fy, fz) || std::hypot(fx - mv.x, fz - mv.z) > kMiniView * 0.92f) continue;
                        proj(fx, fz, sx, sy, sc); if (sc > 0) drawIsleMapFlag(sl, sx, sy, R * 0.10f * std::min(1.4f, sc), 1.0f);
                    }
                    renderer.uiMarker(0, cx, cy, R * 0.095f, mv.yaw - cyaw);
                }
                renderer.uiShape(Renderer::UiRing, cx, cy, R * 1.015f, 0.14f, 0.09f, 0.04f, 0.95f);
                renderer.uiShape(Renderer::UiRing, cx, cy, R * 1.075f, 0.55f, 0.40f, 0.20f, 0.85f);
                const float nrx = cx + std::sin(-cyaw) * R * 0.84f, nry = cy - std::cos(-cyaw) * R * 0.84f;
                renderer.uiShape(Renderer::UiFill, nrx, nry, R * 0.115f, 0.86f, 0.75f, 0.52f, 0.95f);   // selo do norte (gira com a câmera)
                renderer.uiShape(Renderer::UiRing, nrx, nry, R * 0.115f, 0.14f, 0.09f, 0.04f, 0.95f);
                if (wpSet) {   // selo da ROTA (dourado), como o N
                    float wx, wy; wpMiniPos(cyaw, wx, wy);
                    renderer.uiShape(Renderer::UiFill, wx, wy, R * 0.115f, 1.0f, 0.78f, 0.22f, 0.95f);
                    renderer.uiShape(Renderer::UiRing, wx, wy, R * 0.115f, 0.14f, 0.09f, 0.04f, 0.95f);
                }
            }
            drawStatWidgets();
            // 4 botões de atalho abaixo do minimapa (Roda, Chat, Bandeira, Tripulação) em carvalho + latão
            for (int bk = 0; bk < 4; ++bk) {
                const Rect2D mb = menuBtnRect(bk);
                renderer.uiButton(mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f, mb.w * 0.5f, mb.h * 0.5f, 8.0f * (float)uiScale() * 0.4f, theme::BtnWood, 0.92f);
            }
            // Botão de correr (medalhão rústico translúcido: carvalho escuro + anel latão)
            float rr = runR();
            bool runOn = runPtr >= 0;
            if (runOn) renderer.uiShape(Renderer::UiFill, runX(), runY(), rr, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.52f);
            else       renderer.uiShape(Renderer::UiFill, runX(), runY(), rr, 0.08f, 0.06f, 0.04f, 0.32f);
            renderer.uiShape(Renderer::UiRing, runX(), runY(), rr, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, runOn ? 0.95f : 0.62f);
            if (shipCtx == 3) {
                bool on = shipThrPtr >= 0;
                renderer.uiShape(Renderer::UiFill, runX(), runY(), rr, 0.18f, 0.48f, 0.28f, on ? 0.68f : 0.34f);
                renderer.uiShape(Renderer::UiRing, runX(), runY(), rr, 0.48f, 0.85f, 0.55f, 0.88f);
            } else if (!renderer.hasIcons()) renderer.uiShape(Renderer::UiRunIcon, runX(), runY(), rr, theme::kCream.r, theme::kCream.g, theme::kCream.b, runOn ? 1.0f : 0.85f);
            else renderer.uiIconTint(HiRun, runX(), runY(), rr * 0.58f, theme::kCream.r, theme::kCream.g, theme::kCream.b, runOn ? 1.0f : 0.88f);
            // Botão de câmera (acima do de correr)
            float cr = camR();
            renderer.uiShape(Renderer::UiFill, camX(), camY(), cr, 0.08f, 0.06f, 0.04f, 0.32f);
            renderer.uiShape(Renderer::UiRing, camX(), camY(), cr, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.58f);
            // Indicadores (coletável e alvo de ação)
            for (int i = 0; i < nm; ++i) {
                mok[i] = project({mk[i].pos.x, std::max(visMe.y + 1.45f, mk[i].pos.y + 0.6f), mk[i].pos.z}, mpx[i], mpy[i]);
                if (mok[i] && mk[i].line) {
                    renderer.uiShape(Renderer::UiFill, mpx[i], mpy[i], 5.0f * (h / 1080.0f + 0.5f), theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.95f * mk[i].alpha);
                    renderer.uiShape(Renderer::UiRing, mpx[i], mpy[i], 11.0f * (h / 1080.0f + 0.5f), theme::kCream.r, theme::kCream.g, theme::kCream.b, 0.65f * mk[i].alpha);
                }
            }
            if (!swimUi) {
                renderer.uiShape(Renderer::UiFill, actionX(), actionY(), actionR(), 0.08f, 0.06f, 0.04f, 0.32f);
                renderer.uiShape(Renderer::UiRing, actionX(), actionY(), actionR(), theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.62f);
            }
            if (armaMode()) {   // BACAMARTE: ATIRAR (vermelho), progresso da recarga, MIRAR e a mira no centro da tela
                const float ar = actionR();
                renderer.uiShape(Renderer::UiFill, actionX(), actionY(), ar, 0.85f, 0.18f, 0.12f, actionPtr >= 0 ? 0.55f : (armaLoaded ? 0.30f : 0.12f));
                renderer.uiShape(Renderer::UiRing, actionX(), actionY(), ar, 1.0f, 0.45f, 0.35f, 0.85f);
                if (armaReloadT >= 0.0f) renderer.uiArc(actionX(), actionY(), ar * 1.12f, 5.0f * (float)uiScale(), armaReloadT / e0net::kGunReload, 1.0f, 0.82f, 0.3f, 0.95f);
                if (!rescueBtnVisible()) {
                    const bool on = armaAim;
                    renderer.uiShape(Renderer::UiFill, aimBtnX(), aimBtnY(), ar, on ? 0.95f : 1.0f, on ? 0.75f : 1.0f, on ? 0.25f : 1.0f, on ? 0.45f : 0.12f);
                    renderer.uiShape(Renderer::UiRing, aimBtnX(), aimBtnY(), ar, 1, on ? 0.85f : 1.0f, on ? 0.4f : 1.0f, on ? 0.95f : 0.45f);
                }
                // mira: o círculo mostra a abertura do cone dos chumbos (fechado mirando, aberto do quadril)
                const float aimK = rig.aimAmount();
                const float spread = e0net::kGunSpreadHip + (e0net::kGunSpreadAim - e0net::kGunSpreadHip) * aimK;
                const float bloom = armaShotT < 0.4f ? (1.0f - armaShotT / 0.4f) * 0.6f : 0.0f;
                const float rpx = std::max(6.0f, std::tan(spread * (1.0f + bloom)) / std::tan(camera.fovY * 0.5f) * (float)h * 0.5f);
                const float cx = w * 0.5f, cy = h * 0.5f, ca = 0.55f + 0.35f * aimK;
                if (aimK > 0.02f) {   // a mira só aparece quando está mirando; vermelha sobre um jogador
                    const bool tgt = armaOverPlayer();
                    const float gg = tgt ? 0.12f : 1.0f;
                    renderer.uiShape(Renderer::UiRing, cx, cy, rpx, 1, gg, gg, ca * 0.75f * aimK);
                    renderer.uiShape(Renderer::UiFill, cx, cy, 2.4f * (float)uiScale(), 1, gg, gg, ca * aimK);
                }
                if (armaHitT > 0.0f) {   // marcador de acerto: 4 pontos na diagonal (vermelho = cabeça)
                    const float k = armaHitT / 0.35f, d = rpx + 10.0f * (float)uiScale() * (1.4f - 0.4f * k);
                    for (int q = 0; q < 4; ++q) {
                        const float sx = (q & 1) ? 1.0f : -1.0f, sy = (q & 2) ? 1.0f : -1.0f;
                        renderer.uiShape(Renderer::UiFill, cx + sx * d * 0.7071f, cy + sy * d * 0.7071f, 4.0f * (float)uiScale(), 1.0f, armaHitHead ? 0.15f : 1.0f, armaHitHead ? 0.1f : 1.0f, k);
                    }
                }
            }
            if (shipCtx == 3) {   // RE
                bool on = shipRevPtr >= 0;
                renderer.uiShape(Renderer::UiFill, actionX(), actionY(), actionR(), 0.95f, 0.45f, 0.25f, on ? 0.60f : 0.22f);
                renderer.uiShape(Renderer::UiRing, actionX(), actionY(), actionR(), 1.0f, 0.6f, 0.35f, 0.85f);
            }
            if (shipCtx != 0) {   // EMBARCAR / DESCER / SOLTAR LEME (+ LEME)
                int ks[4]; int nb = shipBtnList(ks);
                for (int ki = 0; ki < nb; ++ki) {
                    const int k = ks[ki];
                    renderer.uiShape(Renderer::UiFill, shipBtnX(k), shipBtnY(k), actionR(), 0.35f, 0.75f, 1.0f, 0.18f);
                    renderer.uiShape(Renderer::UiRing, shipBtnX(k), shipBtnY(k), actionR(), 0.45f, 0.82f, 1.0f, 0.75f);
                }
            }
            for (int k = 0; k < WbCount; ++k) {   // botões do cenário: disco escuro translúcido + anel azul (cara de botão), tamanho constante
                if (!wbok[k]) continue;
                const float a = wbtn[k].alpha, rb = worldBtnR();
                renderer.uiShape(Renderer::UiFill, wbx[k], wby[k], rb * 1.18f, 0.35f, 0.75f, 1.0f, 0.10f * a);
                renderer.uiShape(Renderer::UiFill, wbx[k], wby[k], rb, 0.02f, 0.05f, 0.09f, 0.55f * a);
                renderer.uiShape(Renderer::UiRing, wbx[k], wby[k], rb, 0.50f, 0.85f, 1.0f, 0.90f * a);
            }
            if (collectAlpha > 0.02f) {
                renderer.uiShape(Renderer::UiFill, collectX(), collectY(), collectR(), 0.08f, 0.06f, 0.04f, 0.34f * collectAlpha);
                renderer.uiShape(Renderer::UiRing, collectX(), collectY(), collectR(), theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.72f * collectAlpha);
            }

            // Barra de Acesso Rápido no centro inferior: slots rústicos em carvalho translúcido + moldura latão
            for (int i = 0; i < kHotbarSlots; ++i) {
                bool sel = (i == hotbarCur);
                HotbarSlotGeom g = hotbarSlotGeom(i);
                renderer.uiWoodPanel(g.cx, g.cy, g.halfW, g.halfH, g.radius, sel ? theme::PanelCardSel : theme::PanelSlot, sel ? 0.90f : 0.72f, (float)i);
                const ItemStack& it = inv[kInvHotFirst + i];
                if (it.id) {
                    drawItemIcon(g.cx, g.cy - g.halfH * 0.12f, g.halfW * 0.42f, it.id, 1.0f);
                }
            }
        }
        if (invOpen) drawInvShapes(); else if (craftOpen) drawCraftShapes(); else if (chestUi) drawChestShapes(); else if (shopOpen) drawShopShapes(); else if (mapOpen) drawMapShapes();
        renderer.endUi();

        int sc = std::max(2, h / 360) + 1;
        int usc = uiScale();
        renderer.beginText();

        auto drawBtnBox = [&](const Rect2D& r, const char* label, int textSc,
                              float bgR, float bgG, float bgB, float bgA,
                              float fgR, float fgG, float fgB) {
            renderer.rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, fgR * 0.6f, fgG * 0.6f, fgB * 0.6f, bgA * 0.85f);
            renderer.rect(r.x, r.y, r.w, r.h, bgR, bgG, bgB, bgA);
            float tw = Renderer::textWidth(label, textSc);
            float th = 7.0f * textSc;
            renderer.text(r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f, textSc, label, fgR, fgG, fgB, 1.0f);
        };

        // caixa com ícone à esquerda e o texto centralizado no resto
        auto drawBtnBoxI = [&](const Rect2D& r, const char* label, int icon, int textSc,
                               float bgR, float bgG, float bgB, float bgA, float fgR, float fgG, float fgB) {
            renderer.rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, fgR * 0.6f, fgG * 0.6f, fgB * 0.6f, bgA * 0.85f);
            renderer.rect(r.x, r.y, r.w, r.h, bgR, bgG, bgB, bgA);
            float tw = Renderer::textWidth(label, textSc), th = 7.0f * textSc;
            renderer.text(r.x + r.h + (r.w - r.h - tw) * 0.5f, r.y + (r.h - th) * 0.5f, textSc, label, fgR, fgG, fgB, 1.0f);
            hudIcon(icon, r.x + r.h * 0.55f, r.y + r.h * 0.5f, r.h * 0.38f, fgR, fgG, fgB, 1.0f);
        };
        hudIcons.clear();

        if (invOpen) {
            drawInvText();
        } else if (craftOpen) {
            drawCraftText();
        } else if (chestUi) {
            drawChestText();
        } else if (shopOpen) {
            drawShopText();
        } else if (mapOpen) {
            drawMapText(drawBtnBox);
        } else if (!isleMode) {
            if (net.worldReady()) {   // N do minimapa (norte sempre para cima)
                const float na = -rig.yaw();   // norte gira com a câmera
                float nx = miniCX() + std::sin(na) * miniR() * 0.84f, ny = miniCY() - std::cos(na) * miniR() * 0.84f;
                renderer.text(nx - Renderer::textWidth("N", usc) * 0.5f, ny - 3.5f * usc, usc, "N", 0.16f, 0.09f, 0.04f, 0.95f, false);
                if (wpSet) {
                    float wx, wy; wpMiniPos(rig.yaw(), wx, wy);
                    renderer.text(wx - Renderer::textWidth("X", usc) * 0.5f, wy - 3.5f * usc, usc, "X", 0.16f, 0.09f, 0.04f, 0.95f, false);
                }
            }
            if (wpSet && !gunnerMode) {   // marca da ROTA no mundo 3D: só um X dourado com a distância embaixo (nada mais), mesmo longe
                float lx, ly, d; wpLabelPos(lx, ly, d);
                char t[32]; fmtDist(t, sizeof t, d);
                const int xs = std::max(3, usc * 2);
                renderer.text(lx - Renderer::textWidth("X", xs) * 0.5f, ly - 3.5f * (float)xs, xs, "X", 1.0f, 0.82f, 0.25f, 0.95f);
                renderer.text(lx - Renderer::textWidth(t, usc) * 0.5f, ly + 4.5f * (float)xs, usc, t, 1.0f, 1.0f, 1.0f, 0.9f);
            }
            // Botão CONSTRUIR no topo superior esquerdo
            Rect2D ob = menuBtnRect(0);
            auto iconBtn = [&](const Rect2D& r, int icon) {   // só o ícone (sem nome nem widget)
                renderer.rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 0.6f, 0.51f, 0.21f, 0.75f);
                renderer.rect(r.x, r.y, r.w, r.h, 0.18f, 0.13f, 0.08f, 0.88f);
                hudIcon(icon, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.h * 0.38f, 1.0f, 0.85f, 0.35f, 1.0f);
            };
            {   // botão da RODA: ícone da bússola (fundo carvalho+latão desenhado no passe de UI)
                const Rect2D wb = wheelBtnRect();
                hudIcon(HiCompass, wb.x + wb.w * 0.5f, wb.y + wb.h * 0.5f, wb.h * 0.46f, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
            }
            drawChatBtn();
            drawFlagBtn();
            drawTeamBtn();
            drawShipCleanTimer();
            {   // barra de FOME ao lado do INVENTARIO (verde -> laranja -> vermelho; pisca quando baixa)
                Rect2D ib = invOpenBtnRect();
                Rect2D cb = craftOpenBtnRect();
                if (craftBusy >= 0) {   // craft em andamento (tela fechada): barra + nome sob os botões
                    int cn; const Recipe* CR = recipes(cn); float cf = std::min(1.0f, craftT / std::max(0.1f, CR[craftBusy].time));
                    Rect2D cr{ob.x, ob.y + ob.h + 8.0f * (float)uiScale(), 2.0f * miniR(), 14.0f * (float)uiScale()};
                    renderer.rect(cr.x - 2.0f, cr.y - 2.0f, cr.w + 4.0f, cr.h + 4.0f, 0.3f, 0.6f, 0.35f, 0.8f);
                    renderer.rect(cr.x, cr.y, cr.w, cr.h, 0.08f, 0.12f, 0.09f, 0.9f);
                    renderer.rect(cr.x, cr.y, cr.w * cf, cr.h, 0.25f, 0.75f, 0.35f, 0.85f);
                    char ct[48]; std::snprintf(ct, sizeof ct, "FABRICANDO %s %d%%", itemDef(CR[craftBusy].out).name, (int)(cf * 100.0f));
                    renderer.text(cr.x + (cr.w - Renderer::textWidth(ct, usc)) * 0.5f, cr.y + (cr.h - 7.0f * usc) * 0.5f, usc, ct, 1, 1, 1, 1.0f);
                }
                {   // hora do jogo ao lado do relógio
                    float cxK, cyK, right, tw; clockGeom(cxK, cyK, right, tw);
                    int ch, cm; clockHM(ch, cm); char kt[8]; std::snprintf(kt, sizeof kt, "%02d:%02d", ch, cm);
                    renderer.text(cxK + statR() + 4.0f * (float)uiScale() + (tw - Renderer::textWidth(kt, usc)) * 0.5f, cyK - 3.5f * usc, usc, kt, 1.0f, 0.85f, 0.30f, 1.0f);
                }
                if (net.connected()) {   // ID do jogador (3 dígitos) entre a hora e as moedas
                    float cxI, cyI, right, tw; idGeom(cxI, cyI, right, tw);
                    char it[8]; std::snprintf(it, sizeof it, "%03d", (int)net.myId());
                    renderer.text(cxI + statR() + 4.0f * (float)uiScale() + (tw - Renderer::textWidth(it, usc)) * 0.5f, cyI - 3.5f * usc, usc, it, 1.0f, 0.85f, 0.30f, 1.0f);
                }
                {   // quantidade de MOEDAS ao lado do círculo (o resto é desenhado em drawStatWidgets)
                    float cxC, cyC, right, tw; coinGeom(cxC, cyC, right, tw);
                    char mt[16]; coinText(mt, sizeof mt, coins);
                    renderer.text(cxC + statR() + 4.0f * (float)uiScale() + (tw - Renderer::textWidth(mt, usc)) * 0.5f, cyC - 3.5f * usc, usc, mt, 1.0f, 0.85f, 0.30f, 1.0f);
                }
            }

            static const char* kLabel[4] = {"N", "P", "L", "1P"};
            const char* lab = kLabel[(int)rig.preset() & 3];
            {   // ícone da câmera em cima, preset (N/P/L/1P) embaixo
                const int cs = std::max(2, sc - 2); const float cr2 = camR();
                hudIcon(HiCamera, camX(), camY() - cr2 * 0.26f, cr2 * 0.42f, 1, 1, 1, 0.95f);
                renderer.text(camX() - Renderer::textWidth(lab, cs) * 0.5f, camY() + cr2 * 0.40f - 3.5f * cs, cs, lab, 1, 1, 1, 0.95f);
            }
            {
                int bsc = std::max(2, sc - 1);
                if (armaMode()) {   // BACAMARTE: rótulos, munição e traços da mira
                    const char* act = armaReloadT >= 0.0f ? "CARREGANDO" : (armaLoaded ? "ATIRAR" : (invCount(ItAmmo) > 0 ? "CARREGAR" : "SEM BALA"));
                    roundBtnLabel(armaLoaded ? HiShot : HiReload, actionX(), actionY(), actionR(), act, bsc, 1, 0.92f, 0.85f, 0.98f);
                    if (!rescueBtnVisible()) roundBtnLabel(HiAim, aimBtnX(), aimBtnY(), actionR(), armaAim ? "MIRANDO" : "MIRAR", bsc, 1, 1, 1, 0.95f);
                    {   // contagem de balas ACIMA do acesso rápido; a quantidade no mesmo estilo da dos slots (X20, amarelo claro)
                        const HotbarSlotGeom g0 = hotbarSlotGeom(0);
                        const int ns = std::max(1, usc - 1);
                        char t1[24], t2[16];
                        std::snprintf(t1, sizeof t1, "CARGA %d/1   MUNICAO ", armaLoaded ? 1 : 0); std::snprintf(t2, sizeof t2, "X%d", invCount(ItAmmo));
                        const float w1 = Renderer::textWidth(t1, ns), w2 = Renderer::textWidth(t2, ns);
                        const float ax = w * 0.5f - (w1 + w2) * 0.5f, ay = g0.cy - g0.halfH - 14.0f * ns - 10.0f;
                        renderer.text(ax + 1.0f, ay + 1.0f, ns, t1, 0, 0, 0, 0.6f); renderer.text(ax + w1 + 1.0f, ay + 1.0f, ns, t2, 0, 0, 0, 0.6f);
                        renderer.text(ax, ay, ns, t1, 1.0f, 0.92f, 0.7f, 1.0f);
                        renderer.text(ax + w1, ay, ns, t2, 1, 1, 0.6f, 1.0f);
                    }
                    const float aimK = rig.aimAmount();
                    const float spread = e0net::kGunSpreadHip + (e0net::kGunSpreadAim - e0net::kGunSpreadHip) * aimK;
                    const float bloom = armaShotT < 0.4f ? (1.0f - armaShotT / 0.4f) * 0.6f : 0.0f;
                    const float rpx = std::max(6.0f, std::tan(spread * (1.0f + bloom)) / std::tan(camera.fovY * 0.5f) * (float)h * 0.5f);
                    const float cx = w * 0.5f, cy = h * 0.5f, L = 9.0f * (float)uiScale(), T = std::max(2.0f, 1.6f * (float)uiScale()), ca = 0.6f + 0.35f * aimK;
                    if (aimK > 0.02f) {
                        const float cb = ca * aimK, gg = armaOverPlayer() ? 0.12f : 1.0f;
                        renderer.rect(cx - rpx - L - 3, cy - T * 0.5f, L, T, 1, gg, gg, cb);
                        renderer.rect(cx + rpx + 3, cy - T * 0.5f, L, T, 1, gg, gg, cb);
                        renderer.rect(cx - T * 0.5f, cy - rpx - L - 3, T, L, 1, gg, gg, cb);
                        renderer.rect(cx - T * 0.5f, cy + rpx + 3, T, L, 1, gg, gg, cb);
                    }
                } else if (!swimUi) {
                    const char* act = shipCtx == 3 ? "RE" : actionLabel();   // BATER / CORTAR / MINERAR (no leme: RE)
                    const int aic = shipCtx == 3 ? HiBack : (actCur < 0 ? HiFist : (actType == e0net::ObjRock ? HiPick : HiAxe));
                    roundBtnLabel(aic, actionX(), actionY(), actionR(), act, bsc, 1, 1, 1, 0.95f);
                }
                if (shipCtx == 3) {
                    const char* t = shipBtnLabel(2);
                    roundBtnLabel(HiSpeed, runX(), runY(), runR(), t, bsc, 1, 1, 1, 0.98f);
                }
                if (shipCtx != 0) {
                    int ks[4]; int nb = shipBtnList(ks);
                    for (int ki = 0; ki < nb; ++ki) {
                        const int k = ks[ki];
                        const char* t = shipBtnLabel(k);
                        const int sic = shipCtx == 1 ? HiBoat : HiExit;   // só o botão 0 (EMBARCAR / DESCER / SOLTAR) fica na HUD
                        roundBtnLabel(sic, shipBtnX(k), shipBtnY(k), actionR(), t, bsc, 0.75f, 0.92f, 1.0f, 0.98f);
                    }
                    if (shipCtx >= 2) {   // velocímetro e dica do leme
                        const e0net::ShipState& SS = curShip();
                        char vt[64]; std::snprintf(vt, sizeof vt, "%s  %.1f NOS  %s", curDef().name, std::fabs(SS.speed) * 1.944f, SS.speed < -0.2f ? "(RE)" : "");
                        float tw2 = Renderer::textWidth(vt, usc);
                        renderer.rect(w * 0.5f - tw2 * 0.5f - 8.0f * usc, h * 0.125f - 3.0f * usc, tw2 + 16.0f * usc, 13.0f * usc, 0, 0, 0, 0.45f);
                        renderer.text(w * 0.5f - tw2 * 0.5f, h * 0.125f, usc, vt, 0.8f, 0.93f, 1.0f, 1.0f);
                        {   // ilha mais perto do navio: nome, distância e rumo relativo à proa (bússola do oceano aberto)
                            float bd = 1e30f; e0net::IslandSpec bs;
                            for (int sl = 0; sl < e0net::kIslandSlots; ++sl) {
                                int sx, sz; e0net::sectorOfSlot(sl, sx, sz);
                                e0net::IslandSpec sp = e0net::islandSpec(e0net::terrain().mainSeed(), sx, sz);
                                if (!sp.exists) continue;
                                float dx = sp.oxcm * 0.01f - SS.x, dz = sp.ozcm * 0.01f - SS.z, d = dx * dx + dz * dz;
                                if (d < bd) { bd = d; bs = sp; }
                            }
                            if (bd < 1e29f) {
                                float dx = bs.oxcm * 0.01f - SS.x, dz = bs.ozcm * 0.01f - SS.z;
                                float rel = e0net::wrapPi(std::atan2(dx, -dz) - SS.yaw);   // 0 = proa, + = estibordo
                                static const char* kDir[8] = {"A PROA", "PROA-ESTIBORDO", "ESTIBORDO", "POPA-ESTIBORDO", "POPA", "POPA-BOMBORDO", "BOMBORDO", "PROA-BOMBORDO"};
                                int di = (int)std::floor((rel + 0.3927f) / 0.7854f); di = ((di % 8) + 8) % 8;
                                char it[96]; std::snprintf(it, sizeof it, "%s  %.0f M  %s", e0net::islandKindName(bs.kind), std::sqrt(bd), kDir[di]);
                                float tw3 = Renderer::textWidth(it, usc);
                                renderer.rect(w * 0.5f - tw3 * 0.5f - 8.0f * usc, h * 0.125f + 28.0f * usc - 3.0f * usc, tw3 + 16.0f * usc, 13.0f * usc, 0, 0, 0, 0.45f);
                                renderer.text(w * 0.5f - tw3 * 0.5f, h * 0.125f + 28.0f * usc, usc, it, 1.0f, 0.92f, 0.6f, 1.0f);
                            }
                        }
                        if (shipCtx == 3) {
                            const char* hint = "JOYSTICK: BOMBORDO / ESTIBORDO";
                            renderer.text(w * 0.5f - Renderer::textWidth(hint, usc) * 0.5f, h * 0.125f + 14.0f * usc, usc, hint, 0.75f, 0.9f, 1.0f, 0.85f);
                        }
                    }
                }
                for (int k = 0; k < WbCount; ++k)   // ícone + rótulo dos botões do cenário
                    if (wbok[k]) roundBtnLabel(worldBtnIcon(k), wbx[k], wby[k], worldBtnR(), worldBtnLabel(k), std::max(2, bsc - 1), 0.80f, 0.94f, 1.0f, 0.98f * wbtn[k].alpha);
                if (collectAlpha > 0.02f) {
                    const char* btn = "COLETAR";
                    roundBtnLabel(HiGrab, collectX(), collectY(), collectR(), btn, bsc, 1, 1, 1, 0.95f * collectAlpha);
                }
                for (int i = 0; i < nm; ++i) {
                    if (!mok[i]) continue;
                    float tw = Renderer::textWidth(mk[i].name, sc), ly = mk[i].line ? mpy[i] - 11.0f * sc - 12.0f * (h / 1080.0f + 0.5f) : mpy[i] - 3.5f * sc;
                    renderer.rect(mpx[i] - tw * 0.5f - 3.0f * sc, ly - 2.0f * sc, tw + 6.0f * sc, 11.0f * sc, 0, 0, 0, 0.45f * mk[i].alpha);
                    renderer.text(mpx[i] - tw * 0.5f, ly, sc, mk[i].name, 1, 1, 1, mk[i].alpha);
                }

                {   // efeitos de poção ativos, acima do acesso rápido
                    const HotbarSlotGeom g0 = hotbarSlotGeom(0);
                    const int ns = std::max(1, usc - 1);
                    float ey = g0.cy - g0.halfH - 14.0f * ns - 10.0f - (armaMode() ? 18.0f * ns : 0.0f);
                    char eb[40];
                    if (potSpeedT > 0.0f) { std::snprintf(eb, sizeof eb, "VELOCIDADE %d S", (int)std::ceil(potSpeedT)); renderer.text(g0.cx - g0.halfW + 1, ey + 1, ns, eb, 0, 0, 0, 0.6f); renderer.text(g0.cx - g0.halfW, ey, ns, eb, 1.0f, 0.95f, 0.3f, 1.0f); ey -= 14.0f * ns; }
                    if (potShieldT > 0.0f) { std::snprintf(eb, sizeof eb, "ESCUDO %d S", (int)std::ceil(potShieldT)); renderer.text(g0.cx - g0.halfW + 1, ey + 1, ns, eb, 0, 0, 0, 0.6f); renderer.text(g0.cx - g0.halfW, ey, ns, eb, 0.5f, 0.75f, 1.0f, 1.0f); }
                }
                // Numeração dos slots da barra de acesso rápido (1..5)
                for (int i = 0; i < kHotbarSlots; ++i) {
                    bool sel = (i == hotbarCur);
                    HotbarSlotGeom g = hotbarSlotGeom(i);
                    char numStr[4];
                    std::snprintf(numStr, sizeof numStr, "%d", i + 1);
                    int numSc = std::max(2, usc);
                    float tw = Renderer::textWidth(numStr, numSc);
                    float tx = g.cx - tw * 0.5f;
                    float ty = g.cy - g.halfH + 10.0f;
                    renderer.text(tx, ty, numSc, numStr, 1, 1, 1, sel ? 1.0f : 0.75f);
                    const ItemStack& it = inv[kInvHotFirst + i];
                    if (it.id) {
                        const char* nm2 = itemDef(it.id).name;
                        int ns = std::max(1, usc - 1);
                        if (Renderer::textWidth(nm2, ns) > g.halfW * 2.0f - 1.0f) ns = std::max(1, ns - 1);
                        renderer.text(g.cx - Renderer::textWidth(nm2, ns) * 0.5f, g.cy + g.halfH - 11.0f * ns - 6.0f, ns, nm2, 1, 1, 1, 0.92f);
                        if (it.n > 1) {
                            char cb[8]; std::snprintf(cb, sizeof cb, "X%d", it.n);
                            renderer.text(g.cx + g.halfW - Renderer::textWidth(cb, ns) - 8.0f, g.cy - g.halfH + 10.0f, ns, cb, 1, 1, 0.6f, 1.0f);
                        }
                    }
                }
            }
        }

        drawNotifs();
        drawPush();
        if (!net.worldReady()) {  // enquanto o mundo baixa / é gerado
            char msg[64]; std::snprintf(msg, sizeof msg, "CARREGANDO MUNDO... (%s)", net.worldStatus());
            int ls = std::max(3, h / 270);
            renderer.rect(w * 0.5f - Renderer::textWidth(msg, ls) * 0.5f - 12.0f * ls, h * 0.40f - 8.0f * ls, Renderer::textWidth(msg, ls) + 24.0f * ls, 24.0f * ls, 0, 0, 0, 0.55f);
            renderer.text(w * 0.5f - Renderer::textWidth(msg, ls) * 0.5f, h * 0.40f, ls, msg, 1, 1, 1, 1);
        }
        if (hitFlashT > 0.0f) renderer.rect(0, 0, (float)w, (float)h, 0.85f, 0.0f, 0.0f, 0.38f * (hitFlashT / 0.40f));   // levou dano
        if (!shopOpen) { drawTags(); drawRescueMenu(); drawTeamOffer(); drawConquestHud(); }   // a loja é opaca: sem TAGs/menu de resgate por cima
        if (!shopOpen) {   // botões de salvar/carregar/soltar, barra de RCP
            if (rescuingNow()) {
                float u = std::min((float)h / 1080.0f, (float)w / 1900.0f), f = std::min(1.0f, rescueProg / e0net::kReviveTime);
                float bw = 520 * u, bh = 46 * u, bx = w * 0.5f - bw * 0.5f, by = h * 0.20f;
                renderer.rect(bx - 3, by - 3, bw + 6, bh + 6, 0.3f, 0.6f, 0.35f, 0.85f);
                renderer.rect(bx, by, bw, bh, 0.08f, 0.12f, 0.09f, 0.92f);
                renderer.rect(bx, by, bw * f, bh, 0.25f, 0.75f, 0.35f, 0.9f);
                char sm[48]; std::snprintf(sm, sizeof sm, "SALVANDO ID %d  %d%%", (int)(net.predicted().link ? net.predicted().link - 1 : 0), (int)(f * 100.0f));
                int ss = std::max(2, (int)std::lround(3 * u));
                renderer.text(bx + (bw - Renderer::textWidth(sm, ss)) * 0.5f, by + (bh - 7.0f * ss) * 0.5f, ss, sm, 1, 1, 1, 1);
            }
        }
        if (selfDead || wakeFadeT > 0.0f) {   // tela de morte: escurece em tom carvalho-noite com tipografia Cinzel rústica
            float a = selfDead ? 0.90f * std::min(1.0f, deadT / 6.0f) : 0.90f * (wakeFadeT / kWakeFade);
            if (selfDead) { float k = std::min(1.0f, deadT / 6.0f); a = 0.90f * k * k * (3.0f - 2.0f * k); }
            renderer.rect(0, 0, (float)w, (float)h, 0.03f, 0.02f, 0.015f, a);
            if (selfDead) {
                float ta = std::min(1.0f, std::max(0.0f, (deadT - 1.5f) / 1.0f));
                int ls = std::max(5, h / 120); const char* t1m = "VOC\xCA MORREU";
                renderer.text(w * 0.5f - Renderer::textWidth(t1m, ls) * 0.5f, h * 0.28f, ls, t1m, theme::kRedText.r, theme::kRedText.g, theme::kRedText.b, ta);
                int ds = std::max(2, h / 270); char dm[96];
                std::snprintf(dm, sizeof dm, "PERDEU %d ITENS (FICA A ADAGA E AS MOEDAS)", deathLostItems);
                renderer.text(w * 0.5f - Renderer::textWidth(dm, ds) * 0.5f, h * 0.28f + 10.0f * ls, ds, dm, theme::kCream.r, theme::kCream.g, theme::kCream.b, ta);
                const char* wm = (net.predicted().status & e0net::StCarried) ? "VOC\xCA EST\xC1 SENDO CARREGADO" : (net.connected() ? "AGUARDANDO RESGATE DE OUTRO JOGADOR..." : "SEM NINGU\xC9M PARA TE SALVAR");
                renderer.text(w * 0.5f - Renderer::textWidth(wm, ds) * 0.5f, h * 0.28f + 10.0f * ls + 12.0f * ds, ds, wm, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, ta);
                if (deathBtnVisible()) {
                    Rect2D bt = deathBtnRect(); float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
                    renderer.rect(bt.x - 3, bt.y - 3, bt.w + 6, bt.h + 6, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.92f);
                    renderer.rect(bt.x, bt.y, bt.w, bt.h, theme::kWaxRed.r, theme::kWaxRed.g, theme::kWaxRed.b, 0.96f);
                    int bs = std::max(3, (int)std::lround(4 * u)); const char* rl = "RENASCER";
                    renderer.text(bt.x + (bt.w - Renderer::textWidth(rl, bs)) * 0.5f, bt.y + (bt.h - 7.0f * bs) * 0.5f, bs, rl, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1);
                } else {
                    char cd[48]; std::snprintf(cd, sizeof cd, "RENASCER EM %d S", (int)std::ceil(kRespawnBtnAfter - deadT));
                    float cy = deathAdVisible() ? deathBtnRect().y + deathBtnRect().h + 24.0f : h * 0.74f + 20.0f;
                    renderer.text(w * 0.5f - Renderer::textWidth(cd, ds) * 0.5f, cy, ds, cd, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.65f * ta);
                    if (deathAdVisible()) {
                        Rect2D bt = deathBtnRect(); float u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
                        renderer.rect(bt.x - 3, bt.y - 3, bt.w + 6, bt.h + 6, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 0.92f * ta);
                        renderer.rect(bt.x, bt.y, bt.w, bt.h, 0.24f, 0.17f, 0.08f, 0.96f * ta);
                        int bs = std::max(2, (int)std::lround(3 * u)); const char* rl = "RENASCER AGORA (AN\xDANCIO)";
                        renderer.text(bt.x + (bt.w - Renderer::textWidth(rl, bs)) * 0.5f, bt.y + (bt.h - 7.0f * bs) * 0.5f, bs, rl, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, ta);
                    }
                }
            }
        }
        renderer.endText();
        drawTagFlags();
        if (!shopOpen) drawHoldMenu();
        if (!hudIcons.empty()) {
            renderer.beginUi();
            for (const HudIconReq& q : hudIcons) {
                if (q.idx < 0) renderer.uiCloseBtn(q.x, q.y, q.half, q.a);
                else renderer.uiIconTint(q.idx, q.x, q.y, q.half, q.r, q.g, q.b, q.a);
            }
            renderer.endUi();
            hudIcons.clear();
        }
    }
};

void onCmd(android_app* app, int32_t cmd) {
    auto* e = (Engine*)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) e->initSurface();
            hideSystemBars(app->activity);
            break;
        case APP_CMD_GAINED_FOCUS:
            hideSystemBars(app->activity);
            break;
        case APP_CMD_PAUSE: e->audio.pause(); break;
        case APP_CMD_RESUME: e->audio.resume(); break;
        case APP_CMD_TERM_WINDOW:
            e->termSurface();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (e->ready) {
                eglQuerySurface(e->dpy, e->surf, EGL_WIDTH, &e->w);
                eglQuerySurface(e->dpy, e->surf, EGL_HEIGHT, &e->h);
                e->renderer.resize(e->w, e->h);
            }
            break;
    }
}

int32_t onInput(android_app* app, AInputEvent* ev) {
    auto* e = (Engine*)app->userData;
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK) {
        if (!e->backHandled()) return 0;   // menu principal: o Android fecha o app
        if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP) e->doBack();
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) { e->handleTouch(ev); return 1; }
    return 0;
}

}  // namespace

void run(android_app* app) {
    std::unique_ptr<Engine> ep(new Engine());   // no HEAP: com 1000 jogadores os arrays por jogador (animação, áudio...) estouravam a pilha da thread
    Engine& e = *ep;
    e.app = app;
    app->userData = &e;
    app->onAppCmd = onCmd;
    app->onInputEvent = onInput;

    if (!e.initDisplay()) { LOG("falha ao iniciar EGL"); return; }
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);  // low-latency Wi-Fi exige tela ligada
    wifiLowLatencyAcquire(app->activity);
    hideSystemBars(app->activity);
#ifndef E0_CINEMA
    e.pay.init(app->activity, E0_ADS_TEST != 0, Engine::shopPlaySkus());   // AdMob + Google Play Billing
#endif
    e.net.setCacheDir(app->activity->internalDataPath ? app->activity->internalDataPath : "");  // cache do mundo baixado
    if (app->activity->internalDataPath) {
        e.cfgPath = std::string(app->activity->internalDataPath) + "/menu.cfg"; e.loadCfg();
        e.walletPath = std::string(app->activity->internalDataPath) + "/wallet.cfg"; e.loadWallet();
    }
    e.auth.init(app->activity, app->activity->internalDataPath ? std::string(app->activity->internalDataPath) : std::string());   // conta salva + Firebase (se o app tiver)
#ifdef E0_CINEMA
    e.menuDone = true; e.beginGame();   // cinema: sem menu, sempre offline
#endif
    // (fora do cinema a conexão só começa quando o jogador toca em JOGAR no menu principal: Engine::beginGame)
    e.game = createGame();
    if (e.game) e.game->onStart();
#ifdef E0_CINEMA
    e.cineInit();
#endif

    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (!app->destroyRequested) {
        int events;
        android_poll_source* src;
        // bloqueia enquanto não há superfície; senão roda sem esperar
        while (ALooper_pollOnce(e.ready ? 0 : -1, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested) break;
        if (!e.ready) { last = clock::now(); continue; }

        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        e.noteFrame(dt);
        if (dt > 0.1f) dt = 0.1f;
#ifdef E0_CINEMA
        e.cinePoll(dt);
        if (e.cineFrame(dt)) continue;
#endif

        if (e.menuActive) {   // menu principal: o mundo nem roda por baixo (ainda nem conectou)
            e.updateMenu(dt);
            if (e.menuActive) { e.drawMenu(); eglSwapBuffers(e.dpy, e.surf); continue; }
        }

        e.update(dt);
        e.updateLoading(dt);
        e.drawWorld();
#ifdef E0_CINEMA
        if (!e.cHideHud) { e.drawUi(); e.drawDebug(dt); }
        e.cineDrawPanel();
#else
        e.drawUi();
        if (!e.introActive && !e.navOpen && !e.wheelOpen && !e.flagOpen && !e.teamOpen) e.drawDebug(dt);   // painel NAVEGAÇÃO aberto: sem o painel de debug por cima
#endif
        e.drawLoading();
        eglSwapBuffers(e.dpy, e.surf);
    }

    delete e.game;
    e.net.stop();
    wifiLowLatencyRelease(app->activity);
    e.pay.shutdown();
    e.termAll();
}

}  // namespace e0
