// Isolated - MODO CINEMA: controlador. Incluído DENTRO de struct Engine, só em builds com E0_CINEMA=1 (E0_CINEMA=1 ./build.sh). Linguagem dos roteiros em cinema.h / CLAUDE.md.
// Roteiros ficam em <externalDataPath>/cinema/*.cine; tools/cinema.py os envia, escreve o comando em cinema/cmd.txt e traz os vídeos de cinema/out/.
cine::Script cs; bool cRun = false, cRec = false, cHideHud = false, cPanel = false; int cShot = 0; float cT = 0, cFog = 1, cSlow = 1, cPoll = 0, cAll = 0;
long long cFrame = 0; double cClock = 0; int cSaveW = 0, cSaveH = 0, cSel = 0; CinemaRecorder cRecd; std::string cDir, cMsg = "PARADO", cWant; bool cWantRec = false;
std::vector<std::string> cFiles; std::vector<cine::Crew> cCrew; Vec3 cFocus{}; 
struct CHit { float t; Vec3 p; int kind; }; std::vector<CHit> cHits;
bool cShipCtl[e0net::kMaxShips] = {}; float cShipSpd[e0net::kMaxShips] = {}, cShipStr[e0net::kMaxShips] = {};

void cineInit() {
    const char* d = app->activity->externalDataPath;
    if (!d) { LOG("cinema: sem externalDataPath"); return; }
    cDir = std::string(d) + "/cinema";
    mkdir(cDir.c_str(), 0775); mkdir((cDir + "/out").c_str(), 0775);
    cineScan();
    cineStatus("parado");
}
void cineScan() {
    cFiles.clear();
    if (DIR* dd = opendir(cDir.c_str())) {
        while (dirent* e = readdir(dd)) { std::string n = e->d_name; if (n.size() > 5 && n.compare(n.size() - 5, 5, ".cine") == 0) cFiles.push_back(n); }
        closedir(dd);
    }
    std::sort(cFiles.begin(), cFiles.end());
    if (cSel >= (int)cFiles.size()) cSel = 0;
}
void cineStatus(const char* st) {   // tools/cinema.py acompanha por este arquivo
    FILE* f = std::fopen((cDir + "/status.txt").c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%s %lld %lld %s\n", st, cFrame, (long long)(cs.total() * (float)cs.fps), cMsg.c_str());
    std::fclose(f);
}
// Comandos de cinema/cmd.txt: "gravar arquivo.cine" | "previa arquivo.cine" | "parar"
void cinePoll(float dt) {
    if (cDir.empty()) return;
    if (!cWant.empty() && net.worldReady() && e0net::shipState(0).exists) { std::string n = cWant; cWant.clear(); cineStart(n, cWantRec); }
    if ((cPoll -= dt) > 0.0f) return;
    cPoll = 0.4f;
    const std::string p = cDir + "/cmd.txt";
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) return;
    char a[32] = {0}, b[160] = {0};
    int n = std::fscanf(f, "%31s %159s", a, b);
    std::fclose(f); std::remove(p.c_str());
    cineScan();
    if (n >= 1 && !std::strcmp(a, "parar")) { cineStop("parado pelo comando"); return; }
    if (n >= 2 && (!std::strcmp(a, "gravar") || !std::strcmp(a, "previa"))) { cWant = b; cWantRec = !std::strcmp(a, "gravar"); cMsg = "aguardando_mundo"; cineStatus("aguardando"); }
}
void cineStart(const std::string& file, bool rec) {
    if (cRun) return;
    FILE* f = std::fopen((cDir + "/" + file).c_str(), "rb");
    if (!f) { cMsg = "sem_arquivo_" + file; cineStatus("erro"); return; }
    std::string text; char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    cs = cine::Script{}; std::string err;
    if (!cine::parse(text, cs, err)) { LOG("cinema: %s: %s", file.c_str(), err.c_str()); cMsg = err; for (char& c : cMsg) if (c == ' ') c = '_'; cineStatus("erro"); return; }
    cShot = 0; cT = 0; cFrame = 0; cClock = 0; cAll = 0; cRec = rec; cCrew.clear(); cHits.clear();
    for (bool& c : cShipCtl) c = false;
    if (rec) {
        const std::string full = cDir + "/out/" + cs.name + ".mp4";
        if (!cRecd.start(dpy, cfg, ctx, cs.w, cs.h, cs.fps, cs.mbps, full)) { cMsg = "encoder_falhou"; cineStatus("erro"); eglMakeCurrent(dpy, surf, surf, ctx); return; }
        cSaveW = w; cSaveH = h; w = cs.w; h = cs.h;
        renderer.resize(w, h);
        audio.pause();
    }
    renderer.setCinema(true);
    cRun = true; cPanel = false; cMsg = cs.name;
    cineEnterShot(0);
    cineStatus(rec ? "gravando" : "previa");
    LOG("cinema: %s %s, %zu planos, %.1f s", rec ? "gravando" : "previa", cs.name.c_str(), cs.shots.size(), cs.total());
}
void cineStop(const char* why) {
    if (!cRun) return;
    cRun = false;
    if (cRec) {
        cRecd.finish();
        eglMakeCurrent(dpy, surf, surf, ctx);
        w = cSaveW; h = cSaveH;
        renderer.resize(w, h);
        audio.resume();
    }
    renderer.setCinema(false); renderer.setClock(-1.0);
    renderer.setFogScale(1.0f);
    for (bool& c : cShipCtl) c = false;
    cMsg = why; for (char& c : cMsg) if (c == ' ') c = '_';
    cineStatus("pronto");
    LOG("cinema: %s", why);
}
static std::string cineSafe(const std::string& s) { std::string o; for (char c : s) o += (std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '_'); return o; }
void cineEnterShot(int i) {
    cShot = i; cT = 0;
    const cine::Shot& H = cs.shots[(size_t)i];
    if (H.hour >= 0.0f) net.requestTime(H.hour);
    cFog = H.fog; cSlow = H.slow;
    if (!H.crew.empty()) cCrew = H.crew;
    for (int k = 0; k < e0net::kMaxShips; ++k) {
        const cine::ShipSet& s = H.ships[k];
        if (!s.set) continue;
        e0net::ShipState& S = e0net::shipState(k);
        if (s.hide) { S.exists = false; cShipCtl[k] = false; continue; }
        S.exists = true; S.x = s.x; S.z = s.z; S.yaw = s.yaw; S.helmer = -1; ++S.gen;
        cShipCtl[k] = true; cShipSpd[k] = s.speed; cShipStr[k] = s.steer;
    }
    if (cRec) {
        char nm[200]; std::snprintf(nm, sizeof nm, "%s/out/%s_%02d_%s.mp4", cDir.c_str(), cs.name.c_str(), i + 1, cineSafe(H.name).c_str());
        cRecd.nextClip(nm, cFrame * 1000000000LL / cs.fps);
    }
    LOG("cinema: plano %d/%zu %s (%.1f s)", i + 1, cs.shots.size(), H.name.c_str(), H.dur);
}
Vec3 cineToWorld(int space, const float* v) const {   // altura sempre por último: (a, b, y) no navio, (x, z, y) no mundo
    if (space >= 0 && space < e0net::kMaxShips) { float x, z; e0net::shipLocalToWorld(e0net::shipState(space), v[0], v[1], x, z); return {x, v[2], z}; }
    return {v[0], v[2], v[1]};
}
void cineCamera(const cine::Shot& H) {
    const cine::Eval e = cine::evalShot(H, cT);
    const int ls = H.lookSpace == -2 ? H.camSpace : H.lookSpace;
    Vec3 p = cineToWorld(H.camSpace, e.p), l = cineToWorld(ls, e.l), d = l - p;
    float amp = cine::shakeAmp(H, cT);
    if (amp > 0.0f) {
        float t = cT * 7.0f;
        p = p + Vec3{(std::sin(t * 2.3f) + std::sin(t * 3.7f + 1.0f)) * 0.05f, std::sin(t * 2.9f + 2.0f) * 0.09f, (std::sin(t * 3.1f + 4.0f) + std::sin(t * 1.9f)) * 0.05f} * amp;
    }
    const float len = std::max(1e-3f, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z)), asp = (float)w / (float)h;
    camera.position = p; camera.yaw = std::atan2(d.x, -d.z); camera.pitch = std::asin(d.y / len);
    camera.roll = e.roll * 0.01745329f + amp * 0.012f * std::sin(cT * 17.0f);
    camera.fovY = 2.0f * std::atan(std::tan(e.fov * 0.5f * 0.01745329f) / asp);
    camera.zNear = 0.15f; camera.zFar = 2600.0f;
    if (H.focusSet) { cFocus = {H.focus[0], 0.0f, H.focus[1]}; }
    else if (H.focusShip >= 0) { const auto& S = e0net::shipState(H.focusShip); cFocus = {S.x, 0.0f, S.z}; }
    else if (ls >= 0) { const auto& S = e0net::shipState(ls); cFocus = {S.x, 0.0f, S.z}; }
    else cFocus = {l.x, 0.0f, l.z};
}
void cineFire(cine::Fire& f) {   // bordada: as balas saem em sequência, ao longo do costado
    const e0net::ShipState& S = e0net::shipState(f.ship); const e0net::ShipDef& D = e0net::shipDef(f.ship);
    if (!S.exists) return;
    for (int side = -1; side <= 1; side += 2) {
        if (f.side != 0 && f.side != side) continue;
        for (int k = 0; k < f.n; ++k) {
            float a = f.n > 1 ? -D.muzzleA * 1.5f + 3.0f * D.muzzleA * (float)k / (float)(f.n - 1) : 0.0f, delay = 0.13f * (float)k;
            float sx, sz, tx, tz;
            e0net::shipLocalToWorld(S, a, (float)side * (D.hullHalfB + 0.9f), sx, sz);
            e0net::shipLocalToWorld(S, a + 6.0f * ((float)k - f.n * 0.5f), (float)side * (D.hullHalfB + f.dist), tx, tz);
            const float fl = e0net::cannonFlightTime(f.dist);
            cHits.push_back({cAll + delay, {sx, D.deckY + 0.2f, sz}, 2});
            cHits.push_back({cAll + delay + fl, {tx, 0.0f, tz}, 0});
            cballs.push_back({cannonClock + delay, fl, {sx, D.deckY + 0.2f, sz}, {tx, 0.0f, tz}});
        }
    }
}
void cineOverlay(const cine::Shot& H) {
    renderer.beginText();
    if (H.tint[3] > 0.0f) renderer.rect(0, 0, (float)w, (float)h, H.tint[0], H.tint[1], H.tint[2], H.tint[3]);
    if (H.bars > 0.0f) {
        float bh = ((float)h - (float)w / H.bars) * 0.5f;
        if (bh > 0.0f) { renderer.rect(0, 0, (float)w, bh, 0, 0, 0, 1); renderer.rect(0, (float)h - bh, (float)w, bh, 0, 0, 0, 1); }
    }
    for (const cine::Title& t : H.titles) if (cT >= t.t0 && cT <= t.t1) {
        float a = std::min(1.0f, std::min((cT - t.t0) / 0.5f, (t.t1 - cT) / 0.5f));
        int sc = std::max(3, h / 150);
        renderer.text((float)w * 0.5f - Renderer::textWidth(t.text.c_str(), sc) * 0.5f, (float)h * 0.76f, sc, t.text.c_str(), 1, 1, 1, a);
    }
    float fa = cine::fadeAt(H, cT);
    if (fa > 0.0f) { float c = H.white ? 1.0f : 0.0f; renderer.rect(0, 0, (float)w, (float)h, c, c, c, fa); }
    renderer.endText();
}
// Quadro completo do modo cinema (true = cuidou de tudo, inclusive do swap)
bool cineFrame(float rawDt) {
    if (!cRun) return false;
    const float vdt = cRec ? 1.0f / (float)cs.fps : std::min(rawDt, 0.05f), wdt = vdt * cSlow;
    cine::Shot& H = cs.shots[(size_t)cShot];
    for (int k = 0; k < e0net::kMaxShips; ++k) if (cShipCtl[k]) {
        e0net::ShipState& S = e0net::shipState(k);
        S.speed = cShipSpd[k]; S.steer = S.cmdSteer = cShipStr[k]; S.cmdThrottle = 0;
    }
    update(wdt);
    cClock += (double)wdt; renderer.setClock(cClock);
    for (cine::Fire& f : H.fires) if (!f.done && cT >= f.t) { f.done = true; cineFire(f); }
    for (size_t i = 0; i < cHits.size();) { if (cHits[i].t <= cAll) { renderer.burst(cHits[i].p, cHits[i].kind); cHits.erase(cHits.begin() + (long)i); } else ++i; }
    cineCamera(H);
    drawWorld();
    cineOverlay(H);
    if (cRec) cRecd.present(cFrame * 1000000000LL / cs.fps); else eglSwapBuffers(dpy, surf);
    cT += vdt; cAll += wdt; ++cFrame;
    if (cRec && cFrame % 60 == 0) { LOG("cinema: quadro %lld", cFrame); cineStatus("gravando"); }
    if (cT >= H.dur) {
        if (cShot + 1 < (int)cs.shots.size()) cineEnterShot(cShot + 1);
        else cineStop("concluido");
    }
    return true;
}

