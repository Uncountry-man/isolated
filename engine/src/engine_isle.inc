// Isolated - MELHORIAS DA ILHA no app (incluído dentro de struct Engine; regras em shared/isle_build.h, autoridade no servidor).
//  * Botão CONSTRUIR da roda de ações: só abre numa ilha do oceano dominada pela SUA tripulação (você precisa estar nela).
//  * MURALHA: não se posiciona — escolher o cartão mostra o anel em volta da ilha inteira (com portão e cais) e o botão ERGUER MURALHA.
//  * Vista DE CIMA da ilha (câmera inclinada): 1 dedo arrasta a vista, 2 dedos dão zoom; tocar no chão posiciona a peça escolhida;
//    arrastar a própria peça a move. Embaixo, cartões com ROLAGEM HORIZONTAL (miniatura 3D, nome, materiais) para escolher o que erguer.
//  * Sem peça escolhida, tocar numa estrutura a seleciona (DEMOLIR com 2 toques; devolve metade dos materiais).

bool isleMode = false;
int isleSlot = -1;                  // ilha sendo melhorada
float isleCx = 0, isleCz = 0, isleDist = 70.0f, isleDistT = 70.0f;   // foco da vista (chão) e distância da câmera
float isleOx = 0, isleOz = 0;       // centro da ilha (limite do arrasto)
int isleSel = -1;                   // cartão escolhido (StructKind) ou -1
uint8_t isleRot = 0;                // giro da peça (x 45 graus)
float isleGx = 0, isleGz = 0, isleGy = 0; bool isleGhostSet = false; uint8_t isleGhostRes = 0;   // peça sendo posicionada
uint16_t islePick = 0;              // estrutura selecionada (demolir)
float isleDemolT = 0;               // 2º toque confirma (s restantes)
float isleScroll = 0, isleVel = 0;  // rolagem dos cartões
std::string isleMsg; float isleMsgT = 0; bool isleMsgBad = false;
uint16_t isleWaitReq = 0;           // pedido em voo (o botão fica "AGUARDE")
// toques
struct IsleTouch { int id = -1; float x = 0, y = 0, sx = 0, sy = 0; } islePtr[2];
int isleDrag = 0;                   // 1 = vista, 2 = peça, 3 = cartões
bool isleMoved = false; float isleScroll0 = 0, isleLastX = 0; double isleLastT = 0; float islePinch0 = 0, isleDist0 = 0;

struct IsleGeo { float u, barH, stripY, stripH, cardW, cardH, cardGap, closeX, closeY, closeR; Rect2D strip, btnRot, btnOk, btnCancel, btnDemol; };
IsleGeo isleGeo() const {
    IsleGeo G; G.u = std::min((float)h / 1080.0f, (float)w / 1900.0f);
    const float u = G.u;
    G.barH = 112.0f * u;
    G.stripH = 290.0f * u; G.stripY = (float)h - G.stripH;
    G.cardH = 250.0f * u; G.cardW = 236.0f * u; G.cardGap = 22.0f * u;
    G.strip = {24.0f * u, G.stripY + 20.0f * u, (float)w - 48.0f * u, G.cardH};
    G.closeR = 40.0f * u; G.closeX = (float)w - 58.0f * u; G.closeY = 56.0f * u;
    const float bw = 250.0f * u, bh = 92.0f * u, bx = (float)w - bw - 28.0f * u, by = G.stripY - bh - 26.0f * u;
    G.btnOk = {bx, by, bw, bh};
    G.btnRot = {bx - bw * 0.62f - 18.0f * u, by, bw * 0.62f, bh};
    G.btnCancel = {bx - bw * 1.24f - 36.0f * u, by, bw * 0.62f, bh};
    G.btnDemol = {bx, by, bw, bh};
    return G;
}
Rect2D isleCard(const IsleGeo& G, int k) const { return {G.strip.x + 8.0f * G.u + (float)k * (G.cardW + G.cardGap) - isleScroll, G.strip.y, G.cardW, G.cardH}; }
float isleMaxScroll(const IsleGeo& G) const { return std::max(0.0f, e0net::SkKindCount * (G.cardW + G.cardGap) + 16.0f * G.u - G.strip.w); }
void isleSay(const char* t, bool bad) { isleMsg = t; isleMsgT = 2.6f; isleMsgBad = bad; }

