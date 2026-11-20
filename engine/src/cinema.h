// Isolated - MODO CINEMA: roteiro (.cine) e avaliação da câmera. C++ puro (testado no PC em tools/cinematest.cpp).
// Só existe em builds com E0_CINEMA=1 (ver engine_cinema.inc). A linguagem está documentada em CLAUDE.md, seção "Modo cinema".
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace e0 { namespace cine {

struct Key { float t, p[3], l[3], fov, roll; };                       // p/l no espaço de camera/alvo; fov horizontal em graus
struct Orbit { bool on = false; float c[3], r0, r1, h0, h1, a0, a1, fov0, fov1; };   // ângulo 0 = proa, 90 = estibordo (no espaço do navio) ou -Z, +X (no mundo)
struct ShipSet { bool set = false, hide = false; float x = 0, z = 0, yaw = 0, speed = 0, steer = 0; };
struct Crew { int ship; float a, b, yaw; };
struct Fire { float t; int ship, side, n; float dist; bool done = false; };
struct Shake { float t0, dur, amp; };
struct Title { float t0, t1; std::string text; };

struct Shot {
    std::string name = "PLANO"; float dur = 5.0f;
    int camSpace = -1, lookSpace = -2;     // -1 = mundo, 0.. = navio; lookSpace -2 = igual a camSpace
    bool ease = false;                     // suave: acelera e freia no começo/fim do plano
    std::vector<Key> keys; Orbit orbit;
    float hour = -1.0f;                    // cycleSec; < 0 = continua
    float fog = 1.0f, slow = 1.0f, bars = 0.0f;   // névoa (1 = normal), tempo do mundo, proporção das barras (0 = sem)
    float fadeIn = 0.0f, fadeOut = 0.0f; bool white = false;
    float tint[4] = {0, 0, 0, 0};
    int focusShip = -1; bool focusSet = false; float focus[2] = {0, 0};
    ShipSet ships[8];
    std::vector<Crew> crew; std::vector<Fire> fires; std::vector<Shake> shakes; std::vector<Title> titles;
};

struct Script {
    std::string name = "video"; int w = 1920, h = 1080, fps = 60; float mbps = 40.0f;
    std::vector<Shot> shots;
    float total() const { float s = 0; for (const Shot& h : shots) s += h.dur; return s; }
};

inline float hourToCycle(const std::string& s) {   // "16:30" -> cycleSec (06:00 = 0; 1 h do jogo = 75 s)
    int h = 0, m = 0; std::sscanf(s.c_str(), "%d:%d", &h, &m);
    float x = ((float)h + (float)m / 60.0f - 6.0f) / 24.0f;
    x -= std::floor(x);
    return x * 1800.0f;
}

// Separa em palavras; "texto com espaço" é uma palavra só; # começa comentário
inline std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> o; std::string c; bool q = false, has = false;
    for (char ch : line) {
        if (q) { if (ch == '"') q = false; else c += ch; continue; }
        if (ch == '"') { q = true; has = true; continue; }
        if (ch == '#') break;
        if (ch == ' ' || ch == '\t' || ch == '\r') { if (has || !c.empty()) { o.push_back(c); c.clear(); has = false; } continue; }
        c += ch;
    }
    if (has || !c.empty()) o.push_back(c);
    return o;
}

inline bool parse(const std::string& text, Script& S, std::string& err) {
    std::istringstream in(text); std::string line; int ln = 0; Shot* cur = nullptr;
    auto fail = [&](const std::string& m) { err = "linha " + std::to_string(ln) + ": " + m; return false; };
    while (std::getline(in, line)) {
        ++ln;
        auto w = words(line);
        if (w.empty()) continue;
        const std::string& c = w[0];
        const size_t n = w.size();
        auto F = [&](size_t i) { return (float)std::atof(w[i].c_str()); };
        auto I = [&](size_t i) { return std::atoi(w[i].c_str()); };
        auto space = [&](size_t i, int& out) {   // "mundo" | "navio N"
            if (w[i] == "mundo") { out = -1; return true; }
            if (w[i] == "navio" && n > i + 1) { out = I(i + 1); return true; }
            return false;
        };
        if (c == "nome") { if (n < 2) return fail("nome sem valor"); S.name = w[1]; continue; }
        if (c == "video") { if (n < 5) return fail("video L A FPS MBPS"); S.w = I(1); S.h = I(2); S.fps = I(3); S.mbps = F(4); continue; }
        if (c == "plano") {
            if (n < 3) return fail("plano \"nome\" duracao");
            S.shots.emplace_back(); cur = &S.shots.back(); cur->name = w[1]; cur->dur = F(2);
            if (cur->dur <= 0.0f) return fail("duracao do plano precisa ser > 0");
            continue;
        }
        if (!cur) return fail("comando antes do primeiro plano: " + c);
        Shot& H = *cur;
        if (c == "camera") { if (n < 2 || !space(1, H.camSpace)) return fail("camera mundo | camera navio N"); }
        else if (c == "alvo") { if (n < 2 || !space(1, H.lookSpace)) return fail("alvo mundo | alvo navio N"); }
        else if (c == "chave") {
            if (n < 9) return fail("chave t px py pz lx ly lz fov [roll]");
            Key k{F(1), {F(2), F(3), F(4)}, {F(5), F(6), F(7)}, F(8), n > 9 ? F(9) : 0.0f};
            if (!H.keys.empty() && k.t <= H.keys.back().t) return fail("chaves precisam estar em ordem de tempo");
            H.keys.push_back(k);
        }
        else if (c == "orbita") {
            if (n < 12) return fail("orbita ca cb cy raio0 raio1 alt0 alt1 ang0 ang1 fov0 fov1");
            Orbit& o = H.orbit; o.on = true;
            o.c[0] = F(1); o.c[1] = F(2); o.c[2] = F(3); o.r0 = F(4); o.r1 = F(5); o.h0 = F(6); o.h1 = F(7); o.a0 = F(8); o.a1 = F(9); o.fov0 = F(10); o.fov1 = F(11);
        }
        else if (c == "suave") H.ease = true;
        else if (c == "hora") { if (n < 2) return fail("hora HH:MM"); H.hour = hourToCycle(w[1]); }
        else if (c == "nevoa") { if (n < 2) return fail("nevoa K"); H.fog = F(1); }
        else if (c == "lento") { if (n < 2) return fail("lento K"); H.slow = F(1); }
        else if (c == "barras") { if (n < 2) return fail("barras PROPORCAO"); H.bars = F(1); }
        else if (c == "fade_in") { if (n < 2) return fail("fade_in S"); H.fadeIn = F(1); }
        else if (c == "fade_out") { if (n < 2) return fail("fade_out S"); H.fadeOut = F(1); }
        else if (c == "fade_branco") H.white = true;
        else if (c == "tinta") { if (n < 5) return fail("tinta r g b a"); for (int i = 0; i < 4; ++i) H.tint[i] = F((size_t)i + 1); }
        else if (c == "foco") {
            if (n >= 3 && w[1] == "navio") H.focusShip = I(2);
            else if (n >= 3) { H.focusSet = true; H.focus[0] = F(1); H.focus[1] = F(2); }
            else return fail("foco navio N | foco x z");
        }
        else if (c == "navio") {
            if (n < 3) return fail("navio N x z rumo vel leme | navio N some");
            int i = I(1); if (i < 0 || i >= 8) return fail("navio fora do intervalo");
            ShipSet& s = H.ships[i]; s.set = true;
            if (w[2] == "some") s.hide = true;
            else { if (n < 7) return fail("navio N x z rumo vel leme"); s.x = F(2); s.z = F(3); s.yaw = F(4) * 0.01745329f; s.speed = F(5); s.steer = F(6); }
        }
        else if (c == "tripulante") { if (n < 5) return fail("tripulante navio a b rumo"); H.crew.push_back({I(1), F(2), F(3), F(4) * 0.01745329f}); }
        else if (c == "tiro") { if (n < 6) return fail("tiro t navio lado n dist"); H.fires.push_back({F(1), I(2), I(3), I(4), F(5)}); }
        else if (c == "tremor") { if (n < 4) return fail("tremor t dur amp"); H.shakes.push_back({F(1), F(2), F(3)}); }
        else if (c == "titulo") { if (n < 4) return fail("titulo t0 t1 \"texto\""); H.titles.push_back({F(1), F(2), w[3]}); }
        else return fail("comando desconhecido: " + c);
    }
    if (S.shots.empty()) { err = "roteiro sem planos"; return false; }
    for (size_t i = 0; i < S.shots.size(); ++i) {
        const Shot& H = S.shots[i];
        if (H.keys.empty() && !H.orbit.on) { err = "plano \"" + H.name + "\" sem chave nem orbita"; return false; }
    }
    return true;
}

inline float smooth01(float x) { x = std::max(0.0f, std::min(1.0f, x)); return x * x * (3.0f - 2.0f * x); }

// Posição, alvo, fov e roll no instante t (espaços de camSpace/lookSpace). Hermite com tangentes por diferença finita: passa pelas chaves sem quina.
struct Eval { float p[3], l[3], fov, roll; };
inline Eval evalShot(const Shot& H, float t) {
    Eval e{};
    float u = H.dur > 0.0f ? std::max(0.0f, std::min(1.0f, t / H.dur)) : 0.0f;
    if (H.ease) u = smooth01(u);
    if (H.orbit.on) {
        const Orbit& o = H.orbit;
        float r = o.r0 + (o.r1 - o.r0) * u, h = o.h0 + (o.h1 - o.h0) * u, a = (o.a0 + (o.a1 - o.a0) * u) * 0.01745329f;
        e.p[0] = o.c[0] + std::cos(a) * r; e.p[1] = o.c[1] + std::sin(a) * r; e.p[2] = o.c[2] + h;
        e.l[0] = o.c[0]; e.l[1] = o.c[1]; e.l[2] = o.c[2];
        e.fov = o.fov0 + (o.fov1 - o.fov0) * u;
        return e;
    }
    const auto& K = H.keys;
    const float tt = u * H.dur;
    if (K.size() == 1 || tt <= K.front().t) { const Key& k = K.front(); for (int i = 0; i < 3; ++i) { e.p[i] = k.p[i]; e.l[i] = k.l[i]; } e.fov = k.fov; e.roll = k.roll; return e; }
    if (tt >= K.back().t) { const Key& k = K.back(); for (int i = 0; i < 3; ++i) { e.p[i] = k.p[i]; e.l[i] = k.l[i]; } e.fov = k.fov; e.roll = k.roll; return e; }
    size_t i = 1; while (K[i].t < tt) ++i;
    const Key &a = K[i - 1], &b = K[i];
    const Key& pa = i >= 2 ? K[i - 2] : a; const Key& nb = i + 1 < K.size() ? K[i + 1] : b;
    const float d = b.t - a.t, s = (tt - a.t) / d, s2 = s * s, s3 = s2 * s;
    const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    auto her = [&](float v0, float v1, float vp, float vn) {
        float m0 = (v1 - vp) / std::max(1e-4f, b.t - pa.t), m1 = (vn - v0) / std::max(1e-4f, nb.t - a.t);
        return h00 * v0 + h10 * d * m0 + h01 * v1 + h11 * d * m1;
    };
    for (int c = 0; c < 3; ++c) { e.p[c] = her(a.p[c], b.p[c], pa.p[c], nb.p[c]); e.l[c] = her(a.l[c], b.l[c], pa.l[c], nb.l[c]); }
    e.fov = her(a.fov, b.fov, pa.fov, nb.fov); e.roll = her(a.roll, b.roll, pa.roll, nb.roll);
    return e;
}

// Opacidade do fade no instante t do plano (0 = limpo, 1 = cor cheia)
inline float fadeAt(const Shot& H, float t) {
    float f = 0.0f;
    if (H.fadeIn > 0.0f && t < H.fadeIn) f = 1.0f - smooth01(t / H.fadeIn);
    if (H.fadeOut > 0.0f && t > H.dur - H.fadeOut) f = std::max(f, smooth01((t - (H.dur - H.fadeOut)) / H.fadeOut));
    return f;
}

// Tremor suave: soma de senos com frequências incomensuráveis, some no fim
inline float shakeAmp(const Shot& H, float t) {
    float a = 0.0f;
    for (const Shake& s : H.shakes) if (t >= s.t0 && t < s.t0 + s.dur) { float k = 1.0f - (t - s.t0) / s.dur; a = std::max(a, s.amp * k * k); }
    return a;
}

}}  // namespace e0::cine
