// ================= TELA PERSONAGEM (escolha de SKIN) =================
// Incluído DENTRO de `struct Engine` (engine.cpp). Aberta pelo item PERSONAGEM do menu principal (antes de conectar).
// Layout: GRADE 3 x N à esquerda (rolagem vertical com inércia e elástico nas pontas; cada cartão mostra o personagem em 3D com a skin),
// PERSONAGEM grande no centro (arrastar na horizontal gira, com inércia; parado, gira sozinho devagar) e PAINEL à direita (nome, raridade,
// descrição, cores/material e EQUIPAR). A skin equipada fica em menu.cfg ("skin=") e vai ao servidor no Join (PlayerState.skin, protocolo v36).
// Tabela das skins: engine/src/skins.h.

bool charOpen = false;
int skinSel = 0;                  // equipada (salva em menu.cfg)
int charView = 0;                 // a que está sendo vista (tocada na grade)
float charT = 0.0f, charFlash = 0.0f, charViewT = 0.0f;
float charYaw = 3.14159265f - 0.45f, charYawVel = 0.0f, charIdle = 0.0f;
float gridScroll = 0.0f, gridVel = 0.0f;
int charMode = 0, charBtn = -1;   // toque: 0 nada, 1 grade, 2 girar, 3 botão (0 VOLTAR, 1 EQUIPAR)
float charX0 = 0, charY0 = 0, charXL = 0, charYL = 0; bool charMoved = false;
std::chrono::steady_clock::time_point charMoveAt;
AnimPlayer charAnim;
AnimPlayer gridAnim[kCharModels];   // pose PARADA e CONGELADA dos cartões da grade (só o personagem grande anima), uma por modelo (cada um tem o seu esqueleto)
// Personagem grande: só idles CALMOS (PARADO + 6 VIDLE do Mixamo), sorteados a cada 7..12 s e trocados com transição suave
int charMove = 0; float charMoveT = 0.0f, charMoveDur = 2.0f; uint32_t charRng = 0x9E3779B9u; int charActKind = 1;
uint32_t charRand() { charRng ^= charRng << 13; charRng ^= charRng >> 17; charRng ^= charRng << 5; return charRng; }
void pickCharMove() {   // só idles CALMOS (PARADO + os 6 VIDLE), sem repetir o anterior; troca com transição suave (AnimPlayer)
    int m = charMove;
    while (m == charMove) m = (int)(charRand() % (1 + ClipViewIdleN));   // 0 = PARADO do jogo, 1..6 = VIDLE
    charMove = m; charMoveT = 0.0f;
    charMoveDur = 7.0f + (float)(charRand() % 50) * 0.1f;   // 7..12 s em cada
}

struct CharLayout { float u, gx, gy, gw, gh, cardW, cardH, gap, cx0, cx1, rx, ry, rw, rh; Rect2D back, equip; };
CharLayout charLayout() const {
    CharLayout L;
    L.u = (float)h / 1080.0f;
    const float u = L.u;
    L.gap = 16.0f * u; L.cardW = 212.0f * u; L.cardH = 252.0f * u;
    L.gx = 52.0f * u; L.gy = 150.0f * u; L.gw = 3.0f * L.cardW + 2.0f * L.gap; L.gh = (float)h - L.gy - 36.0f * u;
    L.rw = std::min(560.0f * u, (float)w * 0.27f); L.rx = (float)w - L.rw - 52.0f * u; L.ry = 150.0f * u; L.rh = (float)h - L.ry - 36.0f * u;
    L.cx0 = L.gx + L.gw + 24.0f * u; L.cx1 = L.rx - 24.0f * u;
    L.back = {40.0f * u, 34.0f * u, 250.0f * u, 82.0f * u};
    L.equip = {L.rx + 30.0f * u, L.ry + L.rh - 136.0f * u, L.rw - 60.0f * u, 104.0f * u};
    return L;
}
float gridMax(const CharLayout& L) const {
    const int rows = (e0net::kSkinCount + 2) / 3;
    return std::max(0.0f, rows * (L.cardH + L.gap) - L.gap - L.gh);
}
Rect2D cardRect(const CharLayout& L, int i) const {
    return {L.gx + (float)(i % 3) * (L.cardW + L.gap), L.gy + (float)(i / 3) * (L.cardH + L.gap) - gridScroll, L.cardW, L.cardH};
}
int cardAt(const CharLayout& L, float x, float y) const {
    if (x < L.gx || x > L.gx + L.gw || y < L.gy || y > L.gy + L.gh) return -1;
    for (int i = 0; i < e0net::kSkinCount; ++i) if (cardRect(L, i).contains(x, y)) return i;
    return -1;
}