void openIsleMode() {
    using namespace e0net;
    if (!net.connected()) { notify("MELHORIAS DA ILHA: S\xD3 ONLINE"); return; }
    const PlayerState me = net.predicted();
    const int sl = terrain().slotAt(me.x, me.z);
    if (!islandConquerable(sl)) { notify("V\xC1 AT\xC9 UMA ILHA DOMINADA PELA SUA TRIPULA\xC7\xC3O"); return; }
    const uint32_t mine = myTeamId();
    if (!mine) { notify("ENTRE NUMA TRIPULA\xC7\xC3O PARA CONSTRUIR NAS ILHAS"); return; }
    if (net.isleOwner(sl).team != mine) { notify("ESSA ILHA N\xC3O \xC9 DA SUA TRIPULA\xC7\xC3O: DOMINE O MASTRO PRIMEIRO"); return; }
    float px, py, pz;
    if (!islePoint(sl, px, py, pz)) { notify("CARREGANDO A ILHA... TENTE DE NOVO"); return; }
    const World* W = net.island(sl);
    isleMode = true; isleSlot = sl;
    isleOx = W ? W->oxcm * 0.01f : px; isleOz = W ? W->ozcm * 0.01f : pz;
    isleCx = me.x; isleCz = me.z; isleDist = isleDistT = 62.0f;
    isleSel = -1; islePick = 0; isleGhostSet = false; isleDemolT = 0; isleWaitReq = 0;
    islePtr[0].id = islePtr[1].id = -1; isleDrag = 0;
    joyT.id = lookT.id = runPtr = collectPtr = actionPtr = -1; collectDown = false;
    renderer.requestStructThumbs();
}
void closeIsleMode() {
    if (!isleMode) return;
    isleMode = false; isleSlot = -1;
    renderer.setStructGhost(Renderer::StructGhost{});
    rig.snap();
}
// ponto do chão sob o pixel (marcha o raio da câmera até cruzar o terreno)
bool islePickGround(float px, float py, float& gx, float& gz) const {
    const Camera& c = camera;
    const float asp = h > 0 ? (float)w / (float)h : 1.0f, ty = std::tan(c.fovY * 0.5f);
    const float nx = 2.0f * px / (float)w - 1.0f, ny = 1.0f - 2.0f * py / (float)h;
    Vec3 d = normalize(c.forward() + c.rightVec() * (nx * ty * asp) + c.upVec() * (ny * ty));
    if (d.y > -0.01f) return false;
    float prev = 0.0f;
    for (float t = 2.0f; t < 900.0f; t += 1.5f) {
        const Vec3 p = c.position + d * t;
        const float gh = std::max(0.0f, e0net::terrain().heightAt(p.x, p.z));
        if (p.y <= gh) {   // refina entre prev e t
            float a = prev, b = t;
            for (int i = 0; i < 10; ++i) { const float m = (a + b) * 0.5f; const Vec3 q = c.position + d * m; if (q.y <= std::max(0.0f, e0net::terrain().heightAt(q.x, q.z))) b = m; else a = m; }
            const Vec3 q = c.position + d * b; gx = q.x; gz = q.z; return true;
        }
        prev = t;
    }
    return false;
}
Camera isleCamera(float dt) {
    isleDist += (isleDistT - isleDist) * std::min(1.0f, dt * 10.0f);
    const float pitch = -1.12f, gh = std::max(0.0f, e0net::terrain().heightAt(isleCx, isleCz));
    Camera c; c.yaw = 0.0f; c.pitch = pitch; c.roll = 0.0f; c.fovY = 0.95f; c.zNear = 0.5f; c.zFar = 1500.0f;
    c.position = {isleCx, gh - std::sin(pitch) * isleDist, isleCz + std::cos(pitch) * isleDist};
    return c;
}
void isleRefreshGhost() {
    Renderer::StructGhost g;
    if (isleMode && isleSel == e0net::SkWall) {   // MURALHA: prévia do anel inteiro
        isleGhostRes = e0net::wallCheckPlace(isleSlot);
        if (isleGhostRes == e0net::SrOk && !e0net::structHasCost(isleInvSlots(), isleSel)) isleGhostRes = e0net::SrCost;
        g.active = e0net::wallGeomOf(isleSlot) != nullptr; g.ok = isleGhostRes == e0net::SrOk; g.kind = (uint8_t)isleSel; g.slot = isleSlot;
    } else if (isleMode && isleSel >= 0 && isleGhostSet) {
        float px, py, pz; islePoint(isleSlot, px, py, pz);
        const e0net::World* W = net.island(isleSlot);
        float y = 0;
        isleGhostRes = W ? e0net::structCheckPlace(*W, isleSlot, (uint8_t)isleSel, isleRot, isleGx, isleGz, px, pz, y) : (uint8_t)e0net::SrNotHere;
        if (isleGhostRes == e0net::SrOk && !e0net::structHasCost(isleInvSlots(), isleSel)) isleGhostRes = e0net::SrCost;
        isleGy = isleGhostRes == e0net::SrOk || isleGhostRes == e0net::SrCost ? y : std::max(0.0f, e0net::terrain().heightAt(isleGx, isleGz));
        g.active = true; g.ok = isleGhostRes == e0net::SrOk; g.kind = (uint8_t)isleSel; g.rot = isleRot; g.pos = {isleGx, isleGy, isleGz}; g.slot = isleSlot;
    } else if (isleMode && islePick) g.selId = islePick;
    renderer.setStructGhost(g);
}
const e0net::InvSlot* isleInvSlots() const { return inv; }   // a mochila (ItemStack = e0net::InvSlot)
void islePlaceGhost(float gx, float gz) { isleGx = gx; isleGz = gz; isleGhostSet = true; }
uint16_t islePickStruct(float gx, float gz) const {   // estrutura sob o ponto do chão
    for (const e0net::IsleStruct& s : e0net::isleStructs(isleSlot)) {
        if (s.kind == e0net::SkWall) {   // MURALHA: qualquer trecho do anel, o portão ou o cais
            const e0net::IsleWall* W = e0net::isleWallOf(isleSlot);
            if (!W) continue;
            if (e0net::pierAt(*W, gx, gz) || std::hypot(gx - W->gx, gz - W->gz) < W->half + 1.5f) return s.id;
            const int n = (int)W->x.size();
            for (int i = 0; i < n; ++i) {
                float px = gx, pz = gz; const int j = (i + 1) % n;
                e0net::pushFromSeg(px, pz, W->x[i], W->z[i], W->x[j], W->z[j], 2.0f);
                if (px != gx || pz != gz) return s.id;
            }
            continue;
        }
        const e0net::StructDef& D = e0net::structDef(s.kind);
        float lx, lz; e0net::structToLocal(e0net::structYaw(s.rot), s.x, s.z, gx, gz, lx, lz);
        if (std::fabs(lx) < D.hx + 0.6f && std::fabs(lz) < D.hz + 0.6f) return s.id;
    }
    return 0;
}

