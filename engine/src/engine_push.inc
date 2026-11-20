// MENSAGENS PUSH DENTRO DO JOGO (Firebase Cloud Messaging). Incluído dentro de `struct Engine`.
// Com o app ABERTO o PushService (Java) não cria notificação do sistema: enfileira "título\x1fcorpo\x1fação" em GameActivity e o jogo mostra aqui um
// BANNER no tema do jogo (moldura dourada, fundo de madeira escura, título dourado). Tocar abre a ação (loja, navegação, mapa, craft); X dispensa.
// Enviar: console do Firebase > Messaging > Nova campanha; em "Opções adicionais > Dados personalizados" use a chave `action` = loja | navegacao | craft | mapa.

struct PushBanner { std::string title, body; int action = 0; float age = 0.0f; };   // action: 0 nenhuma, 1 loja, 2 navegação, 3 craft, 4 mapa
std::vector<PushBanner> pushQ;     // [0] = o que está na tela
float pushPollT = 0.0f;

static int pushActionFromName(const std::string& s) {
    if (s == "loja" || s == "shop") return 1;
    if (s == "navegacao" || s == "nav" || s == "navigation") return 2;
    if (s == "craft") return 3;
    if (s == "mapa" || s == "map") return 4;
    if (s == "tripulacao" || s == "crew") return 5;
    return 0;
}
static const char* pushActionLabel(int a) { return a == 1 ? "ABRIR LOJA" : (a == 2 ? "NAVEGAR" : (a == 3 ? "ABRIR CRAFT" : (a == 4 ? "ABRIR MAPA" : (a == 5 ? "ABRIR TRIPULA\xC7\xC3" "O" : "")))); }
void pushRun(int a) {
    if (a == 1) openShop(); else if (a == 2) openNav(); else if (a == 3) openCraft(); else if (a == 4) openMap(); else if (a == 5) openTeam();
}
bool pushVisible() const { return !pushQ.empty() && !(menuActive && charOpen) && !loadActive && !introActive && !selfDead && !uiFull() && !navOpen && !wheelOpen && !gunnerMode && !isleMode; }

struct PushGeo { float u, x, y, w, h, bx, by, bw, bh, cx, cy, cr; int tT, tB, lines; std::vector<std::string> wrapped; };
// quebra o texto em linhas que cabem em maxW (tamanho de fonte ts)
static void pushWrap(const std::string& s, int ts, float maxW, std::vector<std::string>& out, int maxLines) {
    out.clear(); std::string cur, word;
    auto flush = [&]() {
        if (word.empty()) return;
        std::string t = cur.empty() ? word : cur + " " + word;
        if (cur.empty() || Renderer::textWidth(t.c_str(), ts) <= maxW) cur = t;
        else { out.push_back(cur); cur = word; }
        word.clear();
    };
    for (char c : s) { if (c == ' ' || c == '\n') flush(); else word += c; }
    flush(); if (!cur.empty()) out.push_back(cur);
    if ((int)out.size() > maxLines) { out.resize((size_t)maxLines); out.back() += "..."; }
}
bool pushGeo(PushGeo& g) const {
    if (pushQ.empty()) return false;
    const PushBanner& m = pushQ[0];
    g.u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
    g.tT = std::max(3, (int)std::lround(5.0f * g.u)); g.tB = std::max(2, (int)std::lround(3.5f * g.u));
    g.w = std::min((float)w - 24.0f, 960.0f * g.u);
    pushWrap(m.body, g.tB, g.w - 150.0f * g.u, g.wrapped, 3);
    g.lines = (int)g.wrapped.size();
    g.h = (28.0f * g.u) + (m.title.empty() ? 0.0f : 10.0f * (float)g.tT + 14.0f * g.u) + (float)g.lines * 11.0f * (float)g.tB + (m.action ? 62.0f * g.u : 0.0f) + 18.0f * g.u;
    g.x = ((float)w - g.w) * 0.5f;
    const float in = std::min(1.0f, m.age / 0.35f), out = std::min(1.0f, std::max(0.0f, (pushLife(m) - m.age) / 0.35f));
    const float k = std::min(in * in * (3 - 2 * in), out * out * (3 - 2 * out));
    g.y = 16.0f * g.u - (1.0f - k) * (g.h + 40.0f * g.u);
    g.cr = 22.0f * g.u; g.cx = g.x + g.w - g.cr - 14.0f * g.u; g.cy = g.y + g.cr + 14.0f * g.u;
    return true;
}
static float pushLife(const PushBanner& m) { return 7.0f + 0.05f * (float)(m.body.size() + m.title.size()); }