void openCharScreen() {
    charOpen = true; charT = 0.0f; charView = skinSel; charViewT = 1.0f; charFlash = 0.0f;
    charYaw = 3.14159265f - 0.45f; charYawVel = 0.0f; charIdle = 0.0f; charMode = 0;
    charMove = 0; charMoveT = 0.0f; charMoveDur = 6.0f; charRng ^= (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    const CharLayout L = charLayout();
    gridScroll = std::max(0.0f, std::min(gridMax(L), (float)(skinSel / 3) * (L.cardH + L.gap) - L.gh * 0.3f));   // a equipada à vista
    gridVel = 0.0f;
    for (int m = 0; m < kCharModels; ++m) {   // um único quadro: parado em pé
        gridAnim[m] = AnimPlayer(); int sk = 0; for (int i = 0; i < e0net::kSkinCount; ++i) if (skinDef(i).model == m) { sk = i; break; }
        gridAnim[m].setSkeleton(renderer.skeletonOfSkin(sk)); gridAnim[m].update(0.6f, AnimInput(), renderer.anims());
    }
    LOG("tela PERSONAGEM aberta (skin %d)", skinSel);
}
void closeCharScreen() { charOpen = false; charMode = 0; }
void equipSkin(int s) {
    skinSel = std::max(0, std::min(e0net::kSkinCount - 1, s));
    net.setSkin(skinSel);
    saveCfg();
    charFlash = 1.0f;
    LOG("skin equipada: %d (%s)", skinSel, skinDef(skinSel).name);
}

void updateCharScreen(float dt) {
    charT += dt; charViewT += dt;
    charFlash = std::max(0.0f, charFlash - dt * 1.4f);
    AnimInput ai;   // em pé no lugar; o movimento sorteado entra pela velocidade/ação (a mistura de poses é a do jogo)
    charMoveT += dt;
    if (charMoveT >= charMoveDur) pickCharMove();
    ai.forceClip = charMove == 0 ? (int)ClipIdle : (int)ClipViewIdle0 + charMove - 1;
    charAnim.setSkeleton(renderer.skeletonOfSkin(charView));
    charAnim.update(dt, ai, renderer.anims());
    const CharLayout L = charLayout();
    const float mx = gridMax(L);
    if (charMode != 1) {   // grade solta: inércia + volta elástica nas pontas
        gridScroll += gridVel * dt;
        gridVel *= std::exp(-4.5f * dt);
        if (gridScroll < 0.0f) { gridScroll += (0.0f - gridScroll) * std::min(1.0f, dt * 12.0f); gridVel = 0.0f; }
        else if (gridScroll > mx) { gridScroll += (mx - gridScroll) * std::min(1.0f, dt * 12.0f); gridVel = 0.0f; }
        if (std::fabs(gridVel) < 4.0f) gridVel = 0.0f;
    }
    if (charMode != 2) {   // personagem solto: inércia; parado há 2,5 s gira sozinho devagar
        charYaw += charYawVel * dt;
        charYawVel *= std::exp(-3.2f * dt);
        charIdle += dt;
        if (charIdle > 2.5f && std::fabs(charYawVel) < 0.3f) charYaw += 0.40f * dt;
    }
}

void charTouch(int ty, float x, float y) {
    const CharLayout L = charLayout();
    const auto now = std::chrono::steady_clock::now();
    if (ty == 0) {
        charX0 = charXL = x; charY0 = charYL = y; charMoved = false; charBtn = -1; charMode = 0; charMoveAt = now;
        if (L.back.contains(x, y)) { charMode = 3; charBtn = 0; }
        else if (L.equip.contains(x, y)) { charMode = 3; charBtn = 1; }
        else if (x >= L.gx - 14.0f * L.u && x <= L.gx + L.gw + 14.0f * L.u && y >= L.gy) { charMode = 1; gridVel = 0.0f; }
        else if (x > L.cx0 - 20.0f * L.u && x < L.cx1 + 20.0f * L.u) { charMode = 2; charYawVel = 0.0f; charIdle = 0.0f; }
        return;
    }
    if (ty == 1) {
        const float dtm = std::max(0.004f, std::chrono::duration<float>(now - charMoveAt).count());
        const float dx = x - charXL, dy = y - charYL;
        if (std::fabs(x - charX0) + std::fabs(y - charY0) > 16.0f * L.u) charMoved = true;
        if (charMode == 1) {
            float s = gridScroll - dy;
            const float mx = gridMax(L);
            if (s < 0.0f || s > mx) s = gridScroll - dy * 0.35f;   // resistência elástica além das pontas
            gridScroll = s;
            gridVel = gridVel * 0.4f + (-dy / dtm) * 0.6f;
        } else if (charMode == 2) {
            const float k = 0.0062f / std::max(0.5f, L.u);   // rad por pixel
            charYaw -= dx * k;   // arrastar para a direita gira a frente do personagem para a direita
            charYawVel = charYawVel * 0.4f + (-dx * k / dtm) * 0.6f;
            charIdle = 0.0f;
        }
        charXL = x; charYL = y; charMoveAt = now;
        return;
    }
    if (ty == 2) {
        if (std::chrono::duration<float>(now - charMoveAt).count() > 0.12f) { gridVel = 0.0f; charYawVel = 0.0f; }   // segurou parado antes de soltar
        if (charMode == 1 && !charMoved) {
            const int i = cardAt(L, x, y);
            if (i >= 0 && i != charView) { charView = i; charViewT = 0.0f; }
            else if (i >= 0 && i == charView && i != skinSel) equipSkin(i);   // 2º toque no mesmo cartão = equipar
        } else if (charMode == 3 && !charMoved) {
            if (charBtn == 0 && L.back.contains(x, y)) closeCharScreen();
            else if (charBtn == 1 && L.equip.contains(x, y) && charView != skinSel) equipSkin(charView);
        }
    }
    charMode = 0; charBtn = -1;
}

// quebra `s` em linhas que caibam em `maxW` pixels (usando largura real da fonte Cinzel)
static int wrapLinesPx(const char* s, int ts, float maxW, char out[][72], int maxLines) {
    int nl = 0, col = 0; out[0][0] = 0;
    while (*s && nl < maxLines) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char* e = s; while (*e && *e != ' ') ++e;
        int wl = std::min((int)(e - s), 70);
        char cand[72];
        int cl = col;
        if (cl > 0) { std::memcpy(cand, out[nl], (size_t)cl); cand[cl++] = ' '; }
        int add = std::min(wl, 70 - cl);
        std::memcpy(cand + cl, s, (size_t)add); cand[cl + add] = 0;
        if (col > 0 && Renderer::textWidth(cand, ts) > maxW) {
            ++nl; col = 0; if (nl >= maxLines) break;
            out[nl][0] = 0;
        }
        if (col > 0) out[nl][col++] = ' ';
        int cp = std::min(wl, 70 - col);
        std::memcpy(out[nl] + col, s, (size_t)cp); col += cp; out[nl][col] = 0;
        s = e;
    }
    return nl + (col > 0 ? 1 : 0);
}
// mantido para chamadores legados por contagem de caracteres
static int wrapLines(const char* s, int maxc, char out[][72], int maxLines) {
    return wrapLinesPx(s, 1, (float)maxc * 5.2f, out, maxLines);
}