void updateIsle(float dt) {
    for (NetClient::StructRes e; net.popStructRes(e); ) {
        if (e.req == isleWaitReq) isleWaitReq = 0;
        char t[96];
        if (e.result == e0net::SrOk) {
            std::snprintf(t, sizeof t, "%s %s", e.op == 2 ? "DEMOLIU" : "ERGUEU", e0net::structDef(e.kind).name);
            if (isleMode) { isleSay(t, false); if (e.op == 2) islePick = 0; } else notify(t);
        } else {
            std::snprintf(t, sizeof t, "N\xC3O DEU: %s", e0net::structResText(e.result));
            if (isleMode) isleSay(t, true); else notify(t);
        }
    }
    if (!isleMode) return;
    isleMsgT = std::max(0.0f, isleMsgT - dt);
    isleDemolT = std::max(0.0f, isleDemolT - dt);
    // a ilha deixou de ser nossa / saí dela / morri: fecha
    const e0net::PlayerState me = net.predicted();
    if (!net.connected() || selfDead || net.isleOwner(isleSlot).team != myTeamId() || e0net::terrain().slotAt(me.x, me.z) != isleSlot) {
        closeIsleMode(); notify("MELHORIAS DA ILHA FECHADAS"); return;
    }
    if (islePick) { bool found = false; for (const auto& s : e0net::isleStructs(isleSlot)) if (s.id == islePick) found = true; if (!found) islePick = 0; }
    // rolagem dos cartões com inércia
    const IsleGeo G = isleGeo();
    if (isleDrag != 3) {
        isleScroll += isleVel * dt; isleVel *= std::exp(-dt * 4.0f);
        const float mx = isleMaxScroll(G);
        if (isleScroll < 0) { isleScroll += (0 - isleScroll) * std::min(1.0f, dt * 12.0f); isleVel = 0; }
        if (isleScroll > mx) { isleScroll += (mx - isleScroll) * std::min(1.0f, dt * 12.0f); isleVel = 0; }
    }
    isleRefreshGhost();
}

