#include "camera_rig.h"

#include <algorithm>
#include <cmath>

namespace e0 {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kProbeRadius = 0.28f;   // raio da esfera de colisão da câmera
constexpr float kPitchMin = -1.25f, kPitchMax = 0.85f, kPitchSoft = 0.22f;
constexpr float kBaseSens = 0.0045f;    // rad/pixel (x sensibilidade)
constexpr float kEyeHeight = 1.65f;     // 1ª pessoa

struct Preset { float dist, shoulder, pivotH, fovH; };
constexpr Preset kPresets[4] = {
    {4.2f, 0.00f, 1.55f, 92.0f},   // NORMAL
    {2.7f, 0.65f, 1.55f, 88.0f},   // PERTO (ombro maior)
    {6.8f, 0.30f, 1.50f, 96.0f},   // LONGE
    {0.0f, 0.00f, kEyeHeight, 100.0f},  // 1ª PESSOA
};

// SmoothDamp (mola criticamente amortecida, sem overshoot, independente do FPS).
float damp(float cur, float target, float& vel, float smoothTime, float dt) {
    smoothTime = std::max(0.0001f, smoothTime);
    float omega = 2.0f / smoothTime, x = omega * dt;
    float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = cur - target;
    float temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * e;
    return target + (change + temp) * e;
}
Vec3 damp3(const Vec3& cur, const Vec3& target, Vec3& vel, float t, float dt) {
    return {damp(cur.x, target.x, vel.x, t, dt), damp(cur.y, target.y, vel.y, t, dt), damp(cur.z, target.z, vel.z, t, dt)};
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float len3(const Vec3& v) { return std::sqrt(dot(v, v)); }

// Ruído de valor 1D suave (para o tremor).
float hash1(int n) { n = (n << 13) ^ n; return 1.0f - (float)((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f; }
float noise1(float x) {
    int i = (int)std::floor(x);
    float f = x - (float)i, u = f * f * (3 - 2 * f);
    return hash1(i) * (1 - u) + hash1(i + 1) * u;
}

}  // namespace

const char* CameraRig::presetName(CamPreset p) {
    static const char* n[4] = {"NORMAL", "PERTO", "LONGE", "1A PESSOA"};
    return n[(int)p & 3];
}

void CameraRig::cycleSensitivity() {
    static const float v[5] = {0.6f, 0.8f, 1.0f, 1.3f, 1.7f};
    int best = 0;
    for (int i = 0; i < 5; ++i) if (std::fabs(v[i] - sens_) < std::fabs(v[best] - sens_)) best = i;
    sens_ = v[(best + 1) % 5];
}

void CameraRig::addTrauma(float t) { trauma_ = std::min(1.0f, trauma_ + t); }

void CameraRig::look(float dt, float dxPix, float dyPix, bool touching) {
    dt = clampf(dt, 1e-4f, 0.1f);
    if (touching) {
        // pixels -> velocidade angular; leve aceleração em arrastos rápidos (precisão devagar, giro rápido)
        float vx = dxPix / dt, vy = dyPix / dt;
        float speedPx = std::sqrt(vx * vx + vy * vy);
        float gain = kBaseSens * sens_ * (1.0f + 0.35f * clampf(speedPx / 3000.0f, 0.0f, 1.0f)) * (1.0f - 0.45f * aim_);   // mirando: giro mais fino
        float k = 1.0f - std::exp(-dt / 0.022f);  // passa-baixa curto: tira o serrilhado do toque sem dar lag perceptível
        lookVelYaw_ += (vx * gain - lookVelYaw_) * k;
        lookVelPitch_ += (-vy * gain - lookVelPitch_) * k;
    } else {
        float d = std::exp(-dt / 0.16f);           // inércia: o giro continua um pouco e freia
        lookVelYaw_ *= d; lookVelPitch_ *= d;
        if (std::fabs(lookVelYaw_) < 0.01f) lookVelYaw_ = 0;
        if (std::fabs(lookVelPitch_) < 0.01f) lookVelPitch_ = 0;
    }
    if (vehicle_) {   // câmera de veículo: sem arrastar, volta suavemente para trás do veículo (yaw da proa) e para um ângulo alto fixo
        vehIdle_ = touching ? 0.0f : vehIdle_ + dt;
        if (vehIdle_ > 1.2f) {
            float d = heading_ - yaw_;
            d = std::fmod(d + kPi, 2 * kPi); if (d < 0) d += 2 * kPi; d -= kPi;
            float k = 1.0f - std::exp(-dt / 0.55f);
            yaw_ += d * k; pitch_ += (-0.24f - pitch_) * (1.0f - std::exp(-dt / 0.7f));
            lookVelYaw_ *= 0.5f; lookVelPitch_ *= 0.5f;
        }
    } else vehIdle_ = 9.0f;
    yaw_ += lookVelYaw_ * dt;
    if (yaw_ > kPi) yaw_ -= 2 * kPi; else if (yaw_ < -kPi) yaw_ += 2 * kPi;

    // pitch com limite suave: a velocidade vai a zero ao se aproximar do batente (sem "parede" seca)
    float v = lookVelPitch_;
    if (v > 0 && pitch_ > kPitchMax - kPitchSoft) v *= clampf((kPitchMax - pitch_) / kPitchSoft, 0.0f, 1.0f);
    if (v < 0 && pitch_ < kPitchMin + kPitchSoft) v *= clampf((pitch_ - kPitchMin) / kPitchSoft, 0.0f, 1.0f);
    pitch_ = clampf(pitch_ + v * dt, kPitchMin, kPitchMax);
    if (preset_ == CamPreset::First) pitch_ = clampf(pitch_, -1.4f, 1.4f);
}

void CameraRig::follow(float dt, const CameraTarget& t, float aspect, const CameraWorld& world) {
    dt = clampf(dt, 1e-4f, 0.1f);
    time_ += dt;
    const Preset& P = kPresets[(int)preset_ & 3];
    bool first = preset_ == CamPreset::First;
    float side = (float)shoulder_ * P.shoulder;
    if (P.shoulder == 0.0f && shoulder_ != 0 && !first) side = (float)shoulder_ * 0.35f;  // NORMAL/LONGE com ombro escolhido

    // ---- teleporte / primeira vez: sem suavização ----
    if (!init_ || len3(t.pos - lastTarget_) > (vehicle_ ? 40.0f : 6.0f)) {
        init_ = true;
        pos_ = t.pos; posVel_ = {}; ahead_ = {}; aheadVel_ = {};
        dist_ = P.dist; shoulderOff_ = side; pivotH_ = P.pivotH; fovH_ = P.fovH;
        fp_ = first ? 1.0f : 0.0f; sprint_ = 0; air_ = 0; arm_ = P.dist; armV_ = 0; dip_ = dipVel_ = 0;
        wasGrounded_ = t.grounded; lastVy_ = t.vy;
    }
    lastTarget_ = t.pos;

    // ---- transições suaves de preset (e do modo veículo: 52 m atrás, pivô 10 m acima, FOV 82) ----
    veh_ = clampf(damp(veh_, vehicle_ ? 1.0f : 0.0f, vehV_, 0.6f, dt), 0.0f, 1.0f);
    aim_ = clampf(damp(aim_, aimOn_ && !vehicle_ ? 1.0f : 0.0f, aimV_, 0.10f, dt), 0.0f, 1.0f);
    float pDist = P.dist + (vehDist_ - P.dist) * veh_, pPivot = P.pivotH + (vehUp_ - P.pivotH) * veh_, pFov = P.fovH + (82.0f - P.fovH) * veh_;
    if (aim_ > 0.0f) {   // mira: sobre o ombro direito, 1,6 m atrás, FOV 60 (em 1ª pessoa só o zoom)
        if (!first) { pDist += (1.6f - pDist) * aim_; side += (0.62f - side) * aim_; pPivot += (1.60f - pPivot) * aim_; }
        pFov += ((first ? 70.0f : 60.0f) - pFov) * aim_;
    }
    dist_ = damp(dist_, pDist, distV_, 0.30f, dt);
    shoulderOff_ = damp(shoulderOff_, side * (1.0f - veh_), shoulderV_, 0.25f, dt);
    pivotH_ = damp(pivotH_, pPivot, pivotHV_, 0.30f, dt);
    fovH_ = damp(fovH_, pFov, fovHV_, 0.35f, dt);
    fp_ = damp(fp_, first ? 1.0f : 0.0f, fpV_, 0.22f, dt);
    fp_ = clampf(fp_, 0.0f, 1.0f);

    // ---- estados: corrida e ar ----
    float sprintT = clampf((t.speed - 3.6f) / (6.0f - 3.6f), 0.0f, 1.0f) * (1.0f - aim_);
    sprint_ = clampf(damp(sprint_, sprintT, sprintV_, 0.22f, dt), 0.0f, 1.0f);
    air_ = clampf(damp(air_, t.grounded ? 0.0f : 1.0f, airV_, 0.15f, dt), 0.0f, 1.0f);

    // ---- pouso: tremor + "mergulho" da câmera ----
    if (!t.grounded) lastVy_ = t.vy;
    if (t.grounded && !wasGrounded_) {
        float impact = clampf(-lastVy_, 0.0f, 14.0f);
        if (impact > 2.0f) { addTrauma(clampf(impact / 14.0f, 0.0f, 1.0f) * 0.38f); dipVel_ = -impact * 0.55f; }
    }
    wasGrounded_ = t.grounded;
    // mola subamortecida simples p/ o mergulho (volta a 0 com um pequeno repique)
    dipVel_ += (-140.0f * dip_ - 17.0f * dipVel_) * dt;
    dip_ = clampf(dip_ + dipVel_ * dt, -0.30f, 0.12f);

    // ---- pivô: horizontal quase rígido, vertical amortecido (pulo não "chicoteia" a câmera) ----
    Vec3 target = t.pos;
    pos_.x = damp(pos_.x, target.x, posVel_.x, 0.045f * (1.0f - 0.6f * aim_), dt);
    pos_.z = damp(pos_.z, target.z, posVel_.z, 0.045f * (1.0f - 0.6f * aim_), dt);
    pos_.y = damp(pos_.y, target.y, posVel_.y, 0.12f + 0.34f * air_, dt);  // no ar a câmera segue só ~65% do salto

    // look-ahead: a câmera adianta um pouco para onde o jogador vai
    Vec3 vel{std::sin(t.yaw) * t.speed, 0, -std::cos(t.yaw) * t.speed};
    Vec3 aheadT = vel * 0.10f;
    float al = len3(aheadT);
    if (al > 0.7f) aheadT = aheadT * (0.7f / al);
    ahead_ = damp3(ahead_, aheadT * (1.0f - fp_) * (1.0f - aim_), aheadVel_, 0.28f, dt);

    Vec3 pivot = pos_ + ahead_ + Vec3{0, pivotH_ + dip_ * (1.0f - 0.5f * fp_), 0};
    Vec3 fwd{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), -std::cos(yaw_) * std::cos(pitch_)};
    Vec3 right{std::cos(yaw_), 0, std::sin(yaw_)};

    // ---- 3ª pessoa: braço de mola com colisão ----
    float dist = dist_ * (1.0f + 0.12f * sprint_ * (1.0f - veh_));
    Vec3 desired = pivot - fwd * dist + right * shoulderOff_;
    Vec3 line = desired - pivot;
    float lineLen = len3(line);
    Vec3 lineDir = lineLen > 1e-4f ? line * (1.0f / lineLen) : Vec3{0, 0, 1};
    float frac = lineLen > 1e-4f ? world.sweepSphere(pivot, desired, kProbeRadius) : 1.0f;
    float targetLen = lineLen * clampf(frac, 0.0f, 1.0f);
    // bateu: encurta quase na hora; livre: volta devagar (evita "pisca" ao passar por quinas)
    // bateu: o braço NUNCA passa do comprimento livre (encurta na hora, sem atravessar a parede); livre: volta devagar
    arm_ = damp(arm_, targetLen, armV_, 0.45f, dt);
    if (arm_ > targetLen) { arm_ = targetLen; armV_ = 0; }
    arm_ = std::max(0.0f, std::min(arm_, lineLen));
    Vec3 pos3 = pivot + lineDir * arm_;

    // ---- 1ª pessoa: olho no corpo, com balanço de corrida ----
    float bobAmp = 0.0f;
    if (t.grounded) bobPhase_ += 2.0f * kPi * (t.speed * 0.55f) * dt;
    bobAmp = clampf(t.speed / 6.0f, 0.0f, 1.0f) * (t.grounded ? 1.0f : 0.0f) * fp_;
    Vec3 head = Vec3{pos_.x, pos_.y + kEyeHeight + dip_, pos_.z} + fwd * 0.10f;
    head.y += std::sin(bobPhase_ * 2.0f) * 0.020f * bobAmp;
    head = head + right * (std::cos(bobPhase_) * 0.012f * bobAmp);
    Vec3 pos = pos3 * (1.0f - fp_) + head * fp_;

    // ---- FOV horizontal -> vertical (hor+): mesmo campo lateral em qualquer proporção de tela ----
    float fovH = fovH_ + 7.0f * sprint_ + 2.5f * air_;
    float vf = 2.0f * std::atan(std::tan(fovH * kPi / 360.0f) / std::max(0.5f, aspect));
    vf = clampf(vf, 40.0f * kPi / 180.0f, 80.0f * kPi / 180.0f);

    // ---- tremor por trauma (ruído suave; só na saída, nunca no yaw de movimento) ----
    trauma_ = std::max(0.0f, trauma_ - 1.1f * dt);
    float sh = trauma_ * trauma_;
    float f = time_ * 18.0f;
    float shYaw = sh * 0.030f * noise1(f + 11.0f), shPitch = sh * 0.030f * noise1(f + 37.0f), shRoll = sh * 0.040f * noise1(f + 73.0f);
    Vec3 shPos{sh * 0.025f * noise1(f + 101.0f), sh * 0.025f * noise1(f + 131.0f), 0};

    cam_.position = pos + right * shPos.x + Vec3{0, shPos.y, 0};
    cam_.yaw = yaw_ + shYaw;
    cam_.pitch = clampf(pitch_ + shPitch, -1.5f, 1.5f);
    cam_.roll = shRoll;
    cam_.fovY = vf;
    cam_.zNear = 0.1f;
    cam_.zFar = 500.0f;

    hide_ = fp_ > 0.8f || arm_ < 0.9f;

    info_.armActual = arm_; info_.armDesired = lineLen; info_.collided = frac < 0.999f;
    info_.fovH = fovH; info_.fovV = vf * 180.0f / kPi; info_.trauma = trauma_;
    info_.lookVel = std::sqrt(lookVelYaw_ * lookVelYaw_ + lookVelPitch_ * lookVelPitch_);
    info_.sprint = sprint_; info_.fp = fp_; info_.dist = dist; info_.pivot = pivot;
}

}  // namespace e0
