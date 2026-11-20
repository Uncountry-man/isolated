#include "anim.h"
#include "fist_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace e0 {

namespace {

inline Quat normalizeQuat(const Quat& q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-8f) return {0, 0, 0, 1};
    float inv = 1.0f / l;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline Quat nlerpQuat(const Quat& a, const Quat& b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    float s = d < 0.0f ? -1.0f : 1.0f;
    float u = 1.0f - t, v = t * s;
    return normalizeQuat({a.x * u + b.x * v, a.y * u + b.y * v, a.z * u + b.z * v, a.w * u + b.w * v});
}

inline BoneTransform lerpBone(const BoneTransform& a, const BoneTransform& b, float t) {
    BoneTransform r;
    r.t = a.t + (b.t - a.t) * t;
    r.q = nlerpQuat(a.q, b.q, t);
    return r;
}

inline Mat4 boneToMat4(const BoneTransform& bt) {
    const Quat& q = bt.q;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m;
    m.m[0] = 1.0f - 2.0f * (yy + zz);
    m.m[1] = 2.0f * (xy + wz);
    m.m[2] = 2.0f * (xz - wy);
    m.m[3] = 0.0f;

    m.m[4] = 2.0f * (xy - wz);
    m.m[5] = 1.0f - 2.0f * (xx + zz);
    m.m[6] = 2.0f * (yz + wx);
    m.m[7] = 0.0f;

    m.m[8]  = 2.0f * (xz + wy);
    m.m[9]  = 2.0f * (yz - wx);
    m.m[10] = 1.0f - 2.0f * (xx + yy);
    m.m[11] = 0.0f;

    m.m[12] = bt.t.x;
    m.m[13] = bt.t.y;
    m.m[14] = bt.t.z;
    m.m[15] = 1.0f;
    return m;
}

inline Mat4 mulAffine(const Mat4& a, const Mat4& b) {
    Mat4 r;
    r.m[0]  = a.m[0] * b.m[0]  + a.m[4] * b.m[1]  + a.m[8]  * b.m[2];
    r.m[1]  = a.m[1] * b.m[0]  + a.m[5] * b.m[1]  + a.m[9]  * b.m[2];
    r.m[2]  = a.m[2] * b.m[0]  + a.m[6] * b.m[1]  + a.m[10] * b.m[2];
    r.m[3]  = 0.0f;

    r.m[4]  = a.m[0] * b.m[4]  + a.m[4] * b.m[5]  + a.m[8]  * b.m[6];
    r.m[5]  = a.m[1] * b.m[4]  + a.m[5] * b.m[5]  + a.m[9]  * b.m[6];
    r.m[6]  = a.m[2] * b.m[4]  + a.m[6] * b.m[5]  + a.m[10] * b.m[6];
    r.m[7]  = 0.0f;

    r.m[8]  = a.m[0] * b.m[8]  + a.m[4] * b.m[9]  + a.m[8]  * b.m[10];
    r.m[9]  = a.m[1] * b.m[8]  + a.m[5] * b.m[9]  + a.m[9]  * b.m[10];
    r.m[10] = a.m[2] * b.m[8]  + a.m[6] * b.m[9]  + a.m[10] * b.m[10];
    r.m[11] = 0.0f;

    r.m[12] = a.m[0] * b.m[12] + a.m[4] * b.m[13] + a.m[8]  * b.m[14] + a.m[12];
    r.m[13] = a.m[1] * b.m[12] + a.m[5] * b.m[13] + a.m[9]  * b.m[14] + a.m[13];
    r.m[14] = a.m[2] * b.m[12] + a.m[6] * b.m[13] + a.m[10] * b.m[14] + a.m[14];
    r.m[15] = 1.0f;
    return r;
}

inline Quat qmul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat qconj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat qaxis(float x, float y, float z, float ang) { float s = std::sin(ang * 0.5f); return {x * s, y * s, z * s, std::cos(ang * 0.5f)}; }
inline float approach(float v, float target, float step) { return v < target ? std::min(target, v + step) : std::max(target, v - step); }

inline BoneTransform unpackBone(const uint8_t* p) {
    BoneTransform bt;
    float tf[3];
    int16_t qi[4];
    std::memcpy(tf, p, 12);
    std::memcpy(qi, p + 12, 8);
    bt.t = {tf[0], tf[1], tf[2]};
    constexpr float kInv = 1.0f / 32767.0f;
    bt.q = normalizeQuat({qi[0] * kInv, qi[1] * kInv, qi[2] * kInv, qi[3] * kInv});
    return bt;
}

}  // namespace

const char* AnimBank::clipLabel(int idx) {
    static const char* kNames[ClipCount] = {
        "PARADO", "ANDAR", "CORRER", "PULAR", "CAIR", "POUSAR",
        "BATER 1", "BATER 2", "BATER 3", "CORTAR", "MINERAR", "COLETAR",
        "BOIAR", "NADAR", "PICARETA", "ARMADO PARADO", "ARMADO ANDAR", "ARMADO CORRER", "MORRER", "RESGATE", "LEVANTAR", "BRACO CARREGAR", "OMBRO",
        "ARMA PARADO", "ARMA MIRAR", "ARMA ANDAR", "ARMA TRAS", "ARMA ESQ", "ARMA DIR", "ARMA CORRER", "RECARREGAR",
        "VIDLE 1", "VIDLE 2", "VIDLE 3", "VIDLE 4", "VIDLE 5", "VIDLE 6"
    };
    return (idx >= 0 && idx < ClipCount) ? kNames[idx] : "?";
}