void isleTouchDown(int pid, float x, float y) {
    const IsleGeo G = isleGeo();
    int k = islePtr[0].id < 0 ? 0 : (islePtr[1].id < 0 ? 1 : -1);
    if (k < 0) return;
    islePtr[k] = {pid, x, y, x, y};
    if (k == 1) {   // 2º dedo: zoom (pinça) na vista
        if (isleDrag == 1 || isleDrag == 0) { isleDrag = 1; islePinch0 = std::hypot(islePtr[1].x - islePtr[0].x, islePtr[1].y - islePtr[0].y); isleDist0 = isleDistT; isleMoved = true; }
        return;
    }
    isleMoved = false;
    if (std::hypot(x - G.closeX, y - G.closeY) < G.closeR * 1.3f) { isleDrag = 4; return; }
    if (y >= G.stripY) { isleDrag = 3; isleScroll0 = isleScroll; isleLastX = x; isleLastT = teamNow(); isleVel = 0; return; }
    const bool placing = isleSel >= 0;
    if ((placing && (G.btnOk.contains(x, y) || G.btnRot.contains(x, y) || G.btnCancel.contains(x, y))) || (!placing && islePick && G.btnDemol.contains(x, y))) { isleDrag = 5; return; }
    isleDrag = 1;
    if (placing && isleGhostSet) {   // começou em cima da peça: arrasta a peça
        float sx, sy;
        if (project({isleGx, isleGy + 1.0f, isleGz}, sx, sy) && std::hypot(sx - x, sy - y) < 150.0f * G.u) isleDrag = 2;
    }
}
void isleTouchMove(int pid, float x, float y) {
    int k = islePtr[0].id == pid ? 0 : (islePtr[1].id == pid ? 1 : -1);
    if (k < 0) return;
    const float px = islePtr[k].x, py = islePtr[k].y;
    islePtr[k].x = x; islePtr[k].y = y;
    const IsleGeo G = isleGeo();
    if (std::hypot(x - islePtr[k].sx, y - islePtr[k].sy) > 14.0f * G.u) isleMoved = true;
    if (islePtr[0].id >= 0 && islePtr[1].id >= 0) {   // pinça
        const float d = std::hypot(islePtr[1].x - islePtr[0].x, islePtr[1].y - islePtr[0].y);
        if (islePinch0 > 10.0f) isleDistT = std::max(22.0f, std::min(320.0f, isleDist0 * islePinch0 / std::max(10.0f, d)));
        return;
    }
    if (isleDrag == 3) {   // cartões
        const double now = teamNow(); const float dtm = (float)std::max(1e-3, now - isleLastT);
        isleScroll = isleScroll0 - (x - islePtr[k].sx);
        isleVel = 0.7f * isleVel + 0.3f * (-(x - isleLastX) / dtm);
        isleLastX = x; isleLastT = now;
    } else if (isleDrag == 1 && isleMoved) {   // arrasta a vista: o chão acompanha o dedo
        const float mpp = isleDist * std::tan(camera.fovY * 0.5f) * 2.0f / (float)h;
        isleCx -= (x - px) * mpp; isleCz -= (y - py) * mpp * 1.15f;
        const float dx = isleCx - isleOx, dz = isleCz - isleOz, d = std::hypot(dx, dz), lim = e0net::World::kHalf;
        if (d > lim) { isleCx = isleOx + dx / d * lim; isleCz = isleOz + dz / d * lim; }
    } else if (isleDrag == 2) {   // arrasta a peça
        float gx, gz; if (islePickGround(x, y, gx, gz)) islePlaceGhost(gx, gz);
    }
}
void isleTouchUp(int pid, float x, float y, bool cancel) {
    int k = islePtr[0].id == pid ? 0 : (islePtr[1].id == pid ? 1 : -1);
    if (k < 0) return;
    islePtr[k].id = -1;
    if (k == 1 || islePtr[1].id >= 0) { if (k == 0) { islePtr[0] = islePtr[1]; islePtr[1].id = -1; } isleMoved = true; return; }
    const int drag = isleDrag; isleDrag = 0;
    if (cancel) return;
    const IsleGeo G = isleGeo();
    if (drag == 4) { if (std::hypot(x - G.closeX, y - G.closeY) < G.closeR * 1.3f) closeIsleMode(); return; }
    if (drag == 3) {   // toque num cartão (sem arrastar) escolhe
        if (isleMoved) return;
        for (int c = 0; c < e0net::SkKindCount; ++c) if (isleCard(G, c).contains(x, y)) {
            if (isleSel == c) { isleSel = -1; isleGhostSet = false; return; }
            isleSel = c; islePick = 0;
            if (c == e0net::SkWall) {   // a muralha é a ilha inteira: afasta a vista para mostrar o anel
                if (const e0net::IsleWall* W = e0net::wallGeomOf(isleSlot)) {
                    isleCx = (W->x0 + W->x1) * 0.5f; isleCz = (W->z0 + W->z1) * 0.5f;
                    isleDistT = std::max(60.0f, std::min(320.0f, std::max(W->x1 - W->x0, W->z1 - W->z0) * 1.05f));
                }
            } else if (!isleGhostSet) { float gx, gz; if (islePickGround((float)w * 0.5f, (float)h * 0.42f, gx, gz)) islePlaceGhost(gx, gz); }
            else islePlaceGhost(isleGx, isleGz);
            const e0net::StructDef& D = e0net::structDef(c);
            isleSay(D.desc, false);
            return;
        }
        return;
    }
    if (drag == 5) {   // botões
        if (isleSel >= 0) {
            if (G.btnCancel.contains(x, y)) { isleSel = -1; isleGhostSet = false; return; }
            if (G.btnRot.contains(x, y) && isleSel != e0net::SkWall) { isleRot = (uint8_t)((isleRot + 1) & 7); islePlaceGhost(isleGx, isleGz); return; }
            if (G.btnOk.contains(x, y)) {
                if (isleWaitReq) return;
                isleRefreshGhost();
                if (isleSel == e0net::SkWall) {
                    if (isleGhostRes != e0net::SrOk) { isleSay(e0net::structResText(isleGhostRes), true); return; }
                    isleWaitReq = net.requestStruct(1, isleSlot, (uint8_t)isleSel, 0, 0.0f, 0.0f, 0);
                    return;
                }
                if (!isleGhostSet) { isleSay("TOQUE NO CH\xC3O PARA POSICIONAR", true); return; }
                if (isleGhostRes != e0net::SrOk) { isleSay(e0net::structResText(isleGhostRes), true); return; }
                isleWaitReq = net.requestStruct(1, isleSlot, (uint8_t)isleSel, isleRot, isleGx, isleGz, 0);
                return;
            }
        } else if (islePick && G.btnDemol.contains(x, y)) {
            if (isleDemolT <= 0.0f) { isleDemolT = 3.0f; isleSay("TOQUE DE NOVO PARA DEMOLIR (DEVOLVE METADE)", true); return; }
            isleDemolT = 0; isleWaitReq = net.requestStruct(2, isleSlot, 0, 0, 0, 0, islePick);
        }
        return;
    }
    if (drag == 2) return;
    if (drag == 1 && !isleMoved) {   // toque no chão
        float gx, gz;
        if (!islePickGround(x, y, gx, gz)) return;
        if (isleSel >= 0) { if (isleSel != e0net::SkWall) islePlaceGhost(gx, gz); }
        else { islePick = islePickStruct(gx, gz); isleDemolT = 0; }
    }
}

