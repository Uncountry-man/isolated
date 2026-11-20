// Isolated - ambiente (céu, luz, névoa) em função da hora do ciclo dia/noite.
// A hora vem do SERVIDOR (shared/protocol.h: kDayLengthSec/kNightLengthSec) para todos verem o mesmo céu.
#pragma once
#include <algorithm>
#include <cmath>

#include "../../shared/protocol.h"
#include "vecmath.h"

namespace e0 {

struct Environment {
    Vec3 sunDir{0, 1, 0};     // aponta PARA o sol (a lua fica no oposto)
    Vec3 horizon{0.6f, 0.78f, 0.95f}, zenith{0.25f, 0.5f, 0.9f};
    Vec3 lightDir{0, 1, 0};   // luz dominante (sol de dia, lua de noite)
    Vec3 lightCol{1, 1, 1};
    Vec3 ambient{0.35f, 0.38f, 0.45f};
    float night = 0;          // 0 = dia pleno ... 1 = noite plena (estrelas, lua)
};

inline float smoothstepf(float a, float b, float x) {
    float t = std::min(1.0f, std::max(0.0f, (x - a) / (b - a)));
    return t * t * (3 - 2 * t);
}
inline Vec3 mixv(const Vec3& a, const Vec3& b, float t) { return a * (1 - t) + b * t; }

// cycleSec em [0, kCycleSec): 0 = nascer do sol, kDayLengthSec = pôr do sol, depois noite.
inline Environment makeEnvironment(double cycleSec) {
    using namespace e0net;
    float t = (float)std::fmod(cycleSec, (double)kCycleSec);
    float a = t < kDayLengthSec ? kPi * t / kDayLengthSec : kPi + kPi * (t - kDayLengthSec) / kNightLengthSec;
    Environment e;
    e.sunDir = normalize({std::cos(a), std::sin(a), 0.3f});  // nasce a leste (+X), põe a oeste; leve inclinação em Z
    float el = e.sunDir.y;

    float day = smoothstepf(-0.12f, 0.22f, el);
    float tw = std::exp(-(el / 0.18f) * (el / 0.18f));  // crepúsculo: forte perto do horizonte
    e.night = 1.0f - day;
    e.horizon = mixv(mixv({0.02f, 0.03f, 0.08f}, {0.62f, 0.78f, 0.95f}, day), {1.0f, 0.55f, 0.30f}, tw * 0.55f);
    e.zenith = mixv(mixv({0.005f, 0.01f, 0.04f}, {0.22f, 0.45f, 0.88f}, day), {0.35f, 0.25f, 0.50f}, tw * 0.25f);

    float sunInt = std::min(1.0f, std::max(0.0f, el * 2.5f)) * 0.7f;
    float moonInt = std::min(1.0f, std::max(0.0f, -el * 2.5f)) * 0.45f;
    Vec3 sunCol = mixv({1.0f, 0.60f, 0.35f}, {1.0f, 0.96f, 0.90f}, smoothstepf(0.0f, 0.45f, el)) * sunInt;
    Vec3 moonCol = Vec3{0.45f, 0.55f, 0.90f} * moonInt;
    if (el >= 0) { e.lightDir = e.sunDir; e.lightCol = sunCol; }
    else { e.lightDir = e.sunDir * -1.0f; e.lightCol = moonCol; }
    e.ambient = mixv({0.17f, 0.21f, 0.36f}, {0.35f, 0.38f, 0.45f}, day);
    return e;
}

}  // namespace e0
