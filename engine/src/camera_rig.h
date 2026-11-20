// Isolated - CameraRig: sistema de câmera em 3ª/1ª pessoa (puro C++, sem dependência de Android; testado no PC).
//
//  * Braço de mola (spring arm) com colisão por esfera: encurta rápido ao bater, volta devagar (CameraWorld).
//  * Suavização crítica (SmoothDamp) independente do FPS: seguir horizontal quase rígido, vertical amortecido (pulo),
//    look-ahead na direção do movimento, "mergulho" ao aterrissar.
//  * Presets (NORMAL / PERTO / LONGE / 1ª PESSOA) com transição suave, ombro esquerdo/centro/direito.
//  * FOV definido na HORIZONTAL (hor+): igual em qualquer tela; abre ao correr; vertical derivado do aspecto.
//  * Olhar: filtro passa-baixa curto + inércia ao soltar o dedo + leve aceleração + limites suaves de pitch.
//  * Tremor por "trauma" (ruído suave; pouso, impactos futuros) + balanço da cabeça em 1ª pessoa.
//  * Esconde o personagem local em 1ª pessoa ou com o braço curto demais (hideLocal()).
//
// Uso por frame: rig.look(dt, dx, dy, tocando) -> usar rig.yaw() no movimento -> rig.follow(dt, alvo, aspecto, mundo)
// -> rig.camera() é a câmera final (com tremor).
#pragma once
#include <algorithm>
#include "camera.h"

namespace e0 {

// Mundo visto pela câmera (colisão). Implemente para paredes/árvores/construções; hoje só existe o chão.
struct CameraWorld {
    virtual ~CameraWorld() = default;
    // Varre uma esfera de `from` até `to`; devolve a fração livre do caminho em [0,1] (1 = sem obstáculo).
    virtual float sweepSphere(const Vec3& from, const Vec3& to, float radius) const = 0;
};

struct FlatWorld : CameraWorld {  // chão plano em y = 0
    float sweepSphere(const Vec3& from, const Vec3& to, float r) const override {
        if (to.y >= r) return 1.0f;
        if (from.y <= r) return 0.0f;
        float t = (from.y - r) / (from.y - to.y);
        return t < 0 ? 0 : (t > 1 ? 1 : t);
    }
};

enum class CamPreset { Normal = 0, Near, Far, First, Count };

struct CameraTarget {   // o jogador que a câmera segue (use o estado "visual", já suavizado pela rede)
    Vec3 pos{};         // pés
    float yaw = 0;      // direção do corpo
    float speed = 0;    // m/s horizontal
    float vy = 0;       // velocidade vertical
    bool grounded = true;
};

struct CameraInfo {     // para depuração
    float armActual = 0, armDesired = 0, fovH = 0, fovV = 0, trauma = 0, lookVel = 0, sprint = 0, fp = 0, dist = 0;
    bool collided = false;
    Vec3 pivot{};
};

class CameraRig {
public:
    // ---- entrada ----
    void look(float dt, float dxPix, float dyPix, bool touching);  // deltas do arrasto em pixels (dy>0 = dedo p/ baixo)
    void follow(float dt, const CameraTarget& t, float aspect, const CameraWorld& world);
    // CÂMERA DE VEÍCULO (navio): 52 m atrás e 10 m acima, o yaw segue a proa sozinho. Arrastar gira livremente; ~1,2 s depois de soltar ela volta para trás do veículo.
    // Chamar todo frame ANTES de look(). `heading` = yaw do veículo (mesma convenção do jogador: 0 = -Z).
    void setVehicle(bool on, float heading, float dist = 52.0f, float up = 10.0f) { vehicle_ = on; heading_ = heading; if (on) { vehDist_ = dist; vehUp_ = up; } }
    bool vehicle() const { return vehicle_; }
    void addTrauma(float t);   // 0..1 (soma, com teto 1); decai sozinho
    // MIRA (bacamarte): câmera sobre o ombro direito, perto e com zoom; o giro fica mais fino. kick = coice do tiro (sobe a mira; o jogador corrige)
    void setAim(bool on) { aimOn_ = on; }
    float aimAmount() const { return aim_; }
    void kick(float pitchUp, float yawSide) { pitch_ = std::min(0.85f, pitch_ + pitchUp); yaw_ += yawSide; }
    void snap() { init_ = false; }  // teleporte/respawn: próxima atualização não suaviza

    // ---- configuração ----
    void setPreset(CamPreset p) { preset_ = p; }
    CamPreset preset() const { return preset_; }
    void cyclePreset() { preset_ = (CamPreset)(((int)preset_ + 1) % (int)CamPreset::Count); }
    static const char* presetName(CamPreset p);
    void setShoulder(int side) { shoulder_ = side < -1 ? -1 : (side > 1 ? 1 : side); }  // -1 esq, 0 centro, +1 dir
    int shoulder() const { return shoulder_; }
    void cycleShoulder() { shoulder_ = shoulder_ >= 1 ? -1 : shoulder_ + 1; }
    void setSensitivity(float s) { sens_ = s; }
    float sensitivity() const { return sens_; }
    void cycleSensitivity();

    // ---- saída ----
    const Camera& camera() const { return cam_; }
    float yaw() const { return yaw_; }       // yaw/pitch "de mira" (sem tremor): use p/ mover o jogador
    float pitch() const { return pitch_; }
    bool hideLocal() const { return hide_; }
    const CameraInfo& info() const { return info_; }

private:
    float yaw_ = 0, pitch_ = -0.3f;
    float lookVelYaw_ = 0, lookVelPitch_ = 0;     // rad/s (filtrado; vira inércia ao soltar)
    CamPreset preset_ = CamPreset::Normal;
    int shoulder_ = 0;
    float sens_ = 1.0f;
    bool aimOn_ = false; float aim_ = 0.0f, aimV_ = 0.0f;
    float vehDist_ = 52.0f, vehUp_ = 10.0f;   // câmera de veículo: m atrás e acima (por navio)
    bool vehicle_ = false; float heading_ = 0, veh_ = 0, vehV_ = 0, vehIdle_ = 9.0f;   // modo veículo: veh_ = mistura suave 0..1

    bool init_ = false, hide_ = false, wasGrounded_ = true;
    Vec3 pos_{}, posVel_{}, ahead_{}, aheadVel_{}, lastTarget_{};
    float dip_ = 0, dipVel_ = 0, lastVy_ = 0;
    float dist_ = 4.2f, distV_ = 0, shoulderOff_ = 0, shoulderV_ = 0, pivotH_ = 1.55f, pivotHV_ = 0, fovH_ = 92, fovHV_ = 0;
    float fp_ = 0, fpV_ = 0, sprint_ = 0, sprintV_ = 0, air_ = 0, airV_ = 0;
    float arm_ = 4.2f, armV_ = 0;
    float trauma_ = 0, time_ = 0, bobPhase_ = 0;
    Camera cam_;
    CameraInfo info_;
};

}  // namespace e0