// ---- painel (só aparece no build de cinema) ----
Rect2D cineBtn() const { float u = (float)uiScale(); return {24.0f, h * 0.40f, 92.0f * u, 26.0f * u}; }
Rect2D cineRow(int i) const { float u = (float)uiScale(), rw = 300.0f * u, rh = 30.0f * u; return {w * 0.5f - rw * 0.5f, h * 0.20f + (float)i * (rh + 6.0f * u), rw, rh}; }
bool cineTouch(float x, float y) {
    if (cRun) { if (!cRec) cineStop("previa interrompida"); return true; }
    if (cHideHud) { cHideHud = false; return true; }   // HUD escondida: qualquer toque volta
    if (cineBtn().contains(x, y)) { cPanel = !cPanel; if (cPanel) cineScan(); return true; }
    if (!cPanel) return false;
    if (cineRow(0).contains(x, y)) { if (!cFiles.empty()) cSel = (cSel + 1) % (int)cFiles.size(); return true; }
    if (!cFiles.empty() && cineRow(1).contains(x, y)) { cWant = cFiles[(size_t)cSel]; cWantRec = false; return true; }
    if (!cFiles.empty() && cineRow(2).contains(x, y)) { cWant = cFiles[(size_t)cSel]; cWantRec = true; return true; }
    if (cineRow(3).contains(x, y)) { cHideHud = true; cPanel = false; return true; }
    return true;
}
void cineDrawPanel() {
    if (cHideHud) return;
    const int sc = uiScale();
    auto btn = [&](const Rect2D& r, const char* t, float g) {
        renderer.rect(r.x, r.y, r.w, r.h, 0.05f, 0.05f, 0.07f, 0.85f);
        renderer.text(r.x + r.w * 0.5f - Renderer::textWidth(t, sc) * 0.5f, r.y + r.h * 0.5f - 3.5f * sc, sc, t, 1.0f, g, 0.5f, 1.0f);
    };
    renderer.beginText();
    btn(cineBtn(), "CINEMA", 0.8f);
    if (cPanel) {
        char s[96]; std::snprintf(s, sizeof s, "ROTEIRO: %s", cFiles.empty() ? "(NENHUM)" : cFiles[(size_t)cSel].c_str());
        btn(cineRow(0), s, 0.95f); btn(cineRow(1), "PREVIA", 0.95f); btn(cineRow(2), "GRAVAR", 0.55f); btn(cineRow(3), "ESCONDER HUD", 0.95f);
        const std::string m = "ULTIMO: " + cMsg;
        Rect2D r = cineRow(4); renderer.text(r.x, r.y, sc, m.c_str(), 0.8f, 0.9f, 1.0f, 1.0f);
    }
    renderer.endText();
}