bool AnimBank::loadFromMemory(const uint8_t* b, size_t len) {
    clips_.clear();
    numBones_ = 0;
    size_t o = 0;
    auto need = [&](size_t n) { return o + n <= len; };
    if (!need(12) || std::memcmp(b, "E0A1", 4) != 0) return false;
    uint32_t nb = 0, nc = 0;
    std::memcpy(&nb, b + 4, 4);
    std::memcpy(&nc, b + 8, 4);
    o = 12;
    if (nb != (uint32_t)kMaxBones || nc == 0 || nc > 64) return false;

    size_t parentAligned = (nb + 3u) & ~3u;
    if (!need(parentAligned + nb * 64u + nb * 20u)) return false;
    std::memcpy(parents_, b + o, nb);
    for (uint32_t i = 0; i < nb; ++i)
        if (parents_[i] >= (int)i || parents_[i] < -1) return false;
    o += parentAligned;

    for (uint32_t i = 0; i < nb; ++i) {
        std::memcpy(invBind_[i].m, b + o, 64);
        o += 64;
    }
    for (uint32_t i = 0; i < nb; ++i) {
        rest_[i] = unpackBone(b + o);
        o += 20;
    }

    clips_.resize(nc);
    for (uint32_t c = 0; c < nc; ++c) {
        if (!need(28)) { clips_.clear(); return false; }
        Clip& cl = clips_[c];
        std::memcpy(cl.name, b + o, 16);
        cl.name[15] = '\0';
        uint32_t nfr = 0, lp = 0;
        std::memcpy(&cl.fps, b + o + 16, 4);
        std::memcpy(&nfr, b + o + 20, 4);
        std::memcpy(&lp, b + o + 24, 4);
        o += 28;
        if (nfr == 0 || nfr > 4096 || cl.fps <= 0.0f) { clips_.clear(); return false; }
        size_t fb = (size_t)nfr * nb * 20u;
        if (!need(fb)) { clips_.clear(); return false; }
        cl.numFrames = nfr;
        cl.loop = lp != 0;
        cl.duration = nfr > 1 ? (float)(nfr - 1) / cl.fps : 1.0f / cl.fps;
        cl.frames.resize((size_t)nfr * nb);
        for (size_t i = 0; i < (size_t)nfr * nb; ++i) {
            cl.frames[i] = unpackBone(b + o);
            o += 20;
        }
    }
    numBones_ = (int)nb;
    return true;
}

bool AnimBank::loadFromFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)sz);
    size_t rd = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return rd == buf.size() && loadFromMemory(buf.data(), buf.size());
}

void AnimBank::restPose(BoneTransform* outPose) const {
    for (int i = 0; i < kMaxBones; ++i) outPose[i] = rest_[i];
}

void AnimBank::sampleTime(int clipIdx, float timeSec, BoneTransform* outPose) const {
    if (!valid() || clipIdx < 0 || clipIdx >= (int)clips_.size()) {
        restPose(outPose);
        return;
    }
    const Clip& c = clips_[clipIdx];
    if (c.numFrames <= 1) {
        std::memcpy(outPose, c.frames.data(), sizeof(BoneTransform) * kMaxBones);
        return;
    }
    float maxF = (float)(c.numFrames - 1);
    float f = timeSec * c.fps;
    if (c.loop) {
        f = std::fmod(f, maxF);
        if (f < 0.0f) f += maxF;
    } else {
        f = std::max(0.0f, std::min(maxF, f));
    }
    int f0 = (int)std::floor(f);
    if (f0 >= (int)c.numFrames - 1) f0 = (int)c.numFrames - 2;
    if (f0 < 0) f0 = 0;
    int f1 = f0 + 1;
    float frac = f - (float)f0;
    const BoneTransform* p0 = &c.frames[(size_t)f0 * kMaxBones];
    const BoneTransform* p1 = &c.frames[(size_t)f1 * kMaxBones];
    for (int i = 0; i < kMaxBones; ++i) outPose[i] = lerpBone(p0[i], p1[i], frac);
}

void AnimBank::samplePhase(int clipIdx, float phase01, BoneTransform* outPose) const {
    if (!valid() || clipIdx < 0 || clipIdx >= (int)clips_.size()) {
        restPose(outPose);
        return;
    }
    float p = phase01 - std::floor(phase01);
    sampleTime(clipIdx, p * clips_[clipIdx].duration, outPose);
}

void AnimBank::blendPose(const BoneTransform* a, const BoneTransform* b, float t, BoneTransform* out) {
    t = std::max(0.0f, std::min(1.0f, t));
    for (int i = 0; i < kMaxBones; ++i) out[i] = lerpBone(a[i], b[i], t);
}

void AnimBank::blendUpperBody(const BoneTransform* base, const BoneTransform* upper, float t, BoneTransform* out) {
    t = std::max(0.0f, std::min(1.0f, t));
    out[0] = base[0];  // 0 = Hips (mantém locomoção)
    for (int i = 1; i <= 54; ++i) out[i] = lerpBone(base[i], upper[i], t);  // 1..54 = Spine até mãos/cabeça
    for (int i = 55; i < kMaxBones; ++i) out[i] = base[i];                  // 55..64 = pernas
}

namespace {
inline void affineRows(const Mat4& A, const Mat4& B, Vec4* o) {   // linhas 0..2 de A * B (A e B afins)
    o[0] = {A.m[0] * B.m[0] + A.m[4] * B.m[1] + A.m[8] * B.m[2], A.m[0] * B.m[4] + A.m[4] * B.m[5] + A.m[8] * B.m[6],
            A.m[0] * B.m[8] + A.m[4] * B.m[9] + A.m[8] * B.m[10], A.m[0] * B.m[12] + A.m[4] * B.m[13] + A.m[8] * B.m[14] + A.m[12]};
    o[1] = {A.m[1] * B.m[0] + A.m[5] * B.m[1] + A.m[9] * B.m[2], A.m[1] * B.m[4] + A.m[5] * B.m[5] + A.m[9] * B.m[6],
            A.m[1] * B.m[8] + A.m[5] * B.m[9] + A.m[9] * B.m[10], A.m[1] * B.m[12] + A.m[5] * B.m[13] + A.m[9] * B.m[14] + A.m[13]};
    o[2] = {A.m[2] * B.m[0] + A.m[6] * B.m[1] + A.m[10] * B.m[2], A.m[2] * B.m[4] + A.m[6] * B.m[5] + A.m[10] * B.m[6],
            A.m[2] * B.m[8] + A.m[6] * B.m[9] + A.m[10] * B.m[10], A.m[2] * B.m[12] + A.m[6] * B.m[13] + A.m[10] * B.m[14] + A.m[14]};
}
}  // namespace

