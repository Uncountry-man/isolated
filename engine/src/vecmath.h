// Isolated - matemática mínima (column-major, compatível com OpenGL)
#pragma once
#include <cmath>

namespace e0 {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 normalize(const Vec3& v) {
    float l = std::sqrt(dot(v, v));
    return l > 1e-8f ? v * (1.0f / l) : Vec3{};
}

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }

    static Mat4 translate(const Vec3& t) { Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r; }
    static Mat4 scale(const Vec3& s) { Mat4 r; r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; return r; }
    static Mat4 rotY(float a) {
        Mat4 r;
        r.m[0] = std::cos(a); r.m[2] = -std::sin(a);
        r.m[8] = std::sin(a); r.m[10] = std::cos(a);
        return r;
    }

    static Mat4 rotX(float a) {   // +a leva +Z para -Y
        Mat4 r;
        r.m[5] = std::cos(a); r.m[6] = std::sin(a);
        r.m[9] = -std::sin(a); r.m[10] = std::cos(a);
        return r;
    }
    static Mat4 rotZ(float a) {   // +a leva +X para +Y
        Mat4 r;
        r.m[0] = std::cos(a); r.m[1] = std::sin(a);
        r.m[4] = -std::sin(a); r.m[5] = std::cos(a);
        return r;
    }

    static Mat4 ortho(float l, float r, float b, float t, float n, float f) {
        Mat4 m;
        m.m[0] = 2 / (r - l); m.m[5] = 2 / (t - b); m.m[10] = -2 / (f - n);
        m.m[12] = -(r + l) / (r - l); m.m[13] = -(t + b) / (t - b); m.m[14] = -(f + n) / (f - n);
        return m;
    }

    static Mat4 perspective(float fovyRad, float aspect, float zn, float zf) {
        Mat4 r;
        float f = 1.0f / std::tan(fovyRad * 0.5f);
        for (float& v : r.m) v = 0;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zf + zn) / (zn - zf);
        r.m[11] = -1;
        r.m[14] = (2 * zf * zn) / (zn - zf);
        return r;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = normalize(center - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] = dot(f, eye);
        return r;
    }
};

}  // namespace e0
