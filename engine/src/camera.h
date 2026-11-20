// Isolated - câmera (posição + yaw/pitch/roll). Quem decide onde ela fica é o CameraRig (camera_rig.h).
#pragma once
#include "vecmath.h"

namespace e0 {

struct Camera {
    Vec3 position{0.0f, 1.7f, 0.0f};
    float yaw = 0.0f;     // radianos; 0 olha para -Z
    float pitch = -0.05f; // radianos (negativo = olhando para baixo)
    float roll = 0.0f;    // radianos (tremor de câmera)
    float fovY = 1.0f;    // vertical, radianos (o CameraRig calcula a partir do FOV horizontal e do aspecto)
    float zNear = 0.1f, zFar = 500.0f;

    Vec3 forward() const {  // direção de visão
        return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }
    Vec3 forwardFlat() const { return {std::sin(yaw), 0.0f, -std::cos(yaw)}; }
    Vec3 rightFlat() const { return {std::cos(yaw), 0.0f, std::sin(yaw)}; }
    Vec3 rightVec() const { return normalize(cross(forward(), {0, 1, 0})); }
    Vec3 upVec() const {  // "para cima" da câmera, já com o roll
        Vec3 r = rightVec(), u = cross(r, forward());
        return u * std::cos(roll) + r * std::sin(roll);
    }

    Mat4 view() const { return Mat4::lookAt(position, position + forward(), upVec()); }
    Mat4 projection(float aspect) const { return Mat4::perspective(fovY, aspect, zNear, zFar); }
};

}  // namespace e0
