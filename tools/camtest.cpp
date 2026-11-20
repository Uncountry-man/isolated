// Testes do CameraRig (lógica pura, roda no PC). Uso: build/camtest
#include <cmath>
#include <cstdio>

#include "../engine/src/camera_rig.h"
using namespace e0;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

static float dist3(const Vec3& a, const Vec3& b) { Vec3 d = a - b; return std::sqrt(dot(d, d)); }
static const float kAspect = 2388.0f / 1080.0f;

// roda `secs` segundos a `fps`; o alvo anda em linha reta (yaw do corpo = 0 => -Z) à velocidade `speed`
static CameraRig runStraight(float fps, float secs, float speed, CamPreset p = CamPreset::Normal) {
    CameraRig r; r.setPreset(p);
    FlatWorld w;
    CameraTarget t; t.pos = {0, 0, 0}; t.yaw = 0; t.speed = speed;
    float dt = 1.0f / fps;
    for (int i = 0; i < (int)(secs * fps); ++i) {
        t.pos.z -= speed * dt;
        r.look(dt, 0, 0, false);
        r.follow(dt, t, kAspect, w);
    }
    return r;
}

int main() {
    FlatWorld world;

    // 1) parado: converge e fica estável (sem tremor sem trauma)
    {
        CameraRig r; CameraTarget t; t.pos = {3, 0, -2};
        for (int i = 0; i < 180; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, world); }
        Vec3 a = r.camera().position;
        for (int i = 0; i < 60; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, world); }
        CHECK(dist3(a, r.camera().position) < 1e-4f, "parado: câmera estável (deriva %.6f m)", dist3(a, r.camera().position));
        CHECK(std::fabs(r.info().armActual - 4.2f) < 0.01f, "braço NORMAL = %.2f m", r.info().armActual);
    }

    // 2) colisão com o chão: olhando de baixo para cima, a câmera nunca entra no chão e o braço encurta
    {
        CameraRig r; CameraTarget t; t.pos = {0, 0, 0};
        float minY = 1e9f, minArm = 1e9f;
        for (int i = 0; i < 240; ++i) {
            r.look(1 / 60.0f, 0, -60.0f, true);  // arrasta o dedo para cima: pitch sobe até o limite
            r.follow(1 / 60.0f, t, kAspect, world);
            minY = std::min(minY, r.camera().position.y); minArm = std::min(minArm, r.info().armActual);
        }
        CHECK(minY > 0.2f, "colisão: altura mínima da câmera %.2f m (raio de sonda 0.28)", minY);
        CHECK(minArm < 3.0f && r.info().collided, "colisão: braço encurtou para %.2f m", minArm);
        CHECK(r.pitch() <= 0.85f + 1e-4f, "pitch respeita o limite superior (%.2f rad)", r.pitch());
        for (int i = 0; i < 120; ++i) { r.look(1 / 60.0f, 0, 300.0f, true); r.follow(1 / 60.0f, t, kAspect, world); }  // olha para baixo
        for (int i = 0; i < 360; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, world); }
        CHECK(r.pitch() >= -1.25f - 1e-4f, "pitch respeita o limite inferior (%.2f rad)", r.pitch());
        CHECK(r.info().armActual > 3.9f, "braço volta ao normal depois (%.2f m)", r.info().armActual);
    }

    // 3) independência de FPS: mesmo trajeto a 30/60/120 fps => mesma posição final
    {
        CameraRig a = runStraight(30, 4, 6), b = runStraight(60, 4, 6), c = runStraight(120, 4, 6);
        float d1 = dist3(a.camera().position, b.camera().position), d2 = dist3(b.camera().position, c.camera().position);
        CHECK(d1 < 0.06f && d2 < 0.06f, "independente do FPS: 30x60 = %.3f m, 60x120 = %.3f m", d1, d2);
    }

    // 4) seguindo corrida: câmera não "pula" (passo por frame proporcional ao do alvo) e o personagem fica na frente
    {
        CameraRig r; CameraTarget t; FlatWorld w;
        float dt = 1 / 60.0f, maxStep = 0, prevZ = 0; bool first = true;
        for (int i = 0; i < 300; ++i) {
            t.pos.z -= 6 * dt; t.speed = 6;
            r.look(dt, 0, 0, false); r.follow(dt, t, kAspect, w);
            if (!first) maxStep = std::max(maxStep, std::fabs(r.camera().position.z - prevZ));
            prevZ = r.camera().position.z; first = false;
        }
        CHECK(maxStep < 6 * dt * 1.6f, "suavidade: maior passo da câmera %.3f m/frame (alvo %.3f)", maxStep, 6 * dt);
        float ahead = r.camera().position.z - t.pos.z;  // câmera atrás do alvo (z maior)
        CHECK(ahead > 2.5f && ahead < 5.5f, "distância atrás do jogador correndo: %.2f m", ahead);
        CHECK(r.info().fovH > 96.0f, "FOV abre ao correr: %.1f graus horizontais", r.info().fovH);
    }

    // 5) pouso: tremor proporcional ao impacto, que some em ~1 s
    {
        CameraRig r; CameraTarget t; FlatWorld w;
        for (int i = 0; i < 120; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); }
        t.grounded = false; t.vy = -9.0f; t.pos.y = 1.0f;
        for (int i = 0; i < 5; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); }
        t.grounded = true; t.vy = 0; t.pos.y = 0;
        r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w);
        float tr = r.info().trauma;
        CHECK(tr > 0.1f, "pouso gera tremor (trauma %.2f)", tr);
        for (int i = 0; i < 90; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); }
        CHECK(r.info().trauma < 0.01f && std::fabs(r.camera().roll) < 0.002f, "tremor some (trauma %.3f, roll %.4f)", r.info().trauma, r.camera().roll);
        CHECK(r.yaw() == 0.0f, "tremor não altera o yaw de movimento");
    }

    // 6) presets: 1ª pessoa esconde o personagem; voltar à 3ª pessoa o mostra; ombro desloca a câmera
    {
        CameraRig r; CameraTarget t; FlatWorld w;
        auto run = [&](float s) { for (int i = 0; i < (int)(s * 60); ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); } };
        run(2);
        CHECK(!r.hideLocal(), "NORMAL: personagem visível");
        r.setPreset(CamPreset::First); run(2);
        CHECK(r.hideLocal() && std::fabs(r.camera().position.y - 1.65f) < 0.05f, "1ª PESSOA: personagem oculto, olho a %.2f m", r.camera().position.y);
        r.setPreset(CamPreset::Normal); run(2.5f);
        CHECK(!r.hideLocal() && std::fabs(r.info().armActual - 4.2f) < 0.05f, "volta ao NORMAL: braço %.2f m", r.info().armActual);
        float x0 = r.camera().position.x;
        r.setPreset(CamPreset::Near); r.setShoulder(1); run(3);
        CHECK(r.camera().position.x - x0 > 0.5f && std::fabs(r.info().armActual - 2.7f) < 0.1f, "PERTO + ombro direito: deslocou %.2f m, braço %.2f", r.camera().position.x - x0, r.info().armActual);
    }

    // 7) FOV horizontal constante (hor+): mesma abertura lateral em telas diferentes
    {
        float hs[3];
        float asp[3] = {16.0f / 9.0f, 2388.0f / 1080.0f, 21.0f / 9.0f};
        for (int i = 0; i < 3; ++i) {
            CameraRig r; CameraTarget t; FlatWorld w;
            for (int k = 0; k < 240; ++k) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, asp[i], w); }
            hs[i] = 2.0f * std::atan(std::tan(r.camera().fovY / 2) * asp[i]) * 180.0f / 3.14159265f;
        }
        CHECK(std::fabs(hs[0] - 92) < 0.3f && std::fabs(hs[1] - 92) < 0.3f && std::fabs(hs[2] - 92) < 0.3f, "FOV horizontal %.1f / %.1f / %.1f em 16:9, 2.21:1, 21:9", hs[0], hs[1], hs[2]);
    }

    // 8) teleporte: sem arrastar a câmera pelo mapa
    {
        CameraRig r; CameraTarget t; FlatWorld w;
        for (int i = 0; i < 60; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); }
        t.pos = {80, 0, -40};
        r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w);
        CHECK(dist3(r.camera().position, t.pos) < 6.5f, "teleporte de 90 m: câmera já no destino (%.2f m do jogador)", dist3(r.camera().position, t.pos));
    }

    // 9) olhar: inércia após soltar e sensibilidade
    {
        CameraRig r; CameraTarget t; FlatWorld w;
        for (int i = 0; i < 20; ++i) { r.look(1 / 60.0f, 8.0f, 0, true); r.follow(1 / 60.0f, t, kAspect, w); }  // arrasto rápido p/ direita
        float yRel = r.yaw();
        for (int i = 0; i < 90; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); }
        float yEnd = r.yaw();
        CHECK(yRel > 0.3f && yEnd > yRel + 0.05f && yEnd < yRel + 1.0f, "inércia: yaw %.2f -> %.2f rad e para", yRel, yEnd);
        float a = r.yaw(); r.look(1 / 60.0f, 0, 0, false);
        CHECK(std::fabs(r.yaw() - a) < 1e-4f, "para completamente depois da inércia");
        CameraRig lo, hi; lo.setSensitivity(0.6f); hi.setSensitivity(1.7f);
        for (int i = 0; i < 30; ++i) { lo.look(1 / 60.0f, 5, 0, true); hi.look(1 / 60.0f, 5, 0, true); }
        CHECK(hi.yaw() > lo.yaw() * 2.4f, "sensibilidade 1.7x gira %.2fx mais que 0.6x", hi.yaw() / lo.yaw());
    }

    // 10) pulo: câmera não acompanha 1:1 a altura do salto (vertical amortecido)
    {
        CameraRig r; CameraTarget t; FlatWorld w;
        for (int i = 0; i < 120; ++i) { r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w); }
        float y0 = r.info().pivot.y, maxRise = 0;
        for (int i = 0; i < 40; ++i) {  // salto de 1.17 m em ~0.72 s
            float tt = i / 60.0f; t.pos.y = std::max(0.0f, 6.5f * tt - 9.0f * tt * tt); t.vy = 6.5f - 18.0f * tt; t.grounded = t.pos.y <= 0.0f && i > 2 ? true : false;
            r.look(1 / 60.0f, 0, 0, false); r.follow(1 / 60.0f, t, kAspect, w);
            maxRise = std::max(maxRise, r.info().pivot.y - y0);
        }
        CHECK(maxRise < 0.9f && maxRise > 0.2f, "pulo de 1.17 m: pivô subiu %.2f m (amortecido)", maxRise);
    }

    std::printf(fails ? "\n%d FALHA(S)\n" : "\nTODOS OS TESTES DE CAMERA PASSARAM\n", fails);
    return fails ? 1 : 0;
}