// ---- desenho ----
void drawIsleScreen() {
    using namespace e0net;
    const IsleGeo G = isleGeo();
    const float u = G.u;
    const World* W = net.island(isleSlot);
    const auto& list = isleStructs(isleSlot);
    const int t5 = std::max(3, (int)std::lround(5.0f * u)), t4 = std::max(2, (int)std::lround(3.8f * u)), t3 = std::max(2, (int)std::lround(3.0f * u)), t25 = std::max(2, (int)std::lround(2.5f * u));
    // formas
    renderer.beginUi();
    renderer.uiGrad((float)w * 0.5f, G.barH * 0.5f, (float)w * 0.5f, G.barH * 0.5f, 0.0f, 0.04f, 0.03f, 0.02f, 0.86f, 0.04f, 0.03f, 0.02f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    renderer.uiGrad((float)w * 0.5f, G.stripY + G.stripH * 0.5f, (float)w * 0.5f, G.stripH * 0.5f, 0.0f, 0.04f, 0.03f, 0.02f, 0.0f, 0.04f, 0.03f, 0.02f, 0.92f, 0.0f, 1.0f, 0.0f, 0.0f);
    renderer.uiIconTint(HiBuild, 66.0f * u, 56.0f * u, 34.0f * u, theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
    closeBtnShapes(G.closeX, G.closeY, G.closeR);
    // marca do mastro (não dá para construir perto)
    { float px, py, pz, sx, sy; if (islePoint(isleSlot, px, py, pz) && project({px, py + 0.3f, pz}, sx, sy)) renderer.uiShape(Renderer::UiRing, sx, sy, 22.0f * u, 1.0f, 0.85f, 0.35f, 0.7f); }
    // botões de ação
    auto btn = [&](const Rect2D& r, float cr, float cg, float cb, float a) {
        renderer.uiRoundRect(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f + 3.0f * u, r.h * 0.5f + 3.0f * u, 14.0f * u, theme::kBrass.r, theme::kBrass.g, theme::kBrass.b, 0.85f * a);
        renderer.uiRoundRect(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, r.h * 0.5f, 12.0f * u, cr, cg, cb, 0.96f * a);
    };
    if (isleSel >= 0) {
        const bool ok = isleGhostSet && isleGhostRes == SrOk && !isleWaitReq;
        const bool okW = isleSel == SkWall ? isleGhostRes == SrOk && !isleWaitReq : ok;
        btn(G.btnCancel, 0.22f, 0.08f, 0.07f, 1.0f);
        if (isleSel != SkWall) btn(G.btnRot, 0.20f, 0.15f, 0.09f, 1.0f);
        btn(G.btnOk, okW ? 0.16f : 0.20f, okW ? 0.42f : 0.17f, okW ? 0.20f : 0.14f, 1.0f);
    } else if (islePick) btn(G.btnDemol, isleDemolT > 0 ? 0.62f : 0.36f, 0.10f, 0.08f, 1.0f);
    renderer.endUi();
    // cartões (recortados na faixa de baixo)
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)G.strip.x, (GLint)((float)h - G.strip.y - G.strip.h - 4.0f), (GLsizei)G.strip.w, (GLsizei)(G.strip.h + 8.0f));
    const e0net::InvSlot* myInv = isleInvSlots();
    renderer.beginUi();
    for (int k = 0; k < SkKindCount; ++k) {
        const Rect2D c = isleCard(G, k);
        if (c.x + c.w < G.strip.x || c.x > G.strip.x + G.strip.w) continue;
        const bool sel = isleSel == k, can = structHasCost(myInv, k);
        renderer.uiRoundRect(c.x + c.w * 0.5f, c.y + c.h * 0.5f, c.w * 0.5f + 3.0f * u, c.h * 0.5f + 3.0f * u, 16.0f * u, sel ? 1.0f : theme::kBrass.r, sel ? 0.80f : theme::kBrass.g, sel ? 0.30f : theme::kBrass.b, sel ? 1.0f : 0.55f);
        renderer.uiGrad(c.x + c.w * 0.5f, c.y + c.h * 0.5f, c.w * 0.5f, c.h * 0.5f, 14.0f * u, 0.20f, 0.15f, 0.10f, 0.97f, 0.07f, 0.05f, 0.04f, 0.97f, 0.0f, 1.0f, 0.25f, (float)k);
        renderer.uiGlow(c.x + c.w * 0.5f, c.y + 78.0f * u, c.w * 0.45f, 70.0f * u, 0.95f, 0.75f, 0.40f, sel ? 0.22f : 0.10f);
        renderer.uiTexture(renderer.structThumb(k), c.x + c.w * 0.5f, c.y + 80.0f * u, 100.0f * u, 75.0f * u, can ? 1.0f : 0.55f);
        const StructDef& D = structDef(k);   // ícones dos materiais
        float ix = c.x + 26.0f * u;
        for (int m = 0; m < 3; ++m) if (D.cost[m]) { drawItemIcon(ix, c.y + c.h - 32.0f * u, 17.0f * u, D.cost[m], 1.0f); ix += 70.0f * u; }
    }
    renderer.endUi();
    renderer.beginText();
    for (int k = 0; k < SkKindCount; ++k) {
        const Rect2D c = isleCard(G, k);
        if (c.x + c.w < G.strip.x || c.x > G.strip.x + G.strip.w) continue;
        const StructDef& D = structDef(k);
        int ts = t3; while (ts > 2 && Renderer::textWidth(D.name, ts) > c.w - 16.0f * u) --ts;
        renderer.text(c.x + (c.w - Renderer::textWidth(D.name, ts)) * 0.5f, c.y + 162.0f * u, ts, D.name, 1.0f, 0.86f, 0.42f, 1.0f, false);
        char lim[24]; std::snprintf(lim, sizeof lim, "%d/%d", structCountKind(isleSlot, k), D.maxPer);
        renderer.text(c.x + c.w - Renderer::textWidth(lim, t25) - 12.0f * u, c.y + 12.0f * u, t25, lim, 0.80f, 0.76f, 0.66f, 0.9f, false);
        float ix = c.x + 26.0f * u;
        for (int m = 0; m < 3; ++m) if (D.cost[m]) {
            char q[12]; std::snprintf(q, sizeof q, "%d", D.costN[m]);
            const bool have = invCount(D.cost[m]) >= D.costN[m];
            renderer.text(ix + 20.0f * u, c.y + c.h - 32.0f * u - 3.5f * t25, t25, q, have ? 0.55f : 1.0f, have ? 0.92f : 0.40f, have ? 0.55f : 0.36f, 1.0f, false);
            ix += 70.0f * u;
        }
    }
    renderer.endText();
    glDisable(GL_SCISSOR_TEST);
    // barra de rolagem
    renderer.beginUi();
    const float maxS = isleMaxScroll(G);
    if (maxS > 1.0f) {
        const float len = G.strip.w, view = len / (len + maxS), pos = std::max(0.0f, std::min(1.0f, isleScroll / maxS));
        const float bl = std::max(60.0f * u, len * view), off = (len - bl) * pos, by = (float)h - 12.0f * u;
        renderer.uiRoundRect(G.strip.x + G.strip.w * 0.5f, by, G.strip.w * 0.5f, 3.0f * u, 3.0f * u, 1.0f, 1.0f, 1.0f, 0.10f);
        renderer.uiRoundRect(G.strip.x + off + bl * 0.5f, by, bl * 0.5f, 3.0f * u, 3.0f * u, 1.0f, 0.78f, 0.25f, 0.85f);
    }
    if (isleMsgT > 0.0f) {   // aviso no meio, acima dos botões
        int tsm = t3; while (tsm > 2 && Renderer::textWidth(isleMsg.c_str(), tsm) > (float)w - 160.0f * u) --tsm;
        const float a = std::min(1.0f, isleMsgT * 3.0f), tw = Renderer::textWidth(isleMsg.c_str(), tsm) + 60.0f * u;
        renderer.uiRoundRect((float)w * 0.5f, G.barH + 52.0f * u, tw * 0.5f, 30.0f * u, 30.0f * u, isleMsgBad ? 0.30f : 0.06f, isleMsgBad ? 0.07f : 0.05f, isleMsgBad ? 0.06f : 0.03f, 0.94f * a);
    }
    renderer.endUi();
    // textos da barra de cima, dos botões e o aviso
    renderer.beginText();
    {
        const std::string nm = islandLabel(isleSlot, W ? W->kind : (uint8_t)IslandBig);
        shopBold(116.0f * u, 22.0f * u, t5, "MELHORIAS DA ILHA", theme::kBrassBright.r, theme::kBrassBright.g, theme::kBrassBright.b, 1.0f);
        renderer.text(116.0f * u, 22.0f * u + 7.0f * t5 + 10.0f * u, t3, nm.c_str(), theme::kCreamSoft.r, theme::kCreamSoft.g, theme::kCreamSoft.b, 0.95f, false);
        char info[128];
        const int inc = islandIncome(W ? W->kind : (uint8_t)IslandBig) + structIncome(isleSlot);
        std::snprintf(info, sizeof info, "ESTRUTURAS %d/%d   RENDA %d MOEDAS / 20 MIN   TORRES %d", (int)list.size(), kIsleMaxStructs, inc, structTowers(isleSlot));
        {   // cabe entre o título e o X
            const float x0 = 116.0f * u + Renderer::textWidth("MELHORIAS DA ILHA", t5) + 40.0f * u, x1 = G.closeX - G.closeR - 30.0f * u;
            int ts = t3; while (ts > 2 && Renderer::textWidth(info, ts) > x1 - x0) --ts;
            if (Renderer::textWidth(info, ts) > x1 - x0) std::snprintf(info, sizeof info, "%d/%d  |  +%d / 20 MIN", (int)list.size(), kIsleMaxStructs, inc);
            renderer.text(x1 - Renderer::textWidth(info, ts), 40.0f * u, ts, info, 0.92f, 0.88f, 0.78f, 1.0f, false);
        }
        const bool bad = isleSel >= 0 && (isleSel == SkWall || isleGhostSet) && isleGhostRes != SrOk;
        const char* hint = bad ? structResText(isleGhostRes)
                         : isleSel == SkWall ? "A MURALHA CERCA A ILHA TODA, COM PORT\xC3O E CAIS (O NAVIO CHAMADO AQUI ATRACA NO CAIS)"
                         : isleSel >= 0 ? "TOQUE NO CH\xC3O OU ARRASTE A PE\xC7" "A  |  1 DEDO MOVE A VISTA, 2 DEDOS D\xC3O ZOOM" : "ESCOLHA O QUE ERGUER NOS CART\xD5" "ES  |  TOQUE NUMA ESTRUTURA PARA DEMOLIR";
        {
            const float maxW = (isleSel >= 0 || islePick ? G.btnCancel.x : (float)w) - 56.0f * u;
            int ts = t3; while (ts > 2 && Renderer::textWidth(hint, ts) > maxW) --ts;
            renderer.text(28.0f * u, G.stripY - 14.0f * u - 7.0f * ts, ts, hint, bad ? 1.0f : 0.85f, bad ? 0.55f : 0.82f, bad ? 0.48f : 0.72f, 0.95f, false);
        }
        auto btnText = [&](const Rect2D& r, const char* tx, float a) {   // cabe no botão: diminui e, se precisar, quebra em 2 linhas
            const float maxW = r.w - 22.0f * u;
            int ts = t4; while (ts > 2 && Renderer::textWidth(tx, ts) > maxW) --ts;
            if (Renderer::textWidth(tx, ts) <= maxW) { renderer.text(r.x + (r.w - Renderer::textWidth(tx, ts)) * 0.5f, r.y + (r.h - 7.0f * ts) * 0.5f, ts, tx, theme::kCream.r, theme::kCream.g, theme::kCream.b, a, false); return; }
            const std::string s2 = tx; size_t cut = std::string::npos, mid = s2.size() / 2;
            for (size_t k = 0; k < s2.size(); ++k) if (s2[k] == ' ' && (cut == std::string::npos || (k > mid ? k - mid : mid - k) < (cut > mid ? cut - mid : mid - cut))) cut = k;
            if (cut == std::string::npos) cut = mid;
            const std::string l1 = s2.substr(0, cut), l2 = s2.substr(cut + (s2[cut] == ' ' ? 1 : 0));
            ts = t4; while (ts > 2 && std::max(Renderer::textWidth(l1.c_str(), ts), Renderer::textWidth(l2.c_str(), ts)) > maxW) --ts;
            const float lh = 7.0f * ts + 6.0f * u, y0 = r.y + (r.h - lh - 7.0f * ts) * 0.5f;
            renderer.text(r.x + (r.w - Renderer::textWidth(l1.c_str(), ts)) * 0.5f, y0, ts, l1.c_str(), theme::kCream.r, theme::kCream.g, theme::kCream.b, a, false);
            renderer.text(r.x + (r.w - Renderer::textWidth(l2.c_str(), ts)) * 0.5f, y0 + lh, ts, l2.c_str(), theme::kCream.r, theme::kCream.g, theme::kCream.b, a, false);
        };
        if (isleSel >= 0) {
            btnText(G.btnCancel, "CANCELAR", 1.0f);
            if (isleSel != SkWall) { char gr[24]; std::snprintf(gr, sizeof gr, "GIRAR %d", (int)isleRot * 45); btnText(G.btnRot, gr, 1.0f); }
            const bool wallSel = isleSel == SkWall, can = isleGhostRes == SrOk && (wallSel || isleGhostSet);
            btnText(G.btnOk, isleWaitReq ? "AGUARDE..." : (can ? (wallSel ? "ERGUER MURALHA" : "CONSTRUIR") : (!wallSel && !isleGhostSet ? "POSICIONE" : "N\xC3O PODE")), 1.0f);
        } else if (islePick) {
            for (const IsleStruct& s : list) if (s.id == islePick) {
                char tx[64]; std::snprintf(tx, sizeof tx, "%s %s", isleDemolT > 0 ? "CONFIRMAR" : "DEMOLIR", structDef(s.kind).name);
                btnText(G.btnDemol, tx, 1.0f);
            }
        }
        if (isleMsgT > 0.0f) {
            const float a = std::min(1.0f, isleMsgT * 3.0f);
            int tsm = t3; while (tsm > 2 && Renderer::textWidth(isleMsg.c_str(), tsm) > (float)w - 160.0f * u) --tsm;
            renderer.text((float)w * 0.5f - Renderer::textWidth(isleMsg.c_str(), tsm) * 0.5f, G.barH + 52.0f * u - 3.5f * tsm, tsm, isleMsg.c_str(), 1.0f, isleMsgBad ? 0.75f : 0.92f, isleMsgBad ? 0.65f : 0.72f, a, false);
        }
    }
    renderer.endText();
    // ícones (fila do passe de texto)
    if (!hudIcons.empty()) { renderer.beginUi(); for (const HudIconReq& q : hudIcons) if (q.idx >= 0) renderer.uiIconTint(q.idx, q.x, q.y, q.half, q.r, q.g, q.b, q.a); renderer.endUi(); hudIcons.clear(); }
}