void AnimBank::computeSkinning(const BoneTransform* localPose, Vec4* outRows195, const Skeleton* sk, Vec4* attachRows) const {
    if (!valid()) { fillIdentityRows(outRows195); if (attachRows) fillIdentityRows(attachRows); return; }
    Mat4 G[kMaxBones];
    for (int i = 0; i < kMaxBones; ++i) {
        BoneTransform bt = localPose[i];
        if (sk) bt.t = (i == 0) ? sk->rootRest + (bt.t - sk->ybotRoot) * sk->hipScale : sk->t[i];
        Mat4 L = boneToMat4(bt);
        int p = parents_[i];
        G[i] = (p < 0) ? L : mulAffine(G[p], L);
        affineRows(G[i], sk ? sk->invBind[i] : invBind_[i], outRows195 + i * 3);
        if (attachRows) affineRows(G[i], sk ? sk->attach[i] : invBind_[i], attachRows + i * 3);
    }
}

namespace {
inline Vec3 qrot(const Quat& q, const Vec3& v) {   // gira v por q
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
inline Quat qbetween(Vec3 a, Vec3 b) {           // menor giro que leva a direção a até b
    const float la = std::sqrt(dot(a, a)), lb = std::sqrt(dot(b, b));
    if (la < 1e-6f || lb < 1e-6f) return {0, 0, 0, 1};
    a = a * (1.0f / la); b = b * (1.0f / lb);
    const float c = dot(a, b);
    if (c < -0.9999f) {
        Vec3 ax = cross(a, std::fabs(a.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
        ax = ax * (1.0f / std::sqrt(dot(ax, ax)));
        return {ax.x, ax.y, ax.z, 0.0f};
    }
    const Vec3 v = cross(a, b);
    return normalizeQuat({v.x, v.y, v.z, 1.0f + c});
}
inline Vec3 invBindPos(const Mat4& B) {          // junta de repouso = translação da inversa de um invBind rígido
    return {-(B.m[0] * B.m[12] + B.m[1] * B.m[13] + B.m[2] * B.m[14]),
            -(B.m[4] * B.m[12] + B.m[5] * B.m[13] + B.m[6] * B.m[14]),
            -(B.m[8] * B.m[12] + B.m[9] * B.m[13] + B.m[10] * B.m[14])};
}
inline Vec3 xform(const Mat4& M, const Vec3& p) {
    return {M.m[0] * p.x + M.m[4] * p.y + M.m[8] * p.z + M.m[12], M.m[1] * p.x + M.m[5] * p.y + M.m[9] * p.z + M.m[13],
            M.m[2] * p.x + M.m[6] * p.y + M.m[10] * p.z + M.m[14]};
}
}  // namespace

void AnimBank::armIK(BoneTransform* pose, const Skeleton& sk, float wR, float wL) const {
    if (!valid() || (wR <= 0.0f && wL <= 0.0f)) return;
    Quat gq[kMaxBones]; Vec3 gy[kMaxBones], gp[kMaxBones];   // rotação global (igual nos dois) e juntas do Y Bot / do personagem
    auto globals = [&]() {
        for (int i = 0; i < kMaxBones; ++i) {
            const int p = parents_[i];
            if (p < 0) { gq[i] = pose[i].q; gy[i] = pose[i].t; gp[i] = sk.rootRest + (pose[i].t - sk.ybotRoot) * sk.hipScale; continue; }
            gq[i] = qmul(gq[p], pose[i].q);
            gy[i] = gy[p] + qrot(gq[p], pose[i].t);
            gp[i] = gp[p] + qrot(gq[p], sk.t[i]);
        }
    };
    globals();
    // pegada (centro do punho) de cada mão no espaço local da mão: attach = T(d) * invBind do Y Bot  =>  d = attach * junta de repouso do Y Bot
    const Vec3 dR = xform(sk.attach[34], invBindPos(invBind_[34])), dL = xform(sk.attach[10], invBindPos(invBind_[10]));
    const Vec3 gunR = gp[32] + (gy[34] - gy[32]);                          // onde a mão direita do Y Bot segura, a partir do ombro do personagem
    const Vec3 relL = qrot(qconj(gq[34]), gy[10] - gy[34]);                // mão esquerda do Y Bot no espaço da mão direita (= na arma)
    const Quat qL = gq[10], qR = gq[34];
    auto solve = [&](int s, int e, int h, Vec3 T, float w, Vec3 hint) {
        if (w <= 0.0f) return;
        const Vec3 S = gp[s], E = gp[e], H = gp[h];
        T = H + (T - H) * std::min(1.0f, w);
        const float a = std::sqrt(dot(E - S, E - S)), b = std::sqrt(dot(H - E, H - E));
        Vec3 st = T - S; float d = std::sqrt(dot(st, st));
        if (d < 1e-5f || a < 1e-5f || b < 1e-5f) return;
        const Vec3 dir = st * (1.0f / d);
        d = std::max(std::fabs(a - b) + 1e-3f, std::min((a + b) * 0.999f, d));
        Vec3 pole = hint - dir * dot(hint, dir);                           // lado para onde o cotovelo dobra: o do cotovelo do Y Bot na animação
        float pl = std::sqrt(dot(pole, pole));
        if (pl < 1e-5f) { pole = cross(dir, Vec3{0, 1, 0}); pl = std::sqrt(dot(pole, pole)); if (pl < 1e-5f) return; }
        pole = pole * (1.0f / pl);
        const float ca = std::max(-1.0f, std::min(1.0f, (a * a + d * d - b * b) / (2.0f * a * d)));
        const Vec3 E2 = S + (dir * ca + pole * std::sqrt(std::max(0.0f, 1.0f - ca * ca))) * a;
        const Quat rU = qbetween(E - S, E2 - S);
        const Vec3 H1 = E2 + qrot(rU, H - E);
        const Quat rF = qbetween(H1 - E2, (S + dir * d) - E2);
        const Quat gs = normalizeQuat(qmul(rU, gq[s])), ge = normalizeQuat(qmul(rF, qmul(rU, gq[e])));
        const Quat gh = gq[h];                                             // a mão mantém a rotação global da animação
        pose[s].q = normalizeQuat(qmul(qconj(gq[parents_[s]]), gs));
        pose[e].q = normalizeQuat(qmul(qconj(gs), ge));
        pose[h].q = normalizeQuat(qmul(qconj(ge), gh));
    };
    solve(32, 33, 34, gunR - qrot(qR, dR), wR, gy[33] - gy[32]);
    globals();
    const Vec3 gunNow = gp[34] + qrot(gq[34], dR);                         // a arma acompanha a mão direita (já resolvida)
    solve(8, 9, 10, gunNow + qrot(gq[34], relL) - qrot(qL, dL), wL, gy[9] - gy[8]);
}

// E0K1 (tools/export_rig.py): "E0K1" | u32 nBones | f32 invBind[n*16] | f32 t[n*3] | f32 rootRest[3] | f32 ybotRoot[3] | f32 hipScale | f32 attach[n*16]
bool Skeleton::loadFromMemory(const uint8_t* b, size_t len) {
    valid = false;
    const size_t need = 8 + kMaxBones * 64 + kMaxBones * 12 + 28 + kMaxBones * 64;
    if (len < need || std::memcmp(b, "E0K1", 4) != 0) return false;
    uint32_t nb = 0; std::memcpy(&nb, b + 4, 4);
    if (nb != (uint32_t)kMaxBones) return false;
    size_t o = 8;
    for (int i = 0; i < kMaxBones; ++i, o += 64) std::memcpy(invBind[i].m, b + o, 64);
    for (int i = 0; i < kMaxBones; ++i, o += 12) std::memcpy(&t[i].x, b + o, 12);
    std::memcpy(&rootRest.x, b + o, 12); o += 12;
    std::memcpy(&ybotRoot.x, b + o, 12); o += 12;
    std::memcpy(&hipScale, b + o, 4); o += 4;
    for (int i = 0; i < kMaxBones; ++i, o += 64) std::memcpy(attach[i].m, b + o, 64);
    valid = hipScale > 0.3f && hipScale < 3.0f;
    return valid;
}

void AnimBank::fillIdentityRows(Vec4* outRows195) {
    for (int i = 0; i < kMaxBones; ++i) {
        outRows195[i * 3 + 0] = {1, 0, 0, 0};
        outRows195[i * 3 + 1] = {0, 1, 0, 0};
        outRows195[i * 3 + 2] = {0, 0, 1, 0};
    }
}

void AnimPlayer::reset() {
    const Skeleton* keep = skSet_;
    *this = AnimPlayer{};
    skSet_ = keep;
    AnimBank::fillIdentityRows(rows_);
    AnimBank::fillIdentityRows(attach_);
}

void AnimPlayer::update(float dt, const AnimInput& in, const AnimBank& bank, bool computeMatrices) {
    sk_ = skSet_ ? skSet_ : bank.defaultSkeleton();
    if (sk_ && !sk_->valid) sk_ = nullptr;
    if (!bank.valid()) {
        if (!init_) { AnimBank::fillIdentityRows(rows_); init_ = true; }
        activeName_ = "SEM ANIM";
        return;
    }
    if (!init_) {
        wasGrounded_ = in.grounded;
        smoothSpeed_ = in.speed;
        init_ = true;
    }

    BoneTransform pose[kMaxBones], tmpA[kMaxBones], tmpB[kMaxBones];

    // Morto: MORRER toca uma vez (cai no chão) e segura o último quadro; sobrepõe qualquer outra camada
    if (in.dead && bank.numClips() > ClipDeath) {
        wasDead_ = true; gettingUp_ = false; rescueT_ = 0.0f;
        deadT_ += dt;
        const int dc = (in.carried && bank.numClips() > ClipDeathFwd) ? ClipDeathFwd : ClipDeath;   // carregado = de bruços, quadro final
        float dur = std::max(0.01f, bank.clipDuration(dc));
        toolW_ = 0.0f;
        if (computeMatrices) {
            bank.sampleTime(dc, in.carried ? dur : std::min(deadT_, dur), pose);
            bank.computeSkinning(pose, rows_, sk_, attach_);
        }
        activeName_ = AnimBank::clipLabel(ClipDeath);
        activePhase_ = std::min(1.0f, deadT_ / dur);
        wasGrounded_ = in.grounded;
        return;
    }
    deadT_ = 0.0f;
    if (wasDead_) { wasDead_ = false; if (bank.numClips() > ClipGetUp) { gettingUp_ = true; getUpT_ = 0.0f; } }   // voltou à vida: levanta do chão
    if (gettingUp_) {
        getUpT_ += dt;
        float dur = std::max(0.01f, bank.clipDuration(ClipGetUp));
        const bool endNow = getUpT_ >= std::min(dur, kGetUpEnd);
        toolW_ = 0.0f;
        if (computeMatrices || endNow) {
            bank.sampleTime(ClipGetUp, std::min(getUpT_, dur), pose);
            if (computeMatrices) bank.computeSkinning(pose, rows_, sk_, attach_);
        }
        activeName_ = AnimBank::clipLabel(ClipGetUp);
        activePhase_ = std::min(1.0f, getUpT_ / dur);
        wasGrounded_ = in.grounded;
        // O clipe original termina com ~0,6 s de pessoa PARADA em pé: corta no instante em que ela acabou de levantar (kGetUpEnd) e dissolve na locomoção
        if (endNow) { gettingUp_ = false; for (int i = 0; i < kMaxBones; ++i) guPose_[i] = pose[i]; guW_ = 1.0f; }
        return;
    }
    if (in.rescuing && bank.numClips() > ClipRescue) {   // RCP em loop
        rescueT_ += dt;
        float dur = std::max(0.01f, bank.clipDuration(ClipRescue));
        toolW_ = 0.0f;
        if (computeMatrices) {
            bank.sampleTime(ClipRescue, std::fmod(rescueT_, dur), pose);
            bank.computeSkinning(pose, rows_, sk_, attach_);
        }
        activeName_ = AnimBank::clipLabel(ClipRescue);
        activePhase_ = std::fmod(rescueT_, dur) / dur;
        wasGrounded_ = in.grounded;
        return;
    }
    rescueT_ = 0.0f;

    // Modo fixo (debug): reproduz continuamente o clipe selecionado
    if (in.forceClip >= 0 && in.forceClip < ClipCount) {
        if (in.forceClip != forceCur_) {   // troca de clipe: o anterior continua tocando e some em 0,6 s
            if (forceCur_ >= 0) { forcePrev_ = forceCur_; forcePrevT_ = debugTime_; forceFade_ = 0.0f; }
            forceCur_ = in.forceClip; debugTime_ = 0.0f;
        }
        debugTime_ += dt;
        forcePrevT_ += dt;
        forceFade_ = std::min(1.0f, forceFade_ + dt / 0.6f);
        float dur = std::max(0.01f, bank.clipDuration(in.forceClip));
        float t = std::fmod(debugTime_, dur);
        toolW_ = (in.tool != 0 && !in.swimming) ? 1.0f : 0.0f;
        if (computeMatrices) {
            bank.sampleTime(in.forceClip, t, pose);
            if (forceFade_ < 1.0f && forcePrev_ >= 0) {
                const float pd = std::max(0.01f, bank.clipDuration(forcePrev_));
                bank.sampleTime(forcePrev_, std::fmod(forcePrevT_, pd), tmpA);
                const float k = forceFade_ * forceFade_ * (3.0f - 2.0f * forceFade_);
                AnimBank::blendPose(tmpA, pose, k, tmpB);
                std::memcpy(pose, tmpB, sizeof(pose));
            }
            applyFist(toolW_, pose);
            bank.computeSkinning(pose, rows_, sk_, attach_);
        }
        activeName_ = AnimBank::clipLabel(in.forceClip);
        activePhase_ = t / dur;
        wasGrounded_ = in.grounded;
        return;
    }

    // ---- 1) Camada de locomoção no chão (IDLE <-> WALK <-> RUN sincronizados em fase) ----
    float spdAlpha = 1.0f - std::exp(-dt * 14.0f);
    smoothSpeed_ += (in.speed - smoothSpeed_) * spdAlpha;
    if (in.speed < 0.01f && smoothSpeed_ < 0.03f) smoothSpeed_ = 0.0f;

    idleTime_ += dt;
    if (smoothSpeed_ > 0.03f) {
        // Frequência do ciclo: ~1.10 Hz andando (3 m/s), ~1.62 Hz correndo (6 m/s)
        float runT = std::max(0.0f, std::min(1.0f, (smoothSpeed_ - 3.0f) / 3.0f));
        float walkScale = std::max(0.42f, std::min(1.0f, smoothSpeed_ / 3.0f));
        float freq = (smoothSpeed_ <= 3.0f) ? (1.10f * walkScale) : (1.10f + (1.62f - 1.10f) * runT);
        locoPhase_ = std::fmod(locoPhase_ + dt * freq, 1.0f);
    } else {
        locoPhase_ = 0.0f;  // recomeça no início do passo ao voltar a andar
    }

    if (smoothSpeed_ <= 0.03f) {
        if (computeMatrices) bank.sampleTime(ClipIdle, idleTime_, pose);
        activeName_ = "PARADO";
        float dur = std::max(0.01f, bank.clipDuration(ClipIdle));
        activePhase_ = std::fmod(idleTime_, dur) / dur;
    } else if (smoothSpeed_ <= 3.15f) {
        float w = std::max(0.0f, std::min(1.0f, smoothSpeed_ / 1.6f));
        if (computeMatrices) {
            bank.sampleTime(ClipIdle, idleTime_, tmpA);
            bank.samplePhase(ClipWalk, locoPhase_, tmpB);
            AnimBank::blendPose(tmpA, tmpB, w, pose);
        }
        activeName_ = "ANDAR";
        activePhase_ = locoPhase_;
    } else {
        float r = std::max(0.0f, std::min(1.0f, (smoothSpeed_ - 3.0f) / 2.6f));
        if (computeMatrices) {
            bank.samplePhase(ClipWalk, locoPhase_, tmpA);
            bank.samplePhase(ClipRun, locoPhase_, tmpB);
            AnimBank::blendPose(tmpA, tmpB, r, pose);
        }
        activeName_ = r > 0.4f ? "CORRER" : "ANDAR";
        activePhase_ = locoPhase_;
    }

    // ---- 1b) BACAMARTE: locomoção com a arma (Pro Rifle Pack) no corpo todo; mirando = corpo de frente para a mira e andar em 4 direções ----
    const bool gun = in.tool == 5 && !in.swimming;
    gunW_ = approach(gunW_, gun ? 1.0f : 0.0f, dt / 0.18f);
    aimW_ = approach(aimW_, (gun && in.aiming && !in.reloading) ? 1.0f : 0.0f, dt / 0.14f);
    {
        float d = std::remainder(in.moveAngle - angS_, 6.2831853f);
        angS_ = std::remainder(angS_ + d * (1.0f - std::exp(-dt * 10.0f)), 6.2831853f);
    }
    BoneTransform gp[kMaxBones];
    const bool haveGun = gunW_ > 0.0f && computeMatrices;
    if (haveGun) {
        gunPose(in, bank, gp);
        AnimBank::blendPose(pose, gp, gunW_, pose);
        if (gunW_ > 0.5f && smoothSpeed_ > 0.03f) activeName_ = aimW_ > 0.5f ? "ARMA MIRAR" : (smoothSpeed_ > 4.0f ? "ARMA CORRER" : "ARMA ANDAR");
        else if (gunW_ > 0.5f) activeName_ = aimW_ > 0.5f ? "ARMA MIRAR" : "ARMA PARADO";
    }

    // ---- 2) Camada aérea e pouso (JUMP -> FALL -> LAND) ----
    if (!in.grounded) {
        if (wasGrounded_) {
            airTime_ = 0.0f;
            jumped_ = (in.vy > 1.2f);
            landing_ = false;
            landWeight_ = 0.0f;
        }
        airTime_ += dt;
        airWeight_ += (1.0f - airWeight_) * (1.0f - std::exp(-dt * 18.0f));
    } else {
        if (!wasGrounded_ && airTime_ > 0.10f && !in.swimming) {
            landing_ = true;
            landTime_ = 0.0f;
            landWeight_ = 1.0f;
        }
        airWeight_ += (0.0f - airWeight_) * (1.0f - std::exp(-dt * 16.0f));
        if (airWeight_ < 0.005f) airWeight_ = 0.0f;
    }

    if (in.swimming) {
        landing_ = false;
        landWeight_ = 0.0f;
    }

    if (airWeight_ > 0.005f) {
        float jumpDur = bank.clipDuration(ClipJump);
        if (jumped_ && airTime_ < jumpDur + 0.08f) {
            if (computeMatrices) {
                bank.sampleTime(ClipJump, std::min(airTime_, jumpDur), tmpA);
                if (airTime_ > jumpDur * 0.6f) {
                    float fBlend = std::min(1.0f, (airTime_ - jumpDur * 0.6f) / (jumpDur * 0.4f + 0.08f));
                    bank.sampleTime(ClipFall, airTime_, tmpB);
                    AnimBank::blendPose(tmpA, tmpB, fBlend, tmpA);
                }
            }
            if (!in.grounded) { activeName_ = "PULAR"; activePhase_ = std::min(1.0f, airTime_ / std::max(0.01f, jumpDur)); }
        } else {
            if (computeMatrices) bank.sampleTime(ClipFall, airTime_, tmpA);
            if (!in.grounded) {
                activeName_ = "CAIR";
                float fd = std::max(0.01f, bank.clipDuration(ClipFall));
                activePhase_ = std::fmod(airTime_, fd) / fd;
            }
        }
        if (computeMatrices) AnimBank::blendPose(pose, tmpA, airWeight_, pose);
    }

    if (landing_) {
        landTime_ += dt;
        float fullDur = std::max(0.20f, bank.clipDuration(ClipLand));
        // Em movimento, amortecimento mais curto (~0.26s) para continuar a corrida com fluidez
        float effDur = (smoothSpeed_ > 0.8f) ? std::min(fullDur, 0.26f) : fullDur;
        if (landTime_ >= effDur) {
            landing_ = false;
            landWeight_ = 0.0f;
        } else {
            float u = landTime_ / effDur;
            float w = (u < 0.30f) ? 1.0f : (1.0f - (u - 0.30f) / 0.70f);
            w = w * w * (3.0f - 2.0f * w);
            landWeight_ = w;
            if (computeMatrices) {
                bank.sampleTime(ClipLand, landTime_ * (fullDur / effDur), tmpA);
                AnimBank::blendPose(pose, tmpA, landWeight_, pose);
            }
            if (landWeight_ > 0.35f && in.grounded) {
                activeName_ = "POUSAR";
                activePhase_ = u;
            }
        }
    }

    // ---- 2b) Ferramenta na mão: braço direito (ossos 31..54) dos clipes "armados" do Pro Melee Axe Pack, na mesma fase da
    //          locomoção (parado / andando / correndo); no ar segura a pose de parado. Some ao nadar (ferramenta guardada).
    {
        float target = (in.tool != 0 && !in.swimming) ? 1.0f : 0.0f;
        if (toolW_ != target) { float st = dt / 0.15f; toolW_ = (target > toolW_) ? std::min(target, toolW_ + st) : std::max(target, toolW_ - st); }
        if (haveGun && (airWeight_ > 0.005f || landWeight_ > 0.005f)) {   // no ar / pousando: as mãos continuam segurando a arma
            const float k = gunW_ * std::max(airWeight_, landWeight_);
            for (int i = 1; i <= 54; ++i) pose[i] = lerpBone(pose[i], gp[i], k);
        }
        if (toolW_ > 0.0f && computeMatrices && in.tool != 5) {   // (a arma usa a pose do Pro Rifle Pack, não a camada do braço)
            // Mistura contínua (sem ramos por limiar -> sem tremor); "no ar" só após 0.12 s para não oscilar em degraus/rampas.
            const bool air = !in.grounded && airTime_ > 0.12f;
            float w = air ? 0.0f : std::max(0.0f, std::min(1.0f, smoothSpeed_ / 1.6f));
            float r = air ? 0.0f : std::max(0.0f, std::min(1.0f, (smoothSpeed_ - 3.0f) / 2.6f));
            bank.sampleTime(ClipArmIdle, idleTime_, tmpA);
            if (w > 0.0f) {
                bank.samplePhase(ClipArmWalk, locoPhase_, tmpB);
                AnimBank::blendPose(tmpA, tmpB, w, tmpA);
            }
            if (r > 0.0f) {
                bank.samplePhase(ClipArmRun, locoPhase_, tmpB);
                AnimBank::blendPose(tmpA, tmpB, r, tmpA);
            }
            for (int i = 31; i <= 54; ++i) pose[i] = lerpBone(pose[i], tmpA[i], toolW_);
        }
    }

    // ---- 3) Camada de nado (BOIAR <-> NADAR) ----
    float swimTarget = in.swimming ? 1.0f : 0.0f;
    swimWeight_ += (swimTarget - swimWeight_) * (1.0f - std::exp(-dt * 12.0f));
    if (!in.swimming && swimWeight_ < 0.005f) swimWeight_ = 0.0f;
    if (in.swimming && swimWeight_ > 0.995f) swimWeight_ = 1.0f;

    if (swimWeight_ > 0.005f) {
        if (smoothSpeed_ > 0.04f) {
            float swimFreq = 0.65f + 0.50f * std::min(1.0f, smoothSpeed_ / 4.4f);
            swimPhase_ = std::fmod(swimPhase_ + dt * swimFreq, 1.0f);
        } else {
            swimPhase_ = 0.0f;
        }
        float idleDur = std::max(0.01f, bank.clipDuration(ClipSwimIdle));
        if (computeMatrices) bank.sampleTime(ClipSwimIdle, idleTime_, tmpA);
        if (smoothSpeed_ > 0.04f) {
            float wSwimMove = std::min(1.0f, smoothSpeed_ / 1.1f);
            if (computeMatrices) {
                bank.samplePhase(ClipSwim, swimPhase_, tmpB);
                AnimBank::blendPose(tmpA, tmpB, wSwimMove, tmpA);
            }
            if (swimWeight_ > 0.5f) {
                activeName_ = (wSwimMove > 0.35f) ? "NADAR" : "BOIAR";
                activePhase_ = (wSwimMove > 0.35f) ? swimPhase_ : (std::fmod(idleTime_, idleDur) / idleDur);
            }
        } else if (swimWeight_ > 0.5f) {
            activeName_ = "BOIAR";
            activePhase_ = std::fmod(idleTime_, idleDur) / idleDur;
        }
        if (computeMatrices) AnimBank::blendPose(pose, tmpA, swimWeight_, pose);
    }

    // ---- 4) Camada de ações de gameplay (BATER 1/2/3, CORTAR, MINERAR, COLETAR) ----
    if (in.triggerAction >= 1 && in.triggerAction <= 6 && in.grounded && !in.swimming) {
        // Golpe depende da ferramenta na mão (índices = PlayerAction: 1 bater, 2 cortar, 3 minerar, 4 coletar, 5/6 bater 2/3):
        //   sem ferramenta  -> socos (cortar/minerar com a mão nua = soco no tronco/pedra)
        //   machadinha      -> cortar = golpe horizontal no tronco (CORTAR); minerar = golpe por cima (MINERAR); bater alterna os dois
        //   picareta        -> minerar = ergue com as 2 mãos e crava no chão (PICARETA); cortar = golpe por cima; bater alterna
        static const int kMapNone[7]    = {-1, ClipPunch, ClipPunch2, ClipPunch3, ClipPickup, ClipPunch2, ClipPunch3};
        static const int kMapHatchet[7] = {-1, ClipMine, ClipChop, ClipMine, ClipPickup, ClipChop, ClipMine};
        static const int kMapPick[7]    = {-1, ClipToolMine, ClipMine, ClipToolMine, ClipPickup, ClipMine, ClipToolMine};
        const int* amap = (in.tool == 1 || in.tool == 3 || in.tool == 4) ? kMapHatchet : (in.tool == 2 ? kMapPick : kMapNone);
        int newClip = amap[in.triggerAction];
        if (actClip_ < 0 || actFadeOut_) {
            // Nenhuma ação ativa: inicia imediatamente
            actClip_ = newClip;
            startedClip_ = newClip;
            queuedClip_ = -1;
            prevActClip_ = -1;
            actTime_ = 0.0f;
            actFadeOut_ = false;
        } else if (queuedClip_ < 0 || newClip == ClipPickup) {
            // Ação em andamento: NUNCA cancela a atual; coloca no máximo 1 na fila (coleta tem prioridade na vaga)
            queuedClip_ = newClip;
        }
    }

    if (actClip_ >= 0) {
        actTime_ += dt;
        float dur = std::max(0.10f, bank.clipDuration(actClip_));
        if (!in.grounded || in.swimming || false) {
            actFadeOut_ = true;
            queuedClip_ = -1;
        }
        if (actTime_ >= dur && !actFadeOut_ && queuedClip_ >= 0) {
            // Terminou a animação atual e há 1 na fila: avança para a animação da fila
            prevActClip_ = actClip_;
            actClip_ = queuedClip_;
            startedClip_ = actClip_;
            queuedClip_ = -1;
            actTime_ = std::max(0.0f, actTime_ - dur);
            dur = std::max(0.10f, bank.clipDuration(actClip_));
        }
        if (actTime_ >= dur) {
            actClip_ = -1;
            queuedClip_ = -1;
            prevActClip_ = -1;
            actWeight_ = 0.0f;
            actFadeOut_ = false;
        } else {
            if (actFadeOut_) {
                actWeight_ += (0.0f - actWeight_) * (1.0f - std::exp(-dt * 14.0f));
                if (actWeight_ < 0.01f) { actClip_ = -1; queuedClip_ = -1; prevActClip_ = -1; actWeight_ = 0.0f; actFadeOut_ = false; }
            } else {
                // golpes de ferramenta começam/terminam na guarda do Pro Melee Axe Pack: transição um pouco mais longa
                const bool toolClip = (actClip_ == ClipChop || actClip_ == ClipMine || actClip_ == ClipToolMine);
                float fadeIn = (prevActClip_ >= 0) ? 1.0f : std::min(1.0f, actTime_ / (actClip_ == ClipToolMine ? 0.22f : (toolClip ? 0.14f : 0.09f)));
                float fadeOut = (queuedClip_ >= 0) ? 1.0f : std::min(1.0f, (dur - actTime_) / (toolClip ? 0.24f : 0.16f));
                actWeight_ = std::min(fadeIn, fadeOut);
            }
            if (actClip_ >= 0 && actWeight_ > 0.005f) {
                if (computeMatrices) {
                    bank.sampleTime(actClip_, actTime_, tmpA);
                    if (prevActClip_ >= 0 && actTime_ < 0.10f) {
                        bank.sampleTime(prevActClip_, bank.clipDuration(prevActClip_), tmpB);
                        AnimBank::blendPose(tmpB, tmpA, actTime_ / 0.10f, tmpA);
                    }
                    if (smoothSpeed_ > 0.20f) {
                        // Em movimento: aplica golpe no tronco/braços/cabeça e preserva as pernas andando/correndo
                        AnimBank::blendUpperBody(pose, tmpA, actWeight_, pose);
                    } else {
                        AnimBank::blendPose(pose, tmpA, actWeight_, pose);
                    }
                }
                activeName_ = AnimBank::clipLabel(actClip_);
                activePhase_ = std::min(1.0f, actTime_ / dur);
            }
        }
    }

    // ---- 5) BACAMARTE: recarga (tronco), inclinação da mira e coice ----
    if (in.reloading && !wasReloading_) reloadT_ = 0.0f;
    wasReloading_ = in.reloading;
    if (in.reloading) reloadT_ += dt;
    reloadW_ = approach(reloadW_, (gun && in.reloading) ? 1.0f : 0.0f, dt / 0.18f);
    if (reloadW_ > 0.0f && computeMatrices && bank.numClips() > ClipGunReload) {
        bank.sampleTime(ClipGunReload, std::min(reloadT_, bank.clipDuration(ClipGunReload)), tmpA);
        AnimBank::blendUpperBody(pose, tmpA, reloadW_ * gunW_, pose);
        if (reloadW_ > 0.5f) { activeName_ = "RECARREGAR"; activePhase_ = std::min(1.0f, reloadT_ / std::max(0.01f, bank.clipDuration(ClipGunReload))); }
    }
    if (in.fired) recoilT_ = 0.0f;
    recoilT_ = std::min(10.0f, recoilT_ + dt);
    pitchS_ += (in.aimPitch - pitchS_) * (1.0f - std::exp(-dt * 18.0f));
    if (haveGun) {
        const float kick = recoilT_ < 0.04f ? recoilT_ / 0.04f : std::exp(-(recoilT_ - 0.04f) * 9.0f);
        const float aw = aimW_ * gunW_;
        // o cano da pose de mira aponta 3,1° para a esquerda (export_gun.py): o giro de -0,054 rad no tronco o alinha com a mira
        pitchSpine(pose, std::max(-1.1f, std::min(1.1f, pitchS_)) * aw + 0.20f * kick * gunW_, -0.054f * aw);
    }

    wasGrounded_ = in.grounded;
    {   // CARREGANDO: braço direito fixo, dobrado segurando a perna, em cima de qualquer locomoção/ação (dedos seguem a locomoção)
        float tgt = (in.carrying && bank.numClips() > ClipArmUp) ? 1.0f : 0.0f;
        carryW_ += std::max(-1.0f, std::min(1.0f, (tgt - carryW_) * 1.0f)) * std::min(1.0f, dt / 0.20f);
        if (std::fabs(tgt - carryW_) < 0.01f) carryW_ = tgt;
        if (carryW_ > 0.0f && computeMatrices) {
            BoneTransform up[kMaxBones];
            bank.sampleTime(ClipArmUp, bank.clipDuration(ClipArmUp) * 0.5f, up);
            for (int i = 31; i <= 34; ++i) pose[i] = lerpBone(pose[i], up[i], carryW_);
        }
    }
    if (guW_ > 0.0f) {   // saída suave do LEVANTAR para a locomoção
        if (computeMatrices) {
            for (int i = 0; i < kMaxBones; ++i) pose[i] = lerpBone(pose[i], guPose_[i], guW_);
        }
        guW_ = std::max(0.0f, guW_ - dt / 0.25f);
    }
    if (computeMatrices) {
        applyFist(std::max(in.tool == 5 ? 0.0f : toolW_, carryW_), pose);   // carregando: a mão fecha na perna (a arma usa os dedos do próprio clipe)
        if (sk_ && gunW_ > 0.0f && !in.dead) bank.armIK(pose, *sk_, gunW_, gunW_ * (1.0f - reloadW_));   // braços curtos: mãos no lugar certo da arma
        bank.computeSkinning(pose, rows_, sk_, attach_);
    }
}

// Pose da arma (sem o ar): parado/andando/correndo com a arma pronta, misturada (aimW_) à pose de MIRA: GAIM parado ou as pernas das 4
// direções de andar (frente/trás/esquerda/direita pelo ângulo do movimento em relação ao corpo) com o tronco TRAVADO na orientação da mira
// (o quadril dos clipes de andar gira diferente do GAIM: a rotação global da coluna é mantida, então o cano não muda de direção ao andar).
void AnimPlayer::gunPose(const AnimInput& in, const AnimBank& bank, BoneTransform* out) {
    (void)in;
    BoneTransform a[kMaxBones], b[kMaxBones], d[kMaxBones];
    const float w = std::max(0.0f, std::min(1.0f, smoothSpeed_ / 1.6f));
    const float r = std::max(0.0f, std::min(1.0f, (smoothSpeed_ - 3.0f) / 2.6f));
    bank.sampleTime(ClipGunIdle, idleTime_, out);
    if (w > 0.0f) { bank.samplePhase(ClipGunWalk, locoPhase_, a); AnimBank::blendPose(out, a, w, out); }
    if (r > 0.0f) { bank.samplePhase(ClipGunRun, locoPhase_, a); AnimBank::blendPose(out, a, r, out); }
    if (aimW_ <= 0.0f) return;
    bank.sampleTime(ClipGunAim, idleTime_, a);
    if (w > 0.0f) {
        const float c = std::cos(angS_), s = std::sin(angS_);
        bank.samplePhase(c >= 0.0f ? ClipGunWalk : ClipGunBack, locoPhase_, b);
        bank.samplePhase(s >= 0.0f ? ClipGunRight : ClipGunLeft, locoPhase_, d);
        const float ac = std::fabs(c), as = std::fabs(s);
        AnimBank::blendPose(b, d, as / std::max(1e-4f, ac + as), b);
        AnimBank::blendPose(a, b, w, b);                       // pernas/quadril: parado -> andando
        b[1].t = a[1].t;
        b[1].q = normalizeQuat(qmul(qmul(qconj(b[0].q), a[0].q), a[1].q));   // coluna com a MESMA rotação global do GAIM
        for (int i = 2; i <= 54; ++i) b[i] = a[i];
        for (int i = 0; i < kMaxBones; ++i) a[i] = b[i];
    }
    AnimBank::blendPose(out, a, aimW_, out);
}

// Inclina o tronco (Spine, Spine1, Spine2 = ossos 1..3) em volta do eixo X do modelo (direita do personagem): + = para cima. `yaw` gira só o
// primeiro osso em volta de Y. Cada osso leva 1/3 do ângulo (curva suave da coluna).
void AnimPlayer::pitchSpine(BoneTransform* pose, float pitch, float yaw) {
    if (std::fabs(pitch) < 1e-4f && std::fabs(yaw) < 1e-4f) return;
    Quat G = pose[0].q;   // rotação global do quadril (raiz)
    for (int k = 1; k <= 3; ++k) {
        Quat R = qaxis(1.0f, 0.0f, 0.0f, pitch / 3.0f);
        if (k == 1) R = qmul(qaxis(0.0f, 1.0f, 0.0f, yaw), R);
        pose[k].q = normalizeQuat(qmul(qmul(qconj(G), qmul(R, G)), pose[k].q));
        G = qmul(G, pose[k].q);
    }
}

// Mão direita fechada ao empunhar ferramenta: troca os dedos (ossos 35..54) pela pegada real do Mixamo (fist_pose.h,
// Pro Melee Axe Pack/standing idle) - vale também durante os golpes, para a mão nunca abrir sobre o cabo.
void AnimPlayer::applyFist(float w, BoneTransform* pose) {
    if (w <= 0.0f) return;
    for (int i = 0; i < 20; ++i) {
        const float* f = kFistPose[i];
        BoneTransform fb; fb.t = {f[0], f[1], f[2]}; fb.q = {f[3], f[4], f[5], f[6]};
        pose[35 + i] = lerpBone(pose[35 + i], fb, w);
    }
}

}  // namespace e0