void drawCharScreen() {
    const CharLayout L = charLayout();
    const float u = L.u, W = (float)w, H = (float)h;
    const float fin = std::min(1.0f, charT / 0.35f);
    const SkinDef& sv = skinDef(charView);
    const Vec3 rc = rarityColor(sv.rarity);
    const float ccx = 0.5f * (L.cx0 + L.cx1);
    // câmera do personagem grande e onde os pés dele caem na tela (para o palco ficar embaixo deles)
    Camera bigCam; bigCam.position = {0.0f, 1.02f, 4.35f}; bigCam.yaw = 0.0f; bigCam.pitch = -0.035f; bigCam.fovY = 0.56f; bigCam.zNear = 0.1f; bigCam.zFar = 50.0f;
    const float bigTop = 120.0f * u, bigBot = H - 40.0f * u;
    const float yc = bigCam.position.y + bigCam.position.z * std::tan(bigCam.pitch), halfV = bigCam.position.z * std::tan(bigCam.fovY * 0.5f);
    const float floorY = bigTop + (bigBot - bigTop) * (0.5f + 0.5f * yc / halfV);
    const int ts = std::max(2, (int)std::lround(3.0f * u));

    glClearColor(0.03f, 0.045f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.setEnvironment(makeEnvironment(560.0));   // tarde clara: luz de lado, céu azul no reflexo

    // ---- 1) fundo náutico noturno + luz quente de lampião + painéis de carvalho/latão ----
    renderer.beginUi();
    renderer.uiWoodPanel(W * 0.5f, H * 0.5f, W * 0.5f, H * 0.5f, 0.0f, theme::PanelBgSea, 1.0f, 7.0f);
    const float pick = std::min(1.0f, charViewT / 0.4f);
    // luz quente de lampião / pôr-do-sol atrás do pirata (casa diretamente com o Main Menu)
    renderer.uiGlow(ccx, H * 0.48f, (L.cx1 - L.cx0) * 0.50f, H * 0.48f, 0.95f, 0.68f, 0.30f, (0.14f + 0.06f * (1.0f - pick)) * fin);
    renderer.uiGlow(ccx, floorY, 260.0f * u, 52.0f * u, 0.88f, 0.66f, 0.28f, 0.24f * fin);   // aura quente no piso
    renderer.uiGlow(ccx, floorY, 185.0f * u, 30.0f * u, 0.0f, 0.0f, 0.0f, 0.72f * fin);      // sombra nos pés
    // divisória sutil no cabeçalho
    renderer.uiDivider(L.back.x + L.back.w + 260.0f * u, 126.0f * u, 230.0f * u, 8.0f * u, 0.75f * fin);
    // painel da grade (carvalho escuro + latão + rebites)
    renderer.uiWoodPanel(L.gx + L.gw * 0.5f, L.gy + L.gh * 0.5f - 6.0f * u, L.gw * 0.5f + 22.0f * u, L.gh * 0.5f + 22.0f * u, 18.0f * u, theme::PanelWood, 0.96f * fin, 1.0f);
    // cartões (recortados no painel)
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)(L.gx - 12.0f * u), (GLint)(H - L.gy - L.gh), (GLsizei)(L.gw + 24.0f * u), (GLsizei)L.gh);
    for (int i = 0; i < e0net::kSkinCount; ++i) {
        const Rect2D r = cardRect(L, i);
        if (r.y + r.h < L.gy || r.y > L.gy + L.gh) continue;
        const SkinDef& sd = skinDef(i);
        const Vec3 c = rarityColor(sd.rarity);
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        const bool view = i == charView;
        renderer.uiWoodPanel(cx, cy, r.w * 0.5f, r.h * 0.5f, 12.0f * u, view ? theme::PanelCardSel : theme::PanelCard, 0.98f, (float)i + 2.0f);
        renderer.uiGlow(cx, r.y + r.h * 0.62f, r.w * 0.30f, 14.0f * u, 0, 0, 0, 0.55f);   // sombra dos pés
        // filete discreto da raridade na base
        renderer.uiRoundRect(cx, r.y + r.h - 6.0f * u, r.w * 0.5f - 18.0f * u, 2.5f * u, 2.0f * u, c.x * 0.85f + 0.12f, c.y * 0.78f + 0.10f, c.z * 0.65f + 0.08f, 0.85f);
    }
    glDisable(GL_SCISSOR_TEST);
    // painel da direita (ficha do pirata em carvalho escuro + latão + rebites)
    renderer.uiWoodPanel(L.rx + L.rw * 0.5f, L.ry + L.rh * 0.5f, L.rw * 0.5f, L.rh * 0.5f, 18.0f * u, theme::PanelWood, 0.96f * fin, 3.0f);
    // VOLTAR (carvalho & latão)
    {
        const bool p = charMode == 3 && charBtn == 0;
        const Rect2D& b = L.back;
        renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 12.0f * u, p ? theme::BtnBrass : theme::BtnWood, 0.96f);
    }
    // EQUIPAR (latão batido) / EQUIPADO (verde-musgo náutico)
    const bool equipped = charView == skinSel;
    {
        const bool p = charMode == 3 && charBtn == 1;
        const Rect2D& b = L.equip;
        renderer.uiButton(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, b.h * 0.5f, 14.0f * u, equipped ? theme::BtnMoss : theme::BtnBrass, p ? 0.85f : 1.0f);
        if (charFlash > 0.0f) renderer.uiGlow(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.70f, b.h * 1.2f, 0.90f, 0.78f, 0.42f, 0.45f * charFlash);
    }
    // amostra de cor do material (se houver)
    const float swY = L.ry + 560.0f * u;
    if (sv.tintK > 0.0f) {
        const float sx = L.rx + 54.0f * u;
        renderer.uiShape(Renderer::UiFill, sx, swY, 24.0f * u, sv.tint.x, sv.tint.y, sv.tint.z, 1.0f);
        renderer.uiShape(Renderer::UiRing, sx, swY, 26.0f * u, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.85f);
    }
    renderer.endUi();

    // ---- 2) personagens em 3D ----
    {   // grande no centro
        CharInst ci{{0.0f, 0.0f, 0.0f}, charYaw, {1.0f, 1.0f, 1.0f}, false, charAnim.boneRows(), 0.0f, 0};
        ci.skin = charView;
        renderer.drawCharPreview(ci, (int)L.cx0, (int)(H - bigBot), (int)(L.cx1 - L.cx0), (int)(bigBot - bigTop), bigCam, 0);
    }
    {   // mini personagem em cada cartão (recortado no painel da grade)
        const GLint sy = (GLint)(H - L.gy - L.gh), sh = (GLint)L.gh;
        Camera cam; cam.position = {0.0f, 0.98f, 3.10f}; cam.yaw = 0.0f; cam.pitch = -0.03f; cam.fovY = 0.66f; cam.zNear = 0.1f; cam.zFar = 30.0f;
        for (int i = 0; i < e0net::kSkinCount; ++i) {
            const Rect2D r = cardRect(L, i);
            if (r.y + r.h < L.gy || r.y > L.gy + L.gh) continue;
            const float ph = r.h - 50.0f * u;   // área da figura (embaixo fica o nome)
            const int gm = std::max(0, std::min(kCharModels - 1, skinDef(i).model));
            CharInst ci{{0.0f, 0.0f, 0.0f}, 3.14159265f - 0.35f, {1.0f, 1.0f, 1.0f}, false, gridAnim[gm].boneRows(), 0.0f, 0};
            ci.skin = i;
            const GLint vx = (GLint)r.x, vy = (GLint)(H - r.y - ph), vw = (GLint)r.w, vh = (GLint)ph;
            const GLint y0 = std::max(vy, sy), y1 = std::min(vy + vh, sy + sh);
            if (y1 <= y0) continue;
            renderer.drawCharPreview(ci, vx, vy, vw, vh, cam, 1, vx, y0, vw, y1 - y0);
        }
    }

    // ---- 3) textos ----
    renderer.beginText();
    // nomes nos cartões (recortados)
    for (int i = 0; i < e0net::kSkinCount; ++i) {
        const Rect2D r = cardRect(L, i);
        if (r.y + r.h < L.gy || r.y > L.gy + L.gh) continue;
        const SkinDef& sd = skinDef(i);
        // nome no cartão: o completo se couber no tamanho normal; senão só o primeiro nome (ex. BARTOLOMEU)
        char cn[48]; std::snprintf(cn, sizeof cn, "%s", sd.name);
        int ns = ts - 1;
        if (Renderer::textWidth(cn, ns) > r.w - 12.0f * u) { if (char* sp = std::strchr(cn, ' ')) *sp = 0; }
        while (ns > 1 && Renderer::textWidth(cn, ns) > r.w - 12.0f * u) --ns;
        const float tw = Renderer::textWidth(cn, ns);
        renderer.text(r.x + (r.w - tw) * 0.5f, r.y + r.h - 38.0f * u, ns, cn, i == charView ? 1.0f : 0.92f, i == charView ? 0.84f : 0.92f, i == charView ? 0.40f : 0.90f, 1.0f);
        if (i == skinSel) {   // selo EM USO em verde-musgo náutico com fio bronze
            const int bs = std::max(1, ts - 1);
            const char* t = "EM USO";
            const float bw = Renderer::textWidth(t, bs) + 16.0f * u, bh = 7.0f * bs + 12.0f * u;
            renderer.rect(r.x + 10.0f * u - 1.5f * u, r.y + 10.0f * u - 1.5f * u, bw + 3.0f * u, bh + 3.0f * u, 0.48f, 0.74f, 0.52f, 0.85f);
            renderer.rect(r.x + 10.0f * u, r.y + 10.0f * u, bw, bh, theme::kSeaGreen.r, theme::kSeaGreen.g, theme::kSeaGreen.b, 0.96f);
            renderer.text(r.x + 18.0f * u, r.y + 16.0f * u, bs, t, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f, false);
        }
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)(L.gx - 12.0f * u), (GLint)(H - L.gy - L.gh), (GLsizei)(L.gw + 24.0f * u), (GLsizei)L.gh);
    renderer.endText();
    glDisable(GL_SCISSOR_TEST);

    renderer.beginText();
    // título e VOLTAR
    {
        const int s2 = std::max(2, (int)std::lround(5.5f * u));
        renderer.text(L.back.x + L.back.w + 36.0f * u, 40.0f * u, s2, "PERSONAGEM", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, fin);
        const char* cnt = "ESCOLHA SEU PIRATA";
        renderer.text(L.back.x + L.back.w + 36.0f * u, 40.0f * u + 7.0f * s2 + 12.0f * u, ts - 1, cnt, theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 0.95f * fin, false);
        const char* bt = "< VOLTAR";
        const bool p = charMode == 3 && charBtn == 0;
        const theme::Col bc = p ? theme::kInk : theme::kBrassBright;
        renderer.text(L.back.x + (L.back.w - Renderer::textWidth(bt, ts)) * 0.5f, L.back.y + (L.back.h - 7.0f * ts) * 0.5f, ts, bt, bc.r, bc.g, bc.b, 1.0f, !p);
    }
    // painel da direita
    {
        const float px = L.rx + 34.0f * u, pw = L.rw - 68.0f * u;
        float y = L.ry + 34.0f * u;
        // raridade (etiqueta rústica com filete lateral)
        const char* rn = rarityName(sv.rarity);
        const float rbw = Renderer::textWidth(rn, ts) + 28.0f * u;
        const float pillH = 7.0f * ts + 20.0f * u;
        renderer.rect(px, y, rbw, pillH, theme::kWoodDeep.r, theme::kWoodDeep.g, theme::kWoodDeep.b, 0.85f);
        renderer.rect(px, y, 4.0f * u, pillH, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.95f);
        renderer.text(px + 15.0f * u, y + (pillH - 7.0f * ts) * 0.5f, ts, rn, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        y += pillH + 34.0f * u;
        // nome grande (maior escala que cabe)
        int ns = std::max(2, (int)std::lround(6.5f * u));
        while (ns > 2 && Renderer::textWidth(sv.name, ns) > pw) --ns;
        renderer.text(px, y, ns, sv.name, theme::kCream.r, theme::kCream.g, theme::kCream.b, 1.0f);
        y += 7.0f * ns + 16.0f * u;
        // apelido
        renderer.text(px, y, ts, sv.title, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f, false);
        y += 7.0f * ts + 30.0f * u;
        // história (quebra pela largura real em pixels da fonte Cinzel)
        char lines[8][72];
        const int nl = wrapLinesPx(sv.desc, ts, pw - 8.0f * u, lines, 8);
        for (int i = 0; i < nl; ++i) { renderer.text(px, y, ts, lines[i], theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 1.0f, false); y += 7.0f * ts + 12.0f * u; }
        // origem
        y += 26.0f * u;
        renderer.text(px, y, ts - 1, "ORIGEM", theme::kCreamMuted.r, theme::kCreamMuted.g, theme::kCreamMuted.b, 1.0f, false);
        renderer.text(px, y + 7.0f * (ts - 1) + 12.0f * u, ts, sv.origin, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 1.0f, false);
        // botão
        const Rect2D& b = L.equip;
        const char* bl = equipped ? "EQUIPADO" : "EQUIPAR";
        const int bs = ts + 1;
        const theme::Col tc = equipped ? theme::kCream : theme::kInk;
        renderer.text(b.x + (b.w - Renderer::textWidth(bl, bs)) * 0.5f, b.y + (b.h - 7.0f * bs) * 0.5f, bs, bl, tc.r, tc.g, tc.b, 1.0f, equipped);
    }
    // dica de giro sob o palco
    {
        const char* hint = "ARRASTE PARA GIRAR";
        const int hs = std::max(1, ts - 1);
        renderer.text(ccx - Renderer::textWidth(hint, hs) * 0.5f, H - 52.0f * u, hs, hint, theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.65f * fin);
    }
    renderer.endText();
}