void drawPushOverlay() {   // fora do jogo (menu principal): passe de texto próprio + ícones
    if (!pushVisible()) return;
    renderer.beginText(); drawPush(); renderer.endText();
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
void updatePush(float dt) {
    if ((pushPollT -= dt) <= 0.0f) {
        pushPollT = 0.25f;
        std::string raw;
        while (pushQ.size() < 4 && e0::pushPoll(app->activity, raw)) {
            std::vector<std::string> f; { std::string cur; for (char c : raw) { if (c == '\x1f') { f.push_back(cur); cur.clear(); } else cur += c; } f.push_back(cur); }
            f.resize(3);
            PushBanner b; b.title = utf8ToLatin1(f[0]); b.body = utf8ToLatin1(f[1]); b.action = pushActionFromName(f[2]);
            if (b.title.empty() && b.body.empty()) { if (b.action) pushRun(b.action); continue; }   // veio de um toque na notificação do sistema: só executa a ação
            if (b.title.empty()) b.title = "ISOLATED";
            pushQ.push_back(b);
        }
    }
    if (pushVisible()) {
        pushQ[0].age += dt;
        if (pushQ[0].age >= pushLife(pushQ[0])) pushQ.erase(pushQ.begin());
    }
}
bool pushTouch(float x, float y) {   // true = o toque foi no banner (consumido)
    PushGeo g; if (!pushVisible() || !pushGeo(g) || pushQ[0].age < 0.2f) return false;
    if (x < g.x || x > g.x + g.w || y < g.y || y > g.y + g.h) return false;
    const float dx = x - g.cx, dy = y - g.cy;
    const int act = (dx * dx + dy * dy <= g.cr * g.cr * 1.6f) ? 0 : pushQ[0].action;   // X só dispensa; o resto do banner executa a ação
    pushQ.erase(pushQ.begin());
    if (act) pushRun(act);
    return true;
}
// desenho no passe de texto/retângulos (como as notificações do canto): madeira escura em degradê, moldura dourada dupla, cantoneiras e título em negrito dourado
void drawPush() {
    PushGeo g; if (!pushVisible() || !pushGeo(g)) return;
    const PushBanner& m = pushQ[0];
    const float u = g.u, x = g.x, y = g.y, ww = g.w, hh = g.h, fa = 1.0f;
    renderer.rect(x - 4.0f * u, y - 4.0f * u, ww + 8.0f * u, hh + 8.0f * u, 0.0f, 0.0f, 0.0f, 0.35f * fa);                              // sombra
    renderer.rect(x - 3.0f * u, y - 3.0f * u, ww + 6.0f * u, hh + 6.0f * u, 0.86f, 0.66f, 0.26f, 0.98f * fa);                           // moldura externa dourada
    renderer.rect(x - 1.0f * u, y - 1.0f * u, ww + 2.0f * u, hh + 2.0f * u, 0.12f, 0.08f, 0.04f, 1.0f);                                 // fio escuro
    const int strips = 24;
    for (int k = 0; k < strips; ++k) {                                                                                                    // fundo de madeira escura (degradê vertical)
        const float t = (float)k / (float)(strips - 1), r = 0.17f - 0.07f * t, gg = 0.11f - 0.05f * t, b = 0.07f - 0.035f * t;
        renderer.rect(x, y + hh * (float)k / (float)strips, ww, hh / (float)strips + 1.0f, r, gg, b, 0.97f * fa);
    }
    renderer.rect(x + 8.0f * u, y + 8.0f * u, ww - 16.0f * u, 2.0f * u, 0.86f, 0.66f, 0.26f, 0.85f * fa);                                // moldura interna dourada
    renderer.rect(x + 8.0f * u, y + hh - 10.0f * u, ww - 16.0f * u, 2.0f * u, 0.86f, 0.66f, 0.26f, 0.85f * fa);
    renderer.rect(x + 8.0f * u, y + 8.0f * u, 2.0f * u, hh - 16.0f * u, 0.86f, 0.66f, 0.26f, 0.85f * fa);
    renderer.rect(x + ww - 10.0f * u, y + 8.0f * u, 2.0f * u, hh - 16.0f * u, 0.86f, 0.66f, 0.26f, 0.85f * fa);
    for (int c = 0; c < 4; ++c) {                                                                                                         // rebites nos cantos
        const float cx = (c & 1) ? x + ww - 9.0f * u : x + 9.0f * u, cy = (c & 2) ? y + hh - 9.0f * u : y + 9.0f * u;
        renderer.rect(cx - 5.0f * u, cy - 5.0f * u, 10.0f * u, 10.0f * u, 0.95f, 0.78f, 0.34f, 1.0f * fa);
    }
    // ícone (bandeira pirata) à esquerda
    hudIcon(HiPirateFlag, x + 62.0f * u, y + hh * 0.5f - (m.action ? 22.0f * u : 0.0f), 34.0f * u, 1.0f, 0.82f, 0.36f, 0.95f * fa);
    // textos
    const float tx = x + 118.0f * u;
    float ty = y + 24.0f * u;
    if (!m.title.empty()) { shopBold(tx, ty, g.tT, m.title.c_str(), 1.0f, 0.84f, 0.40f, fa); ty += 10.0f * (float)g.tT + 14.0f * u; }
    for (const std::string& l : g.wrapped) { renderer.text(tx, ty, g.tB, l.c_str(), 0.95f, 0.91f, 0.80f, fa, false); ty += 11.0f * (float)g.tB; }
    if (m.action) {   // botão dourado da ação
        const char* lb = pushActionLabel(m.action);
        const int tb = std::max(2, (int)std::lround(4.0f * u));
        const float bw = Renderer::textWidth(lb, tb) + 44.0f * u, bh = 40.0f * u, bx = x + ww - bw - 26.0f * u, by = y + hh - bh - 24.0f * u;
        renderer.rect(bx - 2.0f * u, by - 2.0f * u, bw + 4.0f * u, bh + 4.0f * u, 0.45f, 0.30f, 0.08f, 1.0f);
        renderer.rect(bx, by, bw, bh * 0.5f, 0.96f, 0.78f, 0.32f, 1.0f);
        renderer.rect(bx, by + bh * 0.5f, bw, bh * 0.5f, 0.82f, 0.60f, 0.20f, 1.0f);
        renderer.text(bx + (bw - Renderer::textWidth(lb, tb)) * 0.5f, by + (bh - 7.0f * (float)tb) * 0.5f, tb, lb, 0.16f, 0.09f, 0.02f, 1.0f, false);
    }
    closeBtnQueue({g.cx - g.cr, g.cy - g.cr, g.cr * 2.0f, g.cr * 2.0f});   // X vermelho (dispensa)
}
