#include "renderer.h"
#include "e0i.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "font5x7.h"
#include "ui_font.h"
#include "props_data.h"
#include "torch_tip.h"
#include "gun_tip.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <string>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated", __VA_ARGS__)

namespace e0 {

// ---------------------------------------------------------------------------------------------
// Perfis de qualidade. Custo extra por jogador no multiplayer: sombra = LOD baixo no mapa de sombra,
// e LOD por distância no passe principal. (Não há mais reflexo planar: a água só reflete o céu analiticamente.)
// ---------------------------------------------------------------------------------------------
struct QParams {
    int shadowRes;     // lado do mapa de sombra (px)
    float shadowR;     // raio coberto em torno do jogador (m)
    float pcf;         // 1 = 1 amostra (2x2 por hardware), 4 = 4 amostras (suave)
    float lodNear, lodFar;  // distâncias (m) de troca LOD0->LOD1->LOD2
    int shadowLod;     // LOD usado nos casters de sombra
    float viewDist, terrLod0, terrLod1, propDist;  // terreno: alcance, trocas de LOD, alcance dos objetos (m)
    float grassDist, grassDensity;                 // grama: alcance (m) e fração dos tufos
    float smallDist;                               // coletáveis (gravetos, pedrinhas): alcance (m)
};
static const QParams kQ[3] = {
    // lodNear < ~5 m => só o jogador local (câmera a ~4.7 m) usa o modelo completo; os demais usam LOD1/LOD2
    {512, 10.0f, 1.0f, 0.0f, 12.0f, 2, 300.0f, 55.0f, 140.0f, 110.0f, 28.0f, 0.35f, 28.0f},    // BAIXA: ninguém em LOD0
    {1024, 14.0f, 1.0f, 5.5f, 22.0f, 2, 380.0f, 80.0f, 180.0f, 170.0f, 48.0f, 0.70f, 55.0f},   // MEDIA
    {2048, 18.0f, 4.0f, 7.0f, 35.0f, 1, 450.0f, 110.0f, 240.0f, 230.0f, 70.0f, 1.00f, 85.0f},   // ALTA
};

// Modo cinema: render offline, sem compromisso com o FPS
static const QParams kQCine = {4096, 75.0f, 4.0f, 30.0f, 90.0f, 1, 1100.0f, 240.0f, 520.0f, 480.0f, 110.0f, 1.00f, 130.0f};

const char* Renderer::qualityName(int q) {
    static const char* n[3] = {"BAIXA", "MEDIA", "ALTA"};
    return n[q < 0 ? 0 : (q > 2 ? 2 : q)];
}

// ---------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------
// ---- timer de GPU (EXT_disjoint_timer_query), carregado em runtime ----
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
typedef void (*FnGenQ)(GLsizei, GLuint*);
typedef void (*FnDelQ)(GLsizei, const GLuint*);
typedef void (*FnBeginQ)(GLenum, GLuint);
typedef void (*FnEndQ)(GLenum);
typedef void (*FnGetQ64)(GLuint, GLenum, unsigned long long*);
typedef void (*FnGetQi)(GLuint, GLenum, GLint*);
static FnGenQ qGen; static FnDelQ qDel; static FnBeginQ qBegin; static FnEndQ qEnd; static FnGetQ64 qGet64; static FnGetQi qGeti;

static const char* kHdr = "#version 300 es\nprecision highp float;\nprecision highp sampler2DShadow;\nprecision highp sampler2DArray;\n";

// Biblioteca comum (chão, personagem, céu): céu analítico, sombra com PCF.
static const char* kLib = R"(
uniform vec3 uCam, uAmbient, uLightDir, uLightCol;
uniform vec3 uHorizon, uZenith, uSunDir;
uniform float uNight, uFogK;
uniform float uShadowOn, uShadowTexel, uShadowRadius, uPcf;
uniform vec3 uShadowCenter;
uniform vec2 uShadowInv;
uniform mat4 uShadowMat;
uniform sampler2DShadow uShadow;
uniform vec4 uTorch[4];   // tochas acesas: xyz = chama (mundo), w = intensidade (0 = apagada; já inclui o tremular)

// Luz pontual quente das tochas (até 4 mais próximas): queda suave até 15 m, com um pouco de "wrap" para não ficar chapada.
vec3 torchLight(vec3 wp, vec3 n) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float k = uTorch[i].w;
        if (k <= 0.0) continue;
        vec3 d = uTorch[i].xyz - wp;
        float d2 = dot(d, d);
        float att = clamp(1.0 - d2 / 225.0, 0.0, 1.0);
        att *= att;
        float ndl = clamp(dot(n, d * inversesqrt(d2 + 1e-4)) * 0.75 + 0.25, 0.0, 1.0);
        acc += vec3(1.0, 0.50, 0.17) * (k * att * ndl * 3.2 / (1.0 + d2 * 0.22));
    }
    return acc;
}

vec3 skyColor(vec3 r) {
    vec3 col = mix(uHorizon, uZenith, pow(clamp(r.y, 0.0, 1.0), 0.55));
    float sd = dot(r, uSunDir);
    float vis = clamp(uSunDir.y * 4.0 + 1.0, 0.0, 1.0);
    col += vec3(1.0, 0.72, 0.42) * pow(max(sd, 0.0), 28.0) * 0.40 * vis;
    return col;
}

float shadowFactor(vec3 wp, vec3 N, float ndl) {
    if (uShadowOn < 0.5) return 1.0;
    vec3 p = wp + N * (uShadowTexel * 2.0);                  // offset pela normal: evita acne sem "descolar" a sombra
    vec4 sc = uShadowMat * vec4(p, 1.0);
    float z = sc.z - (0.0006 + 0.0015 * (1.0 - ndl));
    float s;
    if (uPcf > 2.0) {
        vec2 o = uShadowInv * 0.8;
        s = 0.25 * (texture(uShadow, vec3(sc.xy + vec2(-o.x, -o.y), z)) + texture(uShadow, vec3(sc.xy + vec2(o.x, -o.y), z)) +
                    texture(uShadow, vec3(sc.xy + vec2(-o.x, o.y), z)) + texture(uShadow, vec3(sc.xy + vec2(o.x, o.y), z)));
    } else {
        s = texture(uShadow, vec3(sc.xy, z));
    }
    float d = length(wp.xz - uShadowCenter.xz);
    return mix(1.0, s, 1.0 - smoothstep(0.7 * uShadowRadius, 0.98 * uShadowRadius, d));   // some suave na borda do mapa
}
)";

static const char* kGroundVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
uniform mat4 uVP;
uniform vec2 uOff;   // o plano do mar acompanha a câmera (oceano aberto: sem borda)
out vec3 vWorld;
void main() { vWorld = aPos + vec3(uOff.x, 0.0, uOff.y); gl_Position = uVP * vec4(vWorld, 1.0); })";

// Água: plano único na superfície (y = 0.00 m) com ondulação suave, Fresnel, reflexo planar/céu e sol/lua.
// A transparência revela a areia e o terreno submerso suavemente, ficando escuro no fundo.
static const char* kGroundMain = R"(
in vec3 vWorld;
uniform float uGloss, uTime;
uniform vec2 uScreenInv;
out vec4 outColor;
void main() {
    vec3 p = vWorld;
    float sh = shadowFactor(p, vec3(0.0, 1.0, 0.0), 1.0);
    vec3 light = uAmbient + uLightCol * max(uLightDir.y, 0.0) * sh;
    float d = distance(p, uCam);
    float fog = clamp(1.0 - exp(-d * 0.012 * uFogK), 0.0, 1.0);

    // Ondulação suave da água
    float wa = p.x * 0.38 + uTime * 0.85, wb = p.z * 0.32 - uTime * 0.65, wc = (p.x + p.z) * 0.18 + uTime * 0.45;
    vec3 N = normalize(vec3(0.035 * cos(wa) + 0.022 * cos(wc), 1.0, 0.035 * cos(wb) + 0.022 * cos(wc)));
    vec3 V = normalize(uCam - p);

    // Cor base da água do mar (turquesa suave sob a luz)
    vec3 waterBody = vec3(0.045, 0.28, 0.36) * light;

    // Fresnel realista: olhando de cima é transparente; em ângulo rasante reflete o céu
    float fres = 0.08 + 0.92 * pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);

    // Reflexo analítico do céu
    vec3 R = reflect(-V, N);
    vec3 refl = skyColor(R);

    float rMix = clamp(fres * uGloss, 0.0, 1.0);
    vec3 col = mix(waterBody, refl, rMix);

    // Brilho especular do sol / lua
    col += uLightCol * sh * pow(max(dot(R, uLightDir), 0.0), 280.0) * 2.2;

    // Alpha suave: translúcida olhando de cima (revelando o fundo) e mais opaca em ângulo rasante
    float a = clamp(mix(0.38, 0.88, rMix), 0.0, 1.0);
    outColor = vec4(mix(col, uHorizon, fog), mix(a, 1.0, fog));
}
)";

static const char* kTerrainVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aWeights;
uniform mat4 uVP;
out vec3 vW, vN;
out vec4 vWeights;
void main() {
    vW = aPos;
    vN = aNormal;
    vWeights = aWeights;
    gl_Position = uVP * vec4(aPos, 1.0);
}
)";

// Terreno com texturas PBR (Sand, Dirt, Grass, Rock): albedo+roughness e normal+AO em texture arrays,
// normal map por TBN e especular GGX. OTIMIZADO p/ mobile (Adreno 710 é limitado por banda de textura):
//  - só lê as camadas com peso relevante (quase todo pixel tem 1 bioma, nas bordas 2) -> ~2 fetches em vez de 8;
//  - textureGrad com derivadas calculadas antes dos ifs (mip correto mesmo com fluxo não uniforme);
//  - normal map / especular só até ~40 m (some suave); longe usa só o albedo + normal geométrica.
static const char* kTerrainMain = R"(
in vec3 vW, vN;
in vec4 vWeights;
uniform float uClipY;
uniform sampler2DArray uAlbedoArray;
uniform sampler2DArray uNormalArray;
out vec4 outColor;
void main() {
    if (vW.y < uClipY) { outColor = vec4(skyColor(normalize(vW - uCam)), 1.0); return; }   // sem discard: mantém o Early-Z (pinta o que o céu pintaria atrás da água)

    vec4 w = vWeights;
    w *= step(0.06, w);                                  // descarta pesos pequenos (evita fetch inútil)
    float wSum = w.r + w.g + w.b + w.a;
    w = wSum > 1e-4 ? w / wSum : vec4(0.0, 0.0, 1.0, 0.0);

    vec3 geomN = normalize(vN);
    float d = distance(vW, uCam);
    float detail = 1.0 - smoothstep(25.0, 40.0, d);      // 1 = perto (normal map + especular), 0 = longe

    // Repetição: 1 ladrilho a cada 4 m. Rocha em encosta íngreme usa projeção vertical (sem estiramento).
    vec2 uv = vW.xz * 0.25;
    vec2 rockUv = abs(geomN.y) < 0.65 ? (abs(geomN.x) > abs(geomN.z) ? vW.zy * 0.25 : vW.xy * 0.25) : uv;
    vec2 dx = dFdx(uv), dy = dFdy(uv), rdx = dFdx(rockUv), rdy = dFdy(rockUv);

    vec4 albedoRgh = vec4(0.0);
    vec4 normalAo = vec4(0.0);
    bool nm = detail > 0.0;
    if (w.r > 0.0) { albedoRgh += w.r * textureGrad(uAlbedoArray, vec3(uv, 0.0), dx, dy);
                     if (nm) normalAo += w.r * textureGrad(uNormalArray, vec3(uv, 0.0), dx, dy); }
    if (w.g > 0.0) { albedoRgh += w.g * textureGrad(uAlbedoArray, vec3(uv, 1.0), dx, dy);
                     if (nm) normalAo += w.g * textureGrad(uNormalArray, vec3(uv, 1.0), dx, dy); }
    if (w.b > 0.0) { albedoRgh += w.b * textureGrad(uAlbedoArray, vec3(uv, 2.0), dx, dy);
                     if (nm) normalAo += w.b * textureGrad(uNormalArray, vec3(uv, 2.0), dx, dy); }
    if (w.a > 0.0) { albedoRgh += w.a * textureGrad(uAlbedoArray, vec3(rockUv, 3.0), rdx, rdy);
                     if (nm) normalAo += w.a * textureGrad(uNormalArray, vec3(rockUv, 3.0), rdx, rdy); }
    if (!nm) normalAo = vec4(0.5, 0.5, 1.0, 1.0);

    vec3 baseCol = albedoRgh.rgb;
    float roughness = clamp(albedoRgh.a, 0.20, 0.95);
    float ao = mix(1.0, clamp(normalAo.a, 0.15, 1.0), detail);

    // Areia molhada perto da linha d'água (y = 0.0 a 0.28 m): mais escura e mais polida
    if (vW.y >= 0.0 && vW.y < 0.28 && w.r > 0.25) {
        float shoreWet = clamp(1.0 - vW.y / 0.28, 0.0, 1.0) * w.r;
        roughness = mix(roughness, 0.18, shoreWet * 0.8);
        baseCol = mix(baseCol, baseCol * 0.72, shoreWet * 0.5);
    }

    // Normal map (NormalGL) via TBN do relevo, atenuado com a distância
    vec3 N = geomN;
    if (nm) {
        vec3 tNorm = normalAo.rgb * 2.0 - 1.0;
        vec3 up = abs(geomN.y) > 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
        vec3 T = normalize(cross(geomN, up));
        vec3 B = cross(geomN, T);
        N = normalize(mix(geomN, mat3(T, B, geomN) * tNorm, 0.85 * detail));
    }

    float ndl = max(dot(N, uLightDir), 0.0);
    float sh = shadowFactor(vW, N, ndl);
    vec3 col = baseCol * (uAmbient * ao + uLightCol * ndl * sh + torchLight(vW, N));

    // Especular GGX (Cook-Torrance) só de perto
    if (nm && ndl > 0.001) {
        vec3 V = (uCam - vW) / max(d, 1e-3);
        vec3 H = normalize(uLightDir + V);
        float ndh = max(dot(N, H), 0.0);
        float r2 = roughness * roughness;
        float r4 = r2 * r2;
        float denom = ndh * ndh * (r4 - 1.0) + 1.0;
        float D = r4 / max(3.14159265 * denom * denom, 1e-4);
        float vdh = 1.0 - max(dot(V, H), 0.0);
        float v2 = vdh * vdh;
        float F = 0.04 + 0.96 * v2 * v2 * vdh;
        float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
        float G = ndl / (ndl * (1.0 - k) + k);
        col += uLightCol * (D * F * G * 0.25 * sh * detail);
    }

    // Transição suave contínua para o fundo do mar
    if (vW.y < 0.0) {
        float depth = clamp(-vW.y / 2.0, 0.0, 1.0);
        vec3 seaDeep = vec3(0.018, 0.11, 0.18) * (uAmbient + uLightCol * max(uLightDir.y, 0.0) * sh);
        col = mix(col * 0.85, seaDeep, smoothstep(0.0, 0.85, depth));
    }

    // Névoa atmosférica
    outColor = vec4(mix(col, uHorizon, clamp(1.0 - exp(-d * 0.012 * uFogK), 0.0, 1.0)), 1.0);
}
)";

// Objetos do mundo (árvores, rochas, construções, coletáveis) com cores de vértice
static const char* kPropVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aColor;
uniform mat4 uVP;
out vec3 vW, vN, vC;
out float vA;
void main() { vW = aPos; vN = aNormal; vC = aColor.rgb; vA = aColor.a; gl_Position = uVP * vec4(aPos, 1.0); }
)";

static const char* kPropMain = R"(
in vec3 vW, vN, vC;
in float vA;
uniform float uClipY;
uniform float uTime;
out vec4 outColor;
void main() {
#ifdef CLIP_DISCARD
    if (vW.y < uClipY) discard;   // só a grama (alpha-test real: tufos na beira d'água mostram o fundo atrás)
#else
    if (vW.y < uClipY) { outColor = vec4(skyColor(normalize(vW - uCam)), 1.0); return; }   // sem discard: mantém o Early-Z (pinta o que o céu pintaria atrás da água)
#endif
    vec3 N = normalize(vN);
    float ndl = max(dot(N, uLightDir), 0.0);
    float sh = shadowFactor(vW, N, ndl);
    vec3 col = vC * (uAmbient + uLightCol * ndl * sh + torchLight(vW, N));
    // materiais brilhantes (alfa do vértice < 1): 0.988 = ouro (brilho próprio + reflexo), 0.94 = diamante (mais brilho, borda azul e cintilar)
    if (vA < 0.995) {
        float dia = vA < 0.96 ? 1.0 : 0.0;
        vec3 V = normalize(uCam - vW);
        vec3 H = normalize(uLightDir + V);
        float spec = pow(max(dot(N, H), 0.0), mix(28.0, 60.0, dia)) * sh;
        float rim = pow(1.0 - max(dot(N, V), 0.0), 2.5);
        col += vC * mix(0.25, 0.55, dia);
        col += uLightCol * spec * mix(0.9, 1.5, dia);
        if (dia > 0.5) {
            float tw = pow(0.5 + 0.5 * sin(uTime * 2.6 + dot(vW, vec3(5.1, 7.3, 3.7))), 6.0);
            col += vec3(0.45, 0.85, 1.0) * (rim * 0.4 + tw * 0.6);
        } else col += vec3(1.0, 0.8, 0.3) * rim * 0.18;
    }

    if (vW.y < 0.0) {
        float depth = clamp(-vW.y / 2.0, 0.0, 1.0);
        vec3 seaDeep = vec3(0.018, 0.11, 0.18) * (uAmbient + uLightCol * max(uLightDir.y, 0.0) * sh);
        col = mix(col * 0.85, seaDeep, smoothstep(0.0, 0.85, depth));
    }
    float d = distance(vW, uCam);
    outColor = vec4(mix(col, uHorizon, clamp(1.0 - exp(-d * 0.012 * uFogK), 0.0, 1.0)), 1.0);
}
)";

// Navio: mesmas cores de vértice dos props, com matriz de modelo (o casco anda e gira)
static const char* kShipVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aColor;
uniform mat4 uVP;
uniform mat4 uModel;
out vec3 vW, vN, vC;
out float vA;
void main() { vec4 w = uModel * vec4(aPos, 1.0); vW = w.xyz; vN = mat3(uModel) * aNormal; vC = aColor.rgb; vA = aColor.a; gl_Position = uVP * w; }
)";

static const char* kCharVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in uvec4 aBones;
layout(location=3) in vec4 aWeights;
layout(location=4) in vec2 aUV;
uniform mat4 uMVP, uModel;
uniform int uSkinned;
uniform vec4 uBones[195];
out vec3 vN, vW, vL;
out vec2 vUV;
void main() {
    vL = aPos;   // posição de bind (brilho das skins pulsa ao longo do corpo)
    vUV = aUV;
    vec3 sp = aPos;
    vec3 sn = aNormal;
    if (uSkinned != 0) {
        uvec4 b = aBones * 3u;
        vec4 r0 = uBones[b.x]      * aWeights.x + uBones[b.y]      * aWeights.y + uBones[b.z]      * aWeights.z + uBones[b.w]      * aWeights.w;
        vec4 r1 = uBones[b.x + 1u] * aWeights.x + uBones[b.y + 1u] * aWeights.y + uBones[b.z + 1u] * aWeights.z + uBones[b.w + 1u] * aWeights.w;
        vec4 r2 = uBones[b.x + 2u] * aWeights.x + uBones[b.y + 2u] * aWeights.y + uBones[b.z + 2u] * aWeights.z + uBones[b.w + 2u] * aWeights.w;
        vec4 p4 = vec4(aPos, 1.0);
        sp = vec3(dot(r0, p4), dot(r1, p4), dot(r2, p4));
        sn = vec3(dot(r0.xyz, aNormal), dot(r1.xyz, aNormal), dot(r2.xyz, aNormal));
    }
    vec4 w = uModel * vec4(sp, 1.0);
    vW = w.xyz;
    vN = mat3(uModel) * sn;
    gl_Position = uMVP * vec4(sp, 1.0);
})";

// Personagem e construções (.e0m):
// uAlpha == 1.0: personagem (difuso + Fresnel + especular); uAlpha == 1.2: ferramenta (fosca)
// uAlpha > 1.5: construção colocada (madeira fosca com veios sutis + névoa)
// uAlpha < 0.99: preview de construção (verde/vermelho translúcido brilhante)
static const char* kCharMain = R"(
in vec3 vN, vW, vL;
in vec2 vUV;
uniform sampler2D uCharTex;   // textura do personagem (uTexOn = 1); sem textura a cor é só uColor
uniform float uTexOn;
uniform vec3 uColor;
uniform float uClipY;
uniform float uAlpha;
uniform vec4 uSkin;    // SKIN (só na malha do personagem): x = metal, y = brilho próprio, z = quanto a textura vira tons de uSkin2 (0 = textura original)
uniform vec3 uSkin2;   // cor do material (prata, ouro, fantasma) e do brilho
uniform float uSkinT;  // tempo (s) para o brilho pulsar
out vec4 outColor;
void main() {
    if (vW.y < uClipY) { outColor = vec4(skyColor(normalize(vW - uCam)), 1.0); return; }   // sem discard: mantém o Early-Z (pinta o que o céu pintaria atrás da água)
    vec3 N = normalize(vN);
    vec3 V = normalize(uCam - vW);
    if (dot(N, V) < 0.0) N = -N;   // peças finas vistas por trás (franjas da camisa, aba): sem isso o Fresnel refletia o céu azul nelas
    float ndl = max(dot(N, uLightDir), 0.0);
    if (uAlpha < 0.99) {
        // Preview verde/vermelho translúcido com realce de borda
        float rim = pow(1.0 - max(dot(N, V), 0.0), 2.0) * 0.35;
        vec3 col = uColor * (0.68 + 0.32 * ndl) + vec3(rim);
        outColor = vec4(col, uAlpha);
        return;
    }
    float sh = shadowFactor(vW, N, ndl);
    if (uAlpha > 1.5) {
        // Construção de tábuas de madeira colocada no mundo (veios da madeira + variação entre tábuas)
        float grain = sin(vW.y * 22.0 + vW.x * 7.5 + vW.z * 7.5) * 0.032 + sin(vW.x * 38.0 + vW.z * 38.0) * 0.015;
        vec3 baseCol = clamp(uColor + vec3(grain, grain * 0.75, grain * 0.45), 0.0, 1.0);
        vec3 col = baseCol * (uAmbient + uLightCol * (0.18 + 0.82 * ndl) * sh + torchLight(vW, N));
        if (vW.y < 0.0) {
            float sub = clamp(-vW.y / 1.5, 0.0, 1.0);
            vec3 underCol = vec3(0.02, 0.12, 0.20) * (uAmbient + uLightCol * ndl * sh);
            col = mix(col, underCol, sub * 0.72);
        }
        float d = distance(vW, uCam);
        outColor = vec4(mix(col, uHorizon, clamp(1.0 - exp(-d * 0.012 * uFogK), 0.0, 1.0)), 1.0);
        return;
    }
    vec3 base = uColor;
    if (uTexOn > 0.5) base *= texture(uCharTex, vUV).rgb;
    if (uSkin.z > 0.0) {   // material: mantém o claro/escuro da textura (desenho da roupa e do rosto) em tons de uSkin2
        float lum = dot(base, vec3(0.30, 0.59, 0.11));
        base = mix(base, uSkin2 * (0.35 + 0.95 * lum), uSkin.z);
    }
    vec3 diff = base * (uAmbient + uLightCol * ndl * sh + torchLight(vW, N)) * (1.0 - 0.55 * uSkin.x);
    vec3 col = diff;   // sem reflexo do céu nem brilho especular nos personagens (acabamento fosco)
    if (uSkin.y > 0.0) {   // brilho próprio: no padrão (se houver) ou na borda do corpo, pulsando devagar
        float pulse = 0.75 + 0.25 * sin(uSkinT * 2.4 + vL.y * 3.0);
        float rim = pow(1.0 - max(dot(N, V), 0.0), 2.0);
        float k = 0.30 + 0.9 * rim;
        col += uSkin2 * uSkin.y * k * pulse;
    }
    if (vW.y < 0.0) {
        float sub = clamp(-vW.y / 1.3, 0.0, 1.0);
        vec3 underCol = vec3(0.02, 0.12, 0.20) * (uAmbient + uLightCol * ndl * sh);
        col = mix(col, underCol, sub * 0.72);
    }
    outColor = vec4(col, 1.0);
})";

// Grama: tufos instanciados (malha de 7 lâminas). Altura/giro/cor vêm do instante; balanço de vento por seno; some devagar com a
// distância (encolhe) em vez de "pipocar". O fragment é o mesmo do terreno (luz + sombra + névoa).
static const char* kGrassVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aInst;
layout(location=2) in vec4 aParam;
uniform mat4 uVP;
uniform float uTime, uGrassDist;
uniform vec3 uCam;
out vec3 vW, vN, vC;
out float vA;
void main() {
    vA = 1.0;
    float dist = distance(aInst.xz, uCam.xz);
    float fade = 1.0 - smoothstep(uGrassDist * 0.6, uGrassDist, dist);
    float sc = (0.55 + 0.9 * aParam.x) * fade;
    float ang = aParam.y * 6.2831853, c = cos(ang), s = sin(ang);
    vec3 loc = vec3((aPos.x * c + aPos.z * s) * sc * 1.4, aPos.y * 0.55 * sc, (-aPos.x * s + aPos.z * c) * sc * 1.4);
    float sway = sin(uTime * 1.8 + aInst.x * 0.45 + aInst.z * 0.6) * 0.10 * aPos.y * aPos.y * sc
               + sin(uTime * 3.1 + aInst.z * 0.9 + aInst.x * 0.3) * 0.03 * aPos.y * sc;
    loc.x += sway; loc.z += sway * 0.6;
    vec3 w = aInst + loc;
    vW = w; vN = vec3(0.0, 1.0, 0.0);
    vC = mix(vec3(0.22, 0.45, 0.15), vec3(0.45, 0.62, 0.20), aParam.z) * (0.5 + 0.7 * aPos.y);   // base escura, ponta clara
    gl_Position = uVP * vec4(w, 1.0);
})";

// Linha do indicador de coleta: cor branca com alpha por vértice (mais forte na base, some suavemente no topo)
static const char* kMarkerVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in float aA;
uniform mat4 uVP;
out float vA;
void main() { vA = aA; gl_Position = uVP * vec4(aPos, 1.0); })";

static const char* kMarkerFS = R"(#version 300 es
precision mediump float;
in float vA;
out vec4 outColor;
void main() { outColor = vec4(1.0, 1.0, 1.0, vA); })";

// Gizmos 3D do editor de construção (setas X/Y/Z e anel de rotação com cor RGBA por vértice)
static const char* kGizmoVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
uniform mat4 uVP;
out vec4 vCol;
void main() { vCol = aCol; gl_Position = uVP * vec4(aPos, 1.0); })";

static const char* kGizmoFS = R"(#version 300 es
precision mediump float;
in vec4 vCol;
out vec4 outColor;
void main() { outColor = vCol; })";

// Partículas de respingo de água ao nadar (gotículas e espuma branca translúcida)
static const char* kSplashVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in float aAlpha;
uniform mat4 uVP;
out vec2 vUV;
out float vAlpha;
void main() {
    vUV = aUV;
    vAlpha = aAlpha;
    gl_Position = uVP * vec4(aPos, 1.0);
})";

static const char* kSplashFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
in float vAlpha;
out vec4 outColor;
void main() {
    float r = length(vUV - vec2(0.5));
    if (r > 0.5) discard;
    float edge = smoothstep(0.5, 0.12, r);
    float core = smoothstep(0.32, 0.0, r);
    vec3 col = mix(vec3(0.85, 0.95, 1.0), vec3(1.0, 1.0, 1.0), core);
    outColor = vec4(col, edge * vAlpha);
})";

// Efeitos do BACAMARTE: partículas coloridas (clarão, fumaça, poeira, sangue) e rastros dos chumbos. Cor PRÉ-MULTIPLICADA na saída:
// o mesmo shader serve para o passe aditivo (ONE, ONE) e o de transparência (ONE, ONE_MINUS_SRC_ALPHA). uv.x >= 1.5 = rastro (faixa).
static const char* kFxVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
uniform mat4 uVP;
out vec2 vUV;
out vec4 vCol;
void main() { vUV = aUV; vCol = aCol; gl_Position = uVP * vec4(aPos, 1.0); })";
static const char* kFxFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
in vec4 vCol;
out vec4 outColor;
void main() {
    float a;
    if (vUV.x >= 1.5) {
        float u = vUV.x - 2.0, across = 1.0 - abs(vUV.y * 2.0 - 1.0);
        a = across * across * (0.15 + 0.85 * u);
    } else {
        float r = length(vUV - vec2(0.5)) * 2.0;
        if (r > 1.0) discard;
        a = pow(1.0 - r, 1.6);
    }
    a *= vCol.a;
    outColor = vec4(vCol.rgb * a, a);
})";

// Chama da tocha: billboard aditivo com fogo procedural (núcleo amarelo-branco, borda laranja, língua que oscila)
static const char* kFlameFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
in float vAlpha;
uniform float uTime;
out vec4 outColor;
void main() {
    vec2 p = vec2(vUV.x - 0.5, vUV.y);                       // x centrado; y 0 = base, 1 = ponta
    float sway = sin(uTime * 7.0 + vUV.y * 5.0) * 0.07 * vUV.y + sin(uTime * 13.0 + vUV.y * 9.0) * 0.03 * vUV.y;
    float w = (1.0 - vUV.y) * 0.34 * (0.35 + 0.65 * smoothstep(0.0, 0.25, vUV.y)) + 0.015;
    float dx = abs(p.x - sway) / w;
    float body = clamp(1.0 - dx * dx, 0.0, 1.0) * smoothstep(1.0, 0.55, vUV.y) * smoothstep(0.0, 0.08, vUV.y);
    float core = clamp(1.0 - dx * 1.6, 0.0, 1.0) * smoothstep(0.62, 0.0, vUV.y);
    vec3 col = mix(vec3(1.0, 0.35, 0.05), vec3(1.0, 0.85, 0.35), core);
    col = mix(col, vec3(1.0, 0.97, 0.8), core * core * 0.7);
    outColor = vec4(col * (body * 1.5), body * vAlpha);
})";

// Brilho suave em volta da chama (halo aditivo)
static const char* kGlowFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
in float vAlpha;
out vec4 outColor;
void main() {
    float r = length(vUV - vec2(0.5)) * 2.0;
    float g = clamp(1.0 - r, 0.0, 1.0);
    g = g * g * g;
    outColor = vec4(vec3(1.0, 0.52, 0.18) * g, g * vAlpha);
})";

static const char* kShadowVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=2) in uvec4 aBones;
layout(location=3) in vec4 aWeights;
uniform mat4 uMVP;
uniform int uSkinned;
uniform vec4 uBones[195];
void main() {
    vec3 p = aPos;
    if (uSkinned != 0) {
        uvec4 b = aBones * 3u;
        vec4 r0 = uBones[b.x]      * aWeights.x + uBones[b.y]      * aWeights.y + uBones[b.z]      * aWeights.z + uBones[b.w]      * aWeights.w;
        vec4 r1 = uBones[b.x + 1u] * aWeights.x + uBones[b.y + 1u] * aWeights.y + uBones[b.z + 1u] * aWeights.z + uBones[b.w + 1u] * aWeights.w;
        vec4 r2 = uBones[b.x + 2u] * aWeights.x + uBones[b.y + 2u] * aWeights.y + uBones[b.z + 2u] * aWeights.z + uBones[b.w + 2u] * aWeights.w;
        vec4 p4 = vec4(aPos, 1.0);
        p = vec3(dot(r0, p4), dot(r1, p4), dot(r2, p4));
    }
    gl_Position = uMVP * vec4(p, 1.0);
})";

static const char* kShadowFS = R"(#version 300 es
precision mediump float;
out vec4 outColor;
void main() { outColor = vec4(1.0); })";

static const char* kSkyVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
out vec2 vNdc;
void main() { vNdc = aPos; gl_Position = vec4(aPos, 1.0, 1.0); })";

static const char* kSkyMain = R"(
in vec2 vNdc;
uniform vec3 uFwd, uRight, uUp;
uniform vec2 uTan;
out vec4 outColor;
float hash(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
void main() {
    vec3 r = normalize(uFwd + uRight * (vNdc.x * uTan.x) + uUp * (vNdc.y * uTan.y));
    vec3 col = skyColor(r);
    float sd = dot(r, uSunDir);
    col = mix(col, vec3(1.6, 1.5, 1.2), smoothstep(0.99935, 0.99946, sd));                                   // disco do sol
    float md = dot(r, -uSunDir);
    col = mix(col, vec3(0.85, 0.90, 1.0), smoothstep(0.99935, 0.99946, md) * clamp(uNight * 1.5, 0.0, 1.0)); // lua
    vec3 q = r * 110.0;
    float st = step(0.992, hash(floor(q))) * smoothstep(0.32, 0.0, length(fract(q) - 0.5));
    col += vec3(st) * uNight * smoothstep(0.0, 0.12, r.y);                                                    // estrelas
    outColor = vec4(col, 1.0);
})";

// Filtro de cor PIRATA (multiplicado sobre a cena 3D, antes da HUD): tom quente de pergaminho/sol de rum no centro,
// bordas queimadas (vinheta marrom-avermelhada). uK = intensidade 0..1.
static const char* kGradeVS = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
static const char* kGradeFS = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform float uK;
uniform float uAspect;
out vec4 o;
void main() {
    vec2 q = (vUv - 0.5) * vec2(uAspect, 1.0);
    float vig = smoothstep(0.32, 1.05, length(q));
    vec3 centre = vec3(1.00, 0.88, 0.64);       // calor dourado
    vec3 edge = vec3(0.40, 0.24, 0.15);         // borda queimada
    vec3 t = mix(centre, edge, vig * vig * 0.9 + vig * 0.1);
    o = vec4(mix(vec3(1.0), t, uK), 1.0);
})";


// MASTRO DE DOMINAÇÃO: pano da bandeira (grade s x v) tremulando no vento; cor = textura gerada pelo shader da bandeira (bakeIslandFlags)
static const char* kClothVS = R"(#version 300 es
layout(location=0) in vec2 aUV;
uniform mat4 uVP;
uniform vec3 uBase;    // canto de baixo junto ao mastro
uniform vec3 uDir;     // direção do vento (horizontal, unitário)
uniform vec2 uSize;    // comprimento, altura (m)
uniform float uTime;   // tempo + semente
out vec3 vW, vN;
out vec2 vUV;
void main() {
    float s = aUV.x, v = aUV.y;
    vec3 side = vec3(-uDir.z, 0.0, uDir.x);
    float env = smoothstep(0.0, 0.22, s) * (0.45 + 0.55 * s);
    float p1 = s * 5.2 - uTime * 3.2 + v * 0.7, p2 = s * 11.0 - uTime * 4.9 + 1.3;
    float off = env * (0.26 * sin(p1) + 0.08 * sin(p2));
    float dOff = env * (0.26 * 5.2 * cos(p1) + 0.08 * 11.0 * cos(p2)) / uSize.x;
    vec3 p = uBase + uDir * (s * uSize.x * (1.0 - 0.04 * env)) + vec3(0.0, v * uSize.y - s * s * 0.10 * uSize.y, 0.0) + side * off;
    vN = normalize(side - uDir * dOff);
    vW = p; vUV = vec2(0.0083 + s * 0.9834, 0.0375 + v * 0.925);
    gl_Position = uVP * vec4(p, 1.0);
}
)";
static const char* kClothMain = R"(
in vec3 vW, vN;
in vec2 vUV;
uniform sampler2D uClothTex;
out vec4 outColor;
void main() {
    vec4 t = texture(uClothTex, vUV);
    if (t.a < 0.5) discard;
    vec3 N = normalize(vN);
    if (!gl_FrontFacing) N = -N;
    float ndl = max(dot(N, uLightDir), 0.0), back = max(-dot(N, uLightDir), 0.0);
    float sh = shadowFactor(vW, N, ndl);
    vec3 col = t.rgb * (uAmbient + uLightCol * (ndl + back * 0.35) * sh + torchLight(vW, N));
    float d = distance(vW, uCam);
    outColor = vec4(mix(col, uHorizon, clamp(1.0 - exp(-d * 0.012 * uFogK), 0.0, 1.0)), 1.0);
}
)";

static const char* kUiVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
uniform vec4 uRect;     // cx, cy, halfW, halfH (pixels)
uniform vec2 uScreen;
out vec2 vP;
out vec2 vPx;
void main() {
    vP = aPos;
    vPx = aPos * uRect.zw;
    vec2 px = uRect.xy + vPx;
    gl_Position = vec4(px.x / uScreen.x * 2.0 - 1.0, 1.0 - px.y / uScreen.y * 2.0, 0.0, 1.0);
})";

static const char* kUiFS = R"(#version 300 es
precision mediump float;
in highp vec2 vP;
in vec2 vPx;
uniform vec4 uColor;
uniform highp vec4 uSheet;   // modo 7 com uThickness = 3 (folha do mapa grande): cx, cy, raio (px), -
uniform highp vec4 uGrid;    // grade da folha: ponto do mundo no centro da tela (x, z), metros por pixel, passo (m)
uniform highp vec4 uRect;
uniform int uMode;      // 0 disco, 1 anel, 2 ícone de correr (>>), 3 rounded box fill, 4 rounded box border
uniform float uRadius;
uniform float uThickness;
uniform sampler2D uIcon;   // atlas de ícones (modo 5)
uniform vec4 uUv;          // u0, v0, u1, v1 do ícone no atlas
out vec4 outColor;
float seg(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}
highp float hash12(highp vec2 p) { highp vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
highp float vnoise(highp vec2 p) {
    highp vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), f.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}
void main() {
    float a = 0.0;
    if (uMode == 10) {   // degradê linear (uColor -> uSheet na direção uUv.xy) + textura orgânica sutil (uUv.z = força, uUv.w = semente)
        float d = sdRoundBox(vPx, uRect.zw, uRadius);
        float aa = fwidth(d) + 1e-4;
        float m = 1.0 - smoothstep(0.0, aa * 1.5, d);
        float t = clamp(dot(vP, uUv.xy) * 0.5 + 0.5, 0.0, 1.0);
        vec4 c = mix(uColor, uSheet, t);
        if (uUv.z > 0.0) {
            highp vec2 p = vPx + vec2(uUv.w * 173.0, uUv.w * 91.0);
            float g1 = vnoise(vec2(p.x * 0.008, p.y * 0.045));   // veio horizontal sutil de madeira/lona
            float g2 = vnoise(p * 0.032);
            float g = (g1 - 0.5) * 0.055 + (g2 - 0.5) * 0.035;
            c.rgb = max(c.rgb + g * min(uUv.z, 1.0), 0.0);
        }
        outColor = vec4(c.rgb, c.a * m);
        return;
    }
    if (uMode == 13) {   // PAINEL RÚSTICO ("Carta Nautica & Carvalho"): uThickness = estilo (0..7), uColor.a = opacidade, uUv.x = seed
        float d = sdRoundBox(vPx, uRect.zw, uRadius);
        float aa = fwidth(d) + 1e-4;
        float fill = 1.0 - smoothstep(-aa * 0.5, aa * 1.3, d);
        if (fill <= 0.002) discard;
        int st = int(uThickness + 0.5);
        float ty = clamp(vP.y * 0.5 + 0.5, 0.0, 1.0);
        highp vec2 p = vPx + vec2(uUv.x * 137.0, uUv.x * 61.0);
        float grain = (vnoise(vec2(p.x * 0.006, p.y * 0.055)) - 0.5) * 0.045 + (vnoise(vec2(p.x * 0.022, p.y * 0.14)) - 0.5) * 0.022;
        float fine  = (vnoise(p * 0.045) - 0.5) * 0.030;
        vec3 col = vec3(0.10, 0.08, 0.06);
        float aOut = uColor.a;
        if (st == 6 || st == 7) {   // fundo de tela cheia: carvalho noturno (6) ou azul-abissal quente (7)
            vec3 topC = st == 6 ? vec3(0.115, 0.088, 0.065) : vec3(0.065, 0.108, 0.140);
            vec3 botC = st == 6 ? vec3(0.055, 0.042, 0.032) : vec3(0.028, 0.046, 0.064);
            col = mix(topC, botC, ty) + grain * 0.55 + fine * 0.45;
            float vig = smoothstep(1.35, 0.25, length(vP * vec2(0.85, 1.0)));
            col *= mix(0.68, 1.05, vig);
            outColor = vec4(max(col, 0.0), aOut * fill);
            return;
        }
        if (st == 5) {   // PERGAMINHO envelhecido com fibras e contorno tinta sépia
            col = mix(vec3(0.90, 0.83, 0.68), vec3(0.79, 0.69, 0.52), ty) + fine * 1.1 + grain * 0.35;
            float edgeDark = smoothstep(-28.0, 0.0, d);
            col = mix(col, vec3(0.66, 0.54, 0.36), edgeDark * 0.45);
            float bOut = smoothstep(-2.4 - aa, -2.4 + aa, d);
            float bIn  = (1.0 - smoothstep(0.0, aa * 1.2, abs(d + 6.5) - 0.7)) * 0.45;
            col = mix(col, vec3(0.22, 0.13, 0.06), max(bOut * 0.92, bIn));
            outColor = vec4(max(col, 0.0), aOut * fill);
            return;
        }
        // painéis de madeira / couro / slots (st = 0..4)
        vec3 cTop, cBot, bCol;
        float bW = 2.0, innerGlow = 0.08, inRim = 0.0;
        if (st == 0) {        // PanelWood: carvalho escuro nobre + latão + rebites
            cTop = vec3(0.145, 0.112, 0.082); cBot = vec3(0.082, 0.062, 0.046);
            bCol = vec3(0.76, 0.58, 0.28); bW = 2.6; inRim = 0.32;
        } else if (st == 1) { // PanelCard: couro/madeira médio-escuro
            cTop = vec3(0.175, 0.135, 0.100); cBot = vec3(0.105, 0.080, 0.060);
            bCol = vec3(0.56, 0.43, 0.22); bW = 1.8; inRim = 0.14;
        } else if (st == 2) { // PanelCardSel: cartão selecionado em latão aceso
            cTop = vec3(0.235, 0.178, 0.112); cBot = vec3(0.125, 0.094, 0.062);
            bCol = vec3(0.95, 0.78, 0.36); bW = 3.2; inRim = 0.42;
        } else if (st == 3) { // PanelSlot: rebaixo escuro de inventário/craft
            cTop = vec3(0.080, 0.062, 0.048); cBot = vec3(0.118, 0.092, 0.070);
            bCol = vec3(0.52, 0.40, 0.22); bW = 1.6; innerGlow = -0.06;
        } else {              // PanelSlotHot: slot destacado / acesso rápido
            cTop = vec3(0.140, 0.108, 0.074); cBot = vec3(0.175, 0.134, 0.088);
            bCol = vec3(0.92, 0.74, 0.34); bW = 2.6; innerGlow = 0.06;
        }
        col = mix(cTop, cBot, ty) + grain * (st >= 3 ? 0.45 : 0.90) + fine * 0.40;
        // chanfro/sombra interna suave (relevo tátil)
        float topRim = (1.0 - smoothstep(0.0, 14.0, (vPx.y + uRect.w))) * innerGlow;
        float botRim = (1.0 - smoothstep(0.0, 16.0, (uRect.w - vPx.y))) * 0.06;
        col += vec3(0.95, 0.80, 0.50) * topRim - vec3(botRim);
        // fio fino interno (só em painéis grandes st==0 ou selecionados st==2)
        if (inRim > 0.01 && min(uRect.z, uRect.w) > 36.0) {
            float lineIn = 1.0 - smoothstep(0.0, aa * 1.4, abs(d + bW + 5.5) - 0.65);
            col = mix(col, bCol * 0.85, lineIn * inRim);
        }
        // rebites nos 4 cantos em PanelWood grande
        if (st == 0 && min(uRect.z, uRect.w) > 54.0) {
            float ins = max(uRadius * 0.58, 12.0);
            vec2 rv = abs(vPx) - (uRect.zw - vec2(ins));
            float rd = length(rv);
            float rBody = 1.0 - smoothstep(3.2 - aa, 3.2 + aa, rd);
            float rRing = 1.0 - smoothstep(4.6 - aa, 4.6 + aa, rd);
            vec3 rCol = mix(vec3(0.45, 0.32, 0.14), vec3(0.92, 0.76, 0.40), clamp(0.5 - rv.y * 0.12, 0.0, 1.0));
            col = mix(col, vec3(0.06, 0.04, 0.02), rRing * 0.75);
            col = mix(col, rCol, rBody * 0.92);
        }
        // borda principal metálica + contorno escuro fino por fora
        float bMask = smoothstep(-bW - aa, -bW + aa, d);
        float bDark = smoothstep(-1.0 - aa, -1.0 + aa, d);
        vec3 metal = bCol * mix(1.12, 0.76, ty);
        col = mix(col, metal, bMask * 0.92);
        col = mix(col, vec3(0.05, 0.03, 0.02), bDark * 0.45);
        outColor = vec4(max(col, 0.0), aOut * fill);
        return;
    }
    if (uMode == 14) {   // BOTÃO RÚSTICO TÁTIL: uThickness = variante (0 Latão, 1 Carvalho, 2 Carmesim, 3 Musgo, 4 Desabilitado, 5 Carmesim Forte)
        float d = sdRoundBox(vPx, uRect.zw, uRadius);
        float aa = fwidth(d) + 1e-4;
        float fill = 1.0 - smoothstep(-aa * 0.5, aa * 1.3, d);
        if (fill <= 0.002) discard;
        int st = int(uThickness + 0.5);
        float ty = clamp(vP.y * 0.5 + 0.5, 0.0, 1.0);
        float tex = (vnoise(vec2(vPx.x * 0.014, vPx.y * 0.085)) - 0.5) * 0.045;
        vec3 c0, c1, brd;
        float brdW = 2.0;
        if (st == 0) {        // BtnBrass: latão batido fosco (nunca neon!)
            c0 = vec3(0.88, 0.71, 0.35); c1 = vec3(0.62, 0.44, 0.16); brd = vec3(0.98, 0.87, 0.56); brdW = 2.2;
        } else if (st == 1) { // BtnWood: carvalho + latão
            c0 = vec3(0.20, 0.15, 0.105); c1 = vec3(0.115, 0.085, 0.060); brd = vec3(0.76, 0.58, 0.28); brdW = 2.0;
        } else if (st == 2) { // BtnCrimson: lacre carmesim escuro
            c0 = vec3(0.38, 0.11, 0.09); c1 = vec3(0.22, 0.055, 0.045); brd = vec3(0.76, 0.36, 0.28); brdW = 2.0;
        } else if (st == 3) { // BtnMoss: verde-musgo náutico
            c0 = vec3(0.19, 0.38, 0.25); c1 = vec3(0.11, 0.23, 0.15); brd = vec3(0.46, 0.74, 0.50); brdW = 2.0;
        } else if (st == 5) { // BtnCrimsonHi: confirmação de perigo
            c0 = vec3(0.56, 0.15, 0.12); c1 = vec3(0.34, 0.08, 0.06); brd = vec3(0.94, 0.74, 0.34); brdW = 2.4;
        } else {              // BtnDisabled: carvalho seco apagado
            c0 = vec3(0.13, 0.115, 0.10); c1 = vec3(0.085, 0.075, 0.068); brd = vec3(0.36, 0.32, 0.26); brdW = 1.6;
        }
        vec3 col = mix(c0, c1, ty) + tex;
        // brilho suave de bisel no topo e sombra na base
        float hi = (1.0 - smoothstep(0.0, 0.28, ty)) * (st == 0 ? 0.12 : 0.07);
        float sh = smoothstep(0.74, 1.0, ty) * 0.12;
        col += vec3(1.0, 0.94, 0.78) * hi - vec3(sh);
        float bMask = smoothstep(-brdW - aa, -brdW + aa, d);
        float bOut  = smoothstep(-0.9 - aa, -0.9 + aa, d);
        col = mix(col, brd * mix(1.08, 0.75, ty), bMask * 0.90);
        col = mix(col, vec3(0.05, 0.03, 0.02), bOut * 0.45);
        outColor = vec4(max(col, 0.0), uColor.a * fill);
        return;
    }
    if (uMode == 15) {   // MEDALHÃO FECHAR (X): lacre carmesim escuro + aro duplo latão + X SDF nítido
        float r = length(vP);
        float aa = fwidth(r) * 1.2 + 1e-4;
        float disc = 1.0 - smoothstep(0.96 - aa, 0.96 + aa, r);
        float sh   = (1.0 - smoothstep(0.85, 1.0, r)) * 0.38;
        if (disc <= 0.002 && sh <= 0.002) discard;
        float ty = clamp(vP.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 col = mix(vec3(0.54, 0.15, 0.12), vec3(0.30, 0.07, 0.055), ty);
        float rim1 = 1.0 - smoothstep(0.035 - aa, 0.035 + aa, abs(r - 0.92));
        float rim2 = 1.0 - smoothstep(0.016 - aa, 0.016 + aa, abs(r - 0.80));
        vec3 brass = mix(vec3(0.94, 0.78, 0.42), vec3(0.62, 0.45, 0.18), ty);
        col = mix(col, brass, max(rim1 * 0.95, rim2 * 0.45));
        // X elegante com braços levemente mais largos nas pontas (estilo gravura)
        float arm = 0.42;
        float d1 = seg(vP, vec2(-arm, -arm), vec2(arm, arm));
        float d2 = seg(vP, vec2(-arm, arm), vec2(arm, -arm));
        float dx = min(d1, d2);
        float wArm = 0.072 + 0.018 * clamp(r / 0.55, 0.0, 1.0);
        float xSh  = 1.0 - smoothstep(wArm - aa, wArm + aa, min(seg(vP - vec2(0.0, 0.05), vec2(-arm, -arm), vec2(arm, arm)), seg(vP - vec2(0.0, 0.05), vec2(-arm, arm), vec2(arm, -arm))));
        float xM   = 1.0 - smoothstep(wArm - aa, wArm + aa, dx);
        col = mix(col, vec3(0.10, 0.03, 0.02), xSh * 0.55);
        col = mix(col, vec3(0.96, 0.91, 0.80), xM);
        vec3 finalC = mix(vec3(0.0), col, disc);
        float finalA = max(sh, disc) * uColor.a;
        outColor = vec4(finalC, finalA);
        return;
    }
    if (uMode == 16) {   // DIVISÓRIA ORNAMENTAL (igual ao MS_ORNAMENT do Menu Principal): linha com fade + losango central
        float ax = abs(vPx.x), ay = abs(vPx.y);
        float aa = max(fwidth(ax), fwidth(ay)) + 0.5;
        float dh = min(uRect.w * 0.85, 7.5);
        float fade = clamp(1.0 - ax / max(uRect.z, 1.0), 0.0, 1.0);
        fade = smoothstep(0.0, 0.85, fade);
        // linha horizontal saindo do losango
        float lineM = (1.0 - smoothstep(0.6, 0.6 + aa, ay)) * smoothstep(dh + 3.0, dh + 6.0, ax) * fade;
        // losango vazado no centro
        float dm = ax + ay;
        float diaOuter = 1.0 - smoothstep(dh - aa, dh + aa, dm);
        float diaInner = 1.0 - smoothstep(dh * 0.48 - aa, dh * 0.48 + aa, dm);
        float dia = clamp(diaOuter - diaInner, 0.0, 1.0);
        // pontinhos laterais
        float dotM = 1.0 - smoothstep(2.0 - aa * 0.5, 2.0 + aa * 0.5, length(vec2(ax - (dh + 6.5), ay)));
        float m = max(max(lineM * 0.65, dia), dotM * 0.85);
        outColor = vec4(uColor.rgb, uColor.a * m);
        return;
    }
    if (uMode == 12) {   // ROSA DOS VENTOS de mapa de piratas (procedural): 8 pontas, anéis com marcas, norte vermelho; uColor.a = opacidade
        float r = length(vP);
        if (r > 1.0) discard;
        float aa = fwidth(r) * 1.3 + 1e-4;
        float ang = atan(vP.x, -vP.y);                                  // 0 = norte, cresce para a direita
        const vec3 ink = vec3(0.17, 0.10, 0.05), lt = vec3(0.82, 0.66, 0.40), red = vec3(0.62, 0.10, 0.07), redD = vec3(0.36, 0.05, 0.04);
        vec3 col = vec3(0.0); float al = 0.0;
        float ring = max(1.0 - smoothstep(0.0, aa, abs(r - 0.545) - 0.011), 1.0 - smoothstep(0.0, aa, abs(r - 0.628) - 0.006));
        float tk = abs(fract(ang * 5.09296 + 0.5) - 0.5);               // 32 marcas por volta
        float tick = (1.0 - smoothstep(0.05, 0.12, tk)) * smoothstep(0.552, 0.562, r) * (1.0 - smoothstep(0.612, 0.622, r));
        float outer = (1.0 - smoothstep(0.0, aa, abs(r - 0.70) - 0.004)) * 0.8;
        float m0 = max(max(ring, tick), outer);
        col = ink; al = m0;
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < 8; ++i) {
                bool card = (i % 2 == 0);
                if (card != (pass == 1)) continue;
                float len = card ? (i == 0 ? 0.97 : 0.90) : 0.68, hw = card ? 0.135 : 0.088;
                float da = ang - float(i) * 0.7853982; da = atan(sin(da), cos(da));
                vec2 pp = r * vec2(sin(da), cos(da));
                float k = clamp(1.0 - pp.y / len, 0.0, 1.0);
                float edge = hw * k - abs(pp.x);
                float cov = (pp.y > 0.0 && pp.y < len) ? smoothstep(-aa, aa, edge) : 0.0;
                vec3 c = pp.x > 0.0 ? (i == 0 ? red : lt) : (i == 0 ? redD : ink);
                // contorno fino escuro
                float rim = (pp.y > 0.0 && pp.y < len) ? (1.0 - smoothstep(0.0, aa * 1.4, abs(edge) )) : 0.0;
                c = mix(c, ink, rim * 0.85);
                col = mix(col, c, cov); al = max(al, cov);
            }
        }
        float cen = 1.0 - smoothstep(0.065, 0.065 + aa, r);
        col = mix(col, lt, cen); al = max(al, cen);
        float cenR = (1.0 - smoothstep(0.0, aa, abs(r - 0.065) - 0.008));
        col = mix(col, ink, cenR); al = max(al, cenR);
        outColor = vec4(col, al * uColor.a);
        return;
    }
    if (uMode == 11) {   // brilho radial macio
        float k = clamp(1.0 - length(vP), 0.0, 1.0);
        outColor = vec4(uColor.rgb, uColor.a * k * k);
        return;
    }
    if (uMode == 0) {
        float d = length(vP);
        float aa = fwidth(d) + 1e-4;
        a = 1.0 - smoothstep(1.0 - aa, 1.0, d);
    } else if (uMode == 1) {
        float d = length(vP);
        float aa = fwidth(d) + 1e-4;
        a = 1.0 - smoothstep(0.045 - aa, 0.045, abs(d - 0.955));
    } else if (uMode == 2) {
        float aa = fwidth(length(vP)) + 1e-4;
        float dd = 1e9;
        for (int i = 0; i < 2; ++i) {
            vec2 q = vP - vec2((float(i) - 0.5) * 0.6, 0.0);
            dd = min(dd, min(seg(q, vec2(-0.2, -0.5), vec2(0.2, 0.0)), seg(q, vec2(0.2, 0.0), vec2(-0.2, 0.5))));
        }
        a = 1.0 - smoothstep(0.11 - aa, 0.11, dd);
    } else if (uMode == 3) {
        float d = sdRoundBox(vPx, uRect.zw, uRadius);
        float aa = fwidth(d) + 1e-4;
        a = 1.0 - smoothstep(0.0, aa * 1.5, d);
    } else if (uMode == 4) {
        float d = sdRoundBox(vPx, uRect.zw, uRadius);
        float th = max(uThickness, 1.0);
        float bd = abs(d + th * 0.5) - th * 0.5;
        float aa = fwidth(bd) + 1e-4;
        a = 1.0 - smoothstep(0.0, aa * 1.5, bd);
    }
    if (uMode == 9) {   // progresso circular: anel de espessura uThickness (fração do raio), preenchido no sentido horário a partir do topo até uUv.x (0..1)
        float d = length(vP), aa = fwidth(d) + 1e-4, th = clamp(uThickness, 0.02, 0.9);
        float ring = (1.0 - smoothstep(1.0 - aa, 1.0, d)) * smoothstep(1.0 - th - aa, 1.0 - th, d);
        float f = atan(vP.x, -vP.y) / 6.2831853; if (f < 0.0) f += 1.0;
        float m = uUv.x >= 0.999 ? 1.0 : 1.0 - smoothstep(uUv.x - 0.004, uUv.x + 0.004, f);
        outColor = vec4(uColor.rgb, uColor.a * ring * m);
        return;
    }
    if (uMode == 6) {   // canto "côncavo" (border-radius para DENTRO): quadrado no canto menos um quarto de círculo centrado no canto oposto, com degradê macio
        vec2 o = -uUv.xy;                                   // centro do círculo = canto oposto do quadrado (em vP, -1..1)
        float d = length(vP - o) * 0.5;                     // 1.0 = na linha do arco (raio = lado do quadrado)
        float k = clamp((d - 1.00) / 0.41, 0.0, 1.0);       // 0 em todo o arco e dentro dele -> 1 no vértice do canto (alfa 0 nas pontas do arco: sem corte nas bordas do quadrado)
        outColor = vec4(uColor.rgb, uColor.a * k * k * (3.0 - 2.0 * k));
        return;
    }
    if (uMode == 8) {   // marcador do mapa: uThickness = tipo (0 seta, 1 navio, 2 ponto), uUv.x = yaw
        float c = cos(uUv.x), sn = sin(uUv.x);
        vec2 f = vec2(sn, -c), rt = vec2(c, sn);                 // frente e direita na tela (y para baixo)
        vec2 q = vec2(dot(vP, rt), dot(vP, f));                  // coordenadas locais: +y = frente
        float d;
        if (uThickness < 0.5) {                                  // seta com recorte atrás
            vec2 P[4] = vec2[4](vec2(0.0, 0.95), vec2(0.62, -0.80), vec2(0.0, -0.34), vec2(-0.62, -0.80));
            float dd = dot(q - P[0], q - P[0]); float sg = 1.0;
            for (int i = 0, j = 3; i < 4; j = i, ++i) {
                vec2 e = P[j] - P[i], w = q - P[i];
                vec2 b = w - e * clamp(dot(w, e) / dot(e, e), 0.0, 1.0);
                dd = min(dd, dot(b, b));
                bvec3 cnd = bvec3(q.y >= P[i].y, q.y < P[j].y, e.x * w.y > e.y * w.x);
                if (all(cnd) || all(not(cnd))) sg = -sg;
            }
            d = sg * sqrt(dd);
        } else if (uThickness < 1.5) {                           // casco do navio
            vec2 P[5] = vec2[5](vec2(0.0, 0.95), vec2(0.50, 0.05), vec2(0.38, -0.85), vec2(-0.38, -0.85), vec2(-0.50, 0.05));
            float dd = dot(q - P[0], q - P[0]); float sg = 1.0;
            for (int i = 0, j = 4; i < 5; j = i, ++i) {
                vec2 e = P[j] - P[i], w = q - P[i];
                vec2 b = w - e * clamp(dot(w, e) / dot(e, e), 0.0, 1.0);
                dd = min(dd, dot(b, b));
                bvec3 cnd = bvec3(q.y >= P[i].y, q.y < P[j].y, e.x * w.y > e.y * w.x);
                if (all(cnd) || all(not(cnd))) sg = -sg;
            }
            d = sg * sqrt(dd);
        } else d = length(vP) - 0.55;
        float aa = fwidth(d) + 1e-4;
        float body = 1.0 - smoothstep(-0.14 - aa, -0.14 + aa, d * 1.0 + 0.0);      // miolo colorido
        float rim = 1.0 - smoothstep(0.0 - aa, 0.0 + aa, d);                       // contorno claro
        vec3 col = mix(vec3(0.93, 0.87, 0.72), uColor.rgb, clamp(body, 0.0, 1.0));
        outColor = vec4(col, rim * uColor.a);
        return;
    }
    if (uMode == 7 && uThickness > 2.5) {   // mapa grande = folha de papel: disco de borda irregular e QUEIMADA (estática, estilo cartoon) sobre o fundo preto
        vec4 tt = texture(uIcon, mix(uUv.xy, uUv.zw, vP * 0.5 + 0.5));
        highp vec2 pr = vP * uRect.zw;                                  // pixel em relação ao centro da tela
        highp vec2 q = uRect.xy + pr - uSheet.xy;
        bool rectM = uSheet.w > 0.5;                              // folha RETANGULAR (mapa de piratas): meia-largura uSheet.z, meia-altura uSheet.w
        highp float R = rectM ? min(uSheet.z, uSheet.w) : uSheet.z, ang = atan(q.y, q.x);
        // contorno orgânico sutil de pergaminho antigo (sem mordidas exageradas de cartoon)
        highp float rad = 0.0045 * sin(ang * 3.0 + 0.6) + 0.0030 * sin(ang * 7.0 + 1.7) + 0.0018 * sin(ang * 15.0 + 4.0)
                        + 0.0010 * sin(ang * 31.0 + 0.3);
        highp float d;
        if (rectM) { highp vec2 e = abs(q) - (vec2(uSheet.z, uSheet.w) - 18.0); d = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0) - 18.0 - R * rad; }
        else d = length(q) - R * (1.0 + rad);
        float m = 1.0 - smoothstep(-1.0, 1.0, d);
        if (uColor.b > 0.5) {   // papel da carta náutica
            if (rectM) {
                // 1) linhas de rumo (rhumb lines) saindo da rosa dos ventos (uGrid.xy, raio uGrid.z)
                highp vec2 v = uRect.xy + pr - uGrid.xy;
                highp float rr = length(v), a16 = atan(v.y, v.x), st = 6.2831853 / 16.0;
                float d1 = rr * sin(abs(mod(a16 + 0.5 * st, st) - 0.5 * st));
                float d2 = rr * sin(abs(mod(a16, st) - 0.5 * st));
                float l1 = 1.0 - smoothstep(0.35, 1.25, d1), l2 = 1.0 - smoothstep(0.30, 1.0, d2);
                float far = smoothstep(uGrid.z * 0.98, uGrid.z * 1.12, rr);
                tt.rgb = mix(tt.rgb, vec3(0.32, 0.22, 0.11), far * max(l1 * 0.18, l2 * 0.08));
            } else {
                highp vec2 gc = (uGrid.xy + pr * uGrid.z) / uGrid.w;
                vec2 fl = abs(fract(gc) - 0.5), wd = fwidth(gc) * 1.0 + 1e-5;
                float gl = max(smoothstep(0.5 - wd.x * 1.5, 0.5, fl.x), smoothstep(0.5 - wd.y * 1.5, 0.5, fl.y));
                tt.rgb = mix(tt.rgb, vec3(0.42, 0.30, 0.15), gl * 0.30);
            }
            // envelhecimento sutil nas bordas (sépia e marrom-escuro natural, zero laranja-neon cartoon)
            float edgeSoft = smoothstep(-52.0, -2.0, d);
            float edgeDark = smoothstep(-10.0, 0.0, d);
            float edgeRim  = smoothstep(-2.4, 0.0, d);
            vec3 c = tt.rgb;
            c = mix(c, c * vec3(0.84, 0.75, 0.60), edgeSoft * 0.65);
            c = mix(c, vec3(0.28, 0.18, 0.10), edgeDark * 0.72);
            c = mix(c, vec3(0.11, 0.07, 0.04), edgeRim * 0.90);
            // sombra projetada suave da folha sobre a mesa de carvalho
            float drop = (1.0 - smoothstep(0.0, 18.0, d)) * 0.48 * (1.0 - m);
            outColor = vec4(mix(vec3(0.02, 0.015, 0.01), c, m), max(m, drop) * uColor.a);
        } else outColor = vec4(tt.rgb, tt.a * m * uColor.a);
        return;
    }
    if (uMode == 7) {   // mapa: textura inteira no retângulo; uThickness > 0.5 = recorte redondo
        vec4 t;
        if (uThickness > 1.5) {   // minimapa 3D: girado pela câmera, chão em perspectiva, relevo extrudado e grade
            float sy = sin(uUv.w), cy = cos(uUv.w);
            float u = -vP.y, den = 1.0 - 0.5 * u;
            vec2 g0 = vec2(vP.x * 0.6, u * 0.5) / den;                      // chão (x direita, y frente) em unidades de 260 m
            vec2 off = g0.y * vec2(sy, -cy) + g0.x * vec2(cy, sy);
            vec2 p2 = vP + vec2(0.0, 0.075);                                 // ponto um pouco mais perto da câmera (altura da terra)
            float u2 = -p2.y, den2 = 1.0 - 0.5 * u2;
            vec2 g2 = vec2(p2.x * 0.6, u2 * 0.5) / den2;
            vec2 off2 = g2.y * vec2(sy, -cy) + g2.x * vec2(cy, sy);
            if (uColor.b > 0.5) {                                            // papel + grade no chão
                t = texture(uIcon, uUv.xy + off * uUv.z);
                vec2 gc = vec2(uColor.r, uColor.g) + off * 5.2;              // linhas a cada 50 m
                vec2 fl = abs(fract(gc) - 0.5);
                vec2 w = fwidth(gc) * 1.2 + 1e-4;
                float ln = max(1.0 - smoothstep(0.5 - w.x * 2.0, 0.5, fl.x + 0.0), 0.0);
                float gl = max(smoothstep(0.5 - w.x * 1.5, 0.5, fl.x), smoothstep(0.5 - w.y * 1.5, 0.5, fl.y));
                t.rgb = mix(t.rgb, vec3(0.42, 0.30, 0.15), gl * 0.35);
            } else {
                vec4 top = texture(uIcon, uUv.xy + off2 * uUv.z);
                vec4 base = texture(uIcon, uUv.xy + off * uUv.z);
                float wall = clamp(base.a - top.a, 0.0, 1.0);
                t = vec4(mix(top.rgb, vec3(0.38, 0.24, 0.11), wall), max(top.a, base.a));
            }
            float haze = smoothstep(0.1, 1.0, u);                            // horizonte: some no fundo claro
            t.rgb = mix(t.rgb, vec3(0.80, 0.72, 0.55), haze * 0.55);
            t.a *= 1.0 - haze * 0.25;
        } else t = texture(uIcon, mix(uUv.xy, uUv.zw, vP * 0.5 + 0.5));
        float m = 1.0;
        if (uThickness > 0.5) { float d = length(vP); m = 1.0 - smoothstep(0.985, 1.0, d); }
        outColor = vec4(t.rgb, t.a * m * uColor.a);
        return;
    }
    if (uMode == 5) {
        vec4 t = texture(uIcon, mix(uUv.xy, uUv.zw, vP * 0.5 + 0.5));
        outColor = vec4(t.rgb * uColor.rgb, t.a * uColor.a);
        return;
    }
    outColor = vec4(uColor.rgb, uColor.a * a);
})";

// BANDEIRA PIRATA (editor de bandeira): pano procedural desenhado no retângulo da UI. Coordenadas do pano: s = 0 no mastro -> 1 na ponta,
// v = 0 em cima -> 1 embaixo; p = (s * aspecto, v) em alturas. Padrão (cor de detalhe sobre o fundo), recorte da ponta, desgaste (manchas, rasgos,
// furos queimados), figura do atlas de ícones (com contorno opcional) e ONDULAÇÃO presa no mastro (uWave.y = 0: parada, para miniaturas).
static const char* kFlagFS = R"(#version 300 es
precision highp float;
in highp vec2 vP;
in highp vec2 vPx;
uniform vec2 uCloth;      // meia largura / meia altura do pano (px)
uniform vec3 uC0, uC1, uC2;
uniform ivec4 uStyle;     // padrão, recorte da ponta, desgaste, bits (1 espelhar figura, 2 contorno, 4 com figura)
uniform vec4 uIconUv;     // célula da figura no atlas
uniform vec4 uIconT;      // centro da figura (s, v), meia altura (fração da altura), semente
uniform vec3 uWave;       // tempo, amplitude, alfa
uniform sampler2D uIcon;
out vec4 outColor;
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), f.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}
float cov(float d) { float w = fwidth(d) + 1e-5; return clamp(0.5 - d / w, 0.0, 1.0); }   // d < 0 = dentro
float iconA(vec2 q) {    // alfa da figura em q (-1..1)
    float e = max(abs(q.x), abs(q.y));
    if (e > 1.0) return 0.0;
    vec2 m = (uIconUv.zw - uIconUv.xy) * 0.03;   // não encosta na borda da célula (o filtro/mipmap puxava as figuras vizinhas do atlas)
    return texture(uIcon, mix(uIconUv.xy + m, uIconUv.zw - m, q * 0.5 + 0.5)).a * (1.0 - smoothstep(0.90, 1.0, e));
}
void main() {
    float t = uWave.x, amp = uWave.y, seed = uIconT.w;
    float s = (vPx.x + uCloth.x) / (2.0 * uCloth.x);
    float env = amp * smoothstep(0.0, 0.30, s) * (0.55 + 0.45 * s);              // preso no mastro, solto na ponta
    float ph1 = s * 6.5 - t * 3.3, ph2 = s * 12.5 - t * 5.2 + 1.3;
    float disp = env * (0.12 * sin(ph1) + 0.045 * sin(ph2));                       // em alturas do pano
    float slope = env * (0.12 * 6.5 * cos(ph1) + 0.045 * 12.5 * cos(ph2));
    float v = (vPx.y + uCloth.y) / (2.0 * uCloth.y) - disp * 0.5;
    float A = uCloth.x / uCloth.y;
    vec2 p = vec2(s * A, v);
    int W = uStyle.z;
    // bordas gastas: o contorno do pano ganha ruído
    float fray = W == 0 ? 0.0 : (W == 1 ? 0.010 : 0.030);
    float n1 = vnoise(p * 9.0 + seed * 7.0) - 0.5, n2 = vnoise(p * 23.0 + seed * 3.0) - 0.5;
    float ss = s + fray * (n1 + n2 * 0.6) * 2.0, vv = v + fray * (n2 + n1 * 0.5) * 2.0 * A * 0.5;
    float d = max(max(-ss, ss - 1.0), max(-vv, vv - 1.0));
    float c2v = abs(2.0 * vv - 1.0);
    int E = uStyle.y;
    if (E == 1) d = max(d, ss - (1.0 - 0.30 * (1.0 - c2v)));                                       // rabo de andorinha
    else if (E == 2) d = max(d, (c2v - (1.0 - 0.84 * ss)) * 0.5);                                   // flâmula (triângulo)
    else if (E == 3) d = max(d, ss - (1.0 - 0.24 * c2v));                                           // bico
    else if (E == 4) d = max(d, ss - (0.88 + 0.12 * abs(fract(vv * 4.0) - 0.5) * 2.0));            // serrilhada
    else if (E == 5) d = max(d, ss - (0.80 + 0.12 * vnoise(vec2(vv * 7.0, seed * 5.0)) + 0.08 * vnoise(vec2(vv * 26.0, seed))));   // rasgada
    float burn = 0.0;
    if (W == 2) {   // farrapo: furos com a borda queimada
        float h = vnoise(p * 4.2 + seed * 11.0) * 0.62 + vnoise(p * 10.0 + seed * 2.0) * 0.38;
        d = max(d, (h - 0.70) * 0.6);
        burn = smoothstep(0.56, 0.70, h);
    }
    float alpha = cov(d) * uWave.z;
    if (alpha <= 0.002) discard;
    // padrão
    int P = uStyle.x;
    vec2 ec = vec2(uIconT.x * A, uIconT.y);
    float m = 0.0;
    if (P == 1) m = cov(abs(v - 0.5) - 0.17);
    else if (P == 2) m = cov(0.5 - v);
    else if (P == 3) m = cov(abs(fract(v * 2.5) - 0.25) - 0.25);
    else if (P == 4) m = cov(s - 0.34);
    else if (P == 5) m = cov((1.0 - s) - v);
    else if (P == 6) m = cov(min(abs(v - 0.5), abs(p.x - A * 0.36)) - 0.085);
    else if (P == 7) { float k = 1.0 / sqrt(1.0 + A * A); m = cov(min(abs(p.x - p.y * A), abs(p.x + p.y * A - A)) * k - 0.085); }
    else if (P == 8) m = cov(sin(s * 6.0 * 3.14159) * sin(v * 4.0 * 3.14159));
    else if (P == 9) m = cov(min(min(p.x, A - p.x), min(v, 1.0 - v)) - 0.085);
    else if (P == 10) m = cov(max(s - 0.45, v - 0.5));
    else if (P == 11) { float w2 = v + 0.05 * sin(p.x * 9.0); m = cov(abs(fract(w2 * 3.0) - 0.25) - 0.25); }
    else if (P == 12) { vec2 q = p - ec; m = cov(sin(atan(q.y, q.x) * 12.0)) * smoothstep(0.02, 0.10, length(q)); }
    else if (P == 13) m = cov(s - 0.42 * (1.0 - abs(2.0 * v - 1.0)));
    else if (P == 14) m = cov(length(p - ec) - uIconT.z * 1.35);
    else if (P == 15) m = cov(min(v, 1.0 - v) - 0.16);
    else if (P == 16) m = cov(abs(s - 0.5) - 0.17);                                                   // pala (faixa vertical no meio)
    else if (P == 17) m = cov(-(s - 0.5) * (v - 0.5) * 8.0);                                          // quartos
    else if (P == 18) m = cov(abs(s - 0.5) * 2.0 + abs(v - 0.5) * 2.0 - 0.9);                        // losango
    else if (P == 19) { vec2 g = fract(p * 3.2) - 0.5; m = cov(length(g) - 0.22); }                   // bolinhas
    else if (P == 20) m = cov(abs(fract((p.x + v) * 2.2) - 0.25) - 0.25);                             // listras diagonais
    else if (P == 21) m = cov(abs(fract((p.x * 0.8 - abs(v - 0.5)) * 2.0) - 0.25) - 0.25);           // chevron
    else if (P == 22) { vec2 q = p - vec2(A * 0.5, 1.08); m = max(cov(sin(atan(q.y, q.x) * 14.0)), cov(length(q) - 0.42)); }   // sol nascente
    else if (P == 23) m = cov(min(abs(v - 0.5), abs(p.x - A * 0.5)) - 0.09);                         // cruz grega
    else if (P == 24) m = cov(abs(length(p - ec) - uIconT.z * 1.45) - 0.045);                        // anel em volta da figura
    else if (P == 25) m = cov(s - (0.10 + 0.07 * abs(fract(v * 5.0) - 0.5) * 2.0));                  // dentes no mastro
    else if (P == 26) m = cov(sin((p.x + v) * 9.0) * sin((p.x - v) * 9.0));                          // arlequim
    else if (P == 27) { float e = min(min(p.x, A - p.x), min(v, 1.0 - v)); m = max(cov(abs(e - 0.05) - 0.025), cov(abs(e - 0.14) - 0.025)); }   // moldura dupla
    else if (P == 28) { vec2 g = abs(fract(p * 4.0) - 0.5); m = cov(0.43 - max(g.x, g.y)); }       // grade
    else if (P == 29) { float k = 1.0 / sqrt(1.0 + A * A); m = cov(abs(p.x - p.y * A) * k - 0.12); }   // faixa diagonal
    else if (P == 30) m = cov(length(p - vec2(0.0, 0.5)) - 0.52);                                     // semicírculo no mastro
    vec3 col = mix(uC0, uC1, m);
    // figura
    if ((uStyle.w & 4) != 0) {
        vec2 q = (p - ec) / max(uIconT.z, 0.01);
        if ((uStyle.w & 1) != 0) q.x = -q.x;
        float a = iconA(q);
        if ((uStyle.w & 2) != 0) {
            float o = 0.0, r = 0.075;
            for (int i = 0; i < 8; ++i) { float an = float(i) * 0.785398; o = max(o, iconA(q + vec2(cos(an), sin(an)) * r)); }
            float lum = dot(uC2, vec3(0.30, 0.59, 0.11));
            vec3 oc = lum > 0.45 ? vec3(0.04, 0.03, 0.03) : vec3(0.93, 0.88, 0.76);
            col = mix(col, oc, clamp(o - a, 0.0, 1.0) * 0.95);
        }
        col = mix(col, uC2, a);
    }
    // tecido, desgaste e luz
    float fab = 1.0 + (vnoise(vec2(p.x * 70.0, p.y * 9.0)) - 0.5) * 0.07 + (vnoise(p * 140.0) - 0.5) * 0.05;
    col *= fab;
    if (W >= 1) {
        float lum = dot(col, vec3(0.30, 0.59, 0.11));
        col = mix(col, vec3(lum) * vec3(1.05, 0.98, 0.86), W == 1 ? 0.20 : 0.34);                  // desbotada
        float st = smoothstep(0.55, 0.90, vnoise(p * 3.0 + seed * 13.0));
        col *= 1.0 - st * (W == 1 ? 0.22 : 0.34);                                                   // manchas
        col *= 1.0 - smoothstep(0.75, 1.0, s) * (W == 1 ? 0.10 : 0.22);                              // ponta suja
    }
    col = mix(col, col * vec3(0.30, 0.18, 0.10), burn * 0.85);
    float shade = 1.0 - 0.30 * slope + 0.10 * amp * smoothstep(0.6, 1.0, cos(ph1 - 0.4));
    col *= clamp(shade, 0.55, 1.35);
    col *= 0.86 + 0.14 * smoothstep(0.0, 0.06, s);                                                   // sombra junto ao mastro
    outColor = vec4(col, alpha);
})";

static const char* kTextVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUv;
layout(location=2) in vec4 aCol;
uniform vec2 uScreen;
out vec2 vUv;
out vec4 vCol;
void main() {
    vUv = aUv; vCol = aCol;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
})";

static const char* kTextFS = R"(#version 300 es
precision mediump float;
in vec2 vUv;
in vec4 vCol;
uniform sampler2D uTex;
out vec4 outColor;
void main() { outColor = vec4(vCol.rgb, vCol.a * texture(uTex, vUv).r); })";

// Atlas: 16 colunas x 5 linhas de células 8x8 (ASCII 32..95 + célula sólida na posição 64)
static constexpr int kAtlasW = 128, kAtlasH = 40, kSolidCell = 64;


static unsigned link(const char* vsSrc, const char* fsSrc);

static unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        LOG("shader error: %s", buf);
        return 0;
    }
    return s;
}

static unsigned link(const char* vsSrc, const char* fsSrc) {
    unsigned vs = compile(GL_VERTEX_SHADER, vsSrc), fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return 0;
    unsigned p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { LOG("program link failed"); return 0; }
    return p;
}

static std::string frag(const char* main, const char* defs = "") { return std::string(kHdr) + defs + kLib + main; }

static int loc(unsigned p, const char* n) { return glGetUniformLocation(p, n); }

static void lookupLit(unsigned p, int* out /*18 ints, ordem de Renderer::Lit*/) {
    static const char* names[18] = {"uCam", "uAmbient", "uLightDir", "uLightCol", "uHorizon", "uZenith", "uSunDir", "uNight",
                                    "uShadowOn", "uShadowTexel", "uShadowRadius", "uPcf", "uShadowCenter", "uShadowInv", "uShadowMat", "uShadow", "uTorch", "uFogK"};
    for (int i = 0; i < 18; ++i) out[i] = loc(p, names[i]);
}

#pragma pack(push, 1)
struct E0THeader {
    uint32_t magic;      // 'E0T1' (0x31543045)
    uint16_t width;
    uint16_t height;
    uint16_t layers;
    uint16_t format;
    uint32_t dataSize;
};
#pragma pack(pop)

static GLuint loadTexture2DArray(AAssetManager* assets, const char* name) {
    if (!assets) return 0;
    AAsset* a = AAssetManager_open(assets, name, AASSET_MODE_BUFFER);
    if (!a) {
        LOG("loadTexture2DArray: arquivo %s nao encontrado no APK", name);
        return 0;
    }
    const uint8_t* buf = (const uint8_t*)AAsset_getBuffer(a);
    size_t len = AAsset_getLength(a);
    if (len < sizeof(E0THeader)) {
        LOG("loadTexture2DArray: arquivo %s truncado (%zu bytes)", name, len);
        AAsset_close(a);
        return 0;
    }
    const E0THeader* hdr = (const E0THeader*)buf;
    if (hdr->magic != 0x31543045) {
        LOG("loadTexture2DArray: magic invalido em %s: %08x", name, hdr->magic);
        AAsset_close(a);
        return 0;
    }
    if (len < sizeof(E0THeader) + hdr->dataSize) {
        LOG("loadTexture2DArray: dados incompletos em %s", name);
        AAsset_close(a);
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    float mb = 0.0f;
    if (hdr->format >= 104 && hdr->format <= 112) {
        // ASTC NxN com mipmaps pré-gerados (tools/compress_pbr.py): [u32 tamanho][todas as camadas] por nível.
        // GL_COMPRESSED_RGBA_ASTC_4x4_KHR = 0x93B0, 5x5 = 0x93B2, 6x6 = 0x93B4, 8x8 = 0x93B7.
        int blk = hdr->format - 100;
        GLenum fmt = blk == 4 ? 0x93B0 : blk == 5 ? 0x93B2 : blk == 6 ? 0x93B4 : 0x93B7;
        const uint8_t* p = buf + sizeof(E0THeader);
        const uint8_t* end = p + hdr->dataSize;
        int lw = hdr->width, lh = hdr->height, lvl = 0;
        while (p + 4 <= end) {
            uint32_t sz; memcpy(&sz, p, 4); p += 4;
            if (p + sz > end) break;
            glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, lvl, fmt, lw, lh, hdr->layers, 0, (GLsizei)sz, p);
            p += sz; mb += sz / 1048576.0f; ++lvl;
            if (lw == 1 && lh == 1) break;
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, lvl - 1);
        if (glGetError() != GL_NO_ERROR) LOG("AVISO: erro GL ao carregar ASTC de %s", name);
    } else {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, hdr->width, hdr->height, hdr->layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf + sizeof(E0THeader));
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        mb = (hdr->width * hdr->height * hdr->layers * 4) / 1048576.0f;
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    LOG("Textura 2D Array %s carregada: %dx%dx%d formato %d (%.1f MB)", name, hdr->width, hdr->height, hdr->layers, hdr->format, mb);
    AAsset_close(a);   // depois do LOG: hdr aponta para o buffer do asset
    return tex;
}

bool Renderer::init() {
    LOG("GL_RENDERER: %s | GL_VERSION: %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    ready_ = false;
    groundProg_ = link(kGroundVS, frag(kGroundMain).c_str());
    charProg_ = link(kCharVS, frag(kCharMain).c_str());
    skyProg_ = link(kSkyVS, frag(kSkyMain).c_str());
    terrainProg_ = link(kTerrainVS, frag(kTerrainMain).c_str());
    propProg_ = link(kPropVS, frag(kPropMain).c_str());
    shipProg_ = link(kShipVS, frag(kPropMain).c_str());
    grassProg_ = link(kGrassVS, frag(kPropMain, "#define CLIP_DISCARD\n").c_str());
    markerProg_ = link(kMarkerVS, kMarkerFS);
    gizmoProg_ = link(kGizmoVS, kGizmoFS);
    splashProg_ = link(kSplashVS, kSplashFS);
    flameProg_ = link(kSplashVS, kFlameFS);
    glowProg_ = link(kSplashVS, kGlowFS);
    shadowProg_ = link(kShadowVS, kShadowFS);
    textProg_ = link(kTextVS, kTextFS);
    uiProg_ = link(kUiVS, kUiFS);
    flagProg_ = link(kUiVS, kFlagFS);
    clothProg_ = link(kClothVS, frag(kClothMain).c_str());
    if (clothProg_) {
        lookupLit(clothProg_, (int*)&litCloth_);
        uClothVP_ = loc(clothProg_, "uVP"); uClothBase_ = loc(clothProg_, "uBase"); uClothDir_ = loc(clothProg_, "uDir");
        uClothSize_ = loc(clothProg_, "uSize"); uClothTime_ = loc(clothProg_, "uTime"); uClothTex_ = loc(clothProg_, "uClothTex");
    } else LOG("AVISO: shader do pano da bandeira falhou");
    if (!flagProg_) LOG("AVISO: shader da bandeira falhou");
    gradeProg_ = link(kGradeVS, kGradeFS);
    if (!groundProg_ || !charProg_ || !skyProg_ || !shadowProg_ || !textProg_ || !uiProg_ || !terrainProg_ || !grassProg_ || !markerProg_ || !gizmoProg_ || !splashProg_ || !flameProg_ || !glowProg_ || !propProg_ || !shipProg_) return false;

    static_assert(sizeof(Lit) == 18 * sizeof(int), "Lit deve ter 18 ints");
    lookupLit(groundProg_, (int*)&litGround_);
    lookupLit(charProg_, (int*)&litChar_);
    lookupLit(skyProg_, (int*)&litSky_);
    lookupLit(terrainProg_, (int*)&litTerrain_);
    lookupLit(grassProg_, (int*)&litGrass_);
    lookupLit(propProg_, (int*)&litProp_);
    lookupLit(shipProg_, (int*)&litShip_);
    uShipVP_ = loc(shipProg_, "uVP"); uShipModel_ = loc(shipProg_, "uModel"); uShipClip_ = loc(shipProg_, "uClipY"); uShipTime_ = loc(shipProg_, "uTime");
    uMarkerVP_ = loc(markerProg_, "uVP");
    if (gradeProg_) { uGradeK_ = loc(gradeProg_, "uK"); uGradeAspect_ = loc(gradeProg_, "uAspect"); }
    glGenVertexArrays(1, &markerVao_); glGenBuffers(1, &markerVbo_);
    glBindVertexArray(markerVao_);
    glBindBuffer(GL_ARRAY_BUFFER, markerVbo_);
    glBufferData(GL_ARRAY_BUFFER, 6 * 16, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 16, (void*)12);
    glBindVertexArray(0);
    uGizmoVP_ = loc(gizmoProg_, "uVP");
    glGenVertexArrays(1, &gizmoVao_); glGenBuffers(1, &gizmoVbo_);
    glBindVertexArray(gizmoVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo_);
    glBufferData(GL_ARRAY_BUFFER, 4096 * 28, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void*)12);
    glBindVertexArray(0);
    fxProg_ = link(kFxVS, kFxFS); uFxVP_ = loc(fxProg_, "uVP");
    glGenVertexArrays(1, &fxVao_); glGenBuffers(1, &fxVbo_);
    glBindVertexArray(fxVao_);
    glBindBuffer(GL_ARRAY_BUFFER, fxVbo_);
    glBufferData(GL_ARRAY_BUFFER, 640 * 6 * 36, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, (void*)20);
    glBindVertexArray(0);
    uSplashVP_ = loc(splashProg_, "uVP"); uFlameVP_ = loc(flameProg_, "uVP"); uFlameTime_ = loc(flameProg_, "uTime"); uGlowVP_ = loc(glowProg_, "uVP");
    glGenVertexArrays(1, &splashVao_); glGenBuffers(1, &splashVbo_);
    glBindVertexArray(splashVao_);
    glBindBuffer(GL_ARRAY_BUFFER, splashVbo_);
    glBufferData(GL_ARRAY_BUFFER, 480 * 6 * 24, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 24, (void*)20);
    glBindVertexArray(0);
    uGrassVP_ = loc(grassProg_, "uVP"); uGrassTime_ = loc(grassProg_, "uTime"); uGrassDist_ = loc(grassProg_, "uGrassDist"); uGrassClip_ = loc(grassProg_, "uClipY");
    uTerrainVP_ = loc(terrainProg_, "uVP"); uTerrainClip_ = loc(terrainProg_, "uClipY");
    uTerrainAlbedo_ = loc(terrainProg_, "uAlbedoArray");
    uTerrainNormal_ = loc(terrainProg_, "uNormalArray");
    uPropVP_ = loc(propProg_, "uVP"); uPropClip_ = loc(propProg_, "uClipY"); uPropTime_ = loc(propProg_, "uTime");
    texTerrainAlbedo_ = loadTexture2DArray(assets_, "terrain_albedo.e0t");
    texTerrainNormal_ = loadTexture2DArray(assets_, "terrain_normal.e0t");
    uGroundTime_ = loc(groundProg_, "uTime"); uGroundOff_ = loc(groundProg_, "uOff");
    uGroundVP_ = loc(groundProg_, "uVP");
    uGroundGloss_ = loc(groundProg_, "uGloss"); uGroundScreenInv_ = loc(groundProg_, "uScreenInv");
    uCharMVP_ = loc(charProg_, "uMVP"); uCharModel_ = loc(charProg_, "uModel"); uCharColor_ = loc(charProg_, "uColor"); uCharSkinned_ = loc(charProg_, "uSkinned"); uCharBones_ = loc(charProg_, "uBones"); uCharClip_ = loc(charProg_, "uClipY"); uCharAlpha_ = loc(charProg_, "uAlpha");
    uCharTex_ = loc(charProg_, "uCharTex"); uCharTexOn_ = loc(charProg_, "uTexOn");
    uCharSkin_ = loc(charProg_, "uSkin"); uCharSkin2_ = loc(charProg_, "uSkin2"); uCharSkinT_ = loc(charProg_, "uSkinT");
    uShadowMVP_ = loc(shadowProg_, "uMVP"); uShadowSkinned_ = loc(shadowProg_, "uSkinned"); uShadowBones_ = loc(shadowProg_, "uBones");
    AnimBank::fillIdentityRows(identBones_);
    uSkyFwd_ = loc(skyProg_, "uFwd"); uSkyRight_ = loc(skyProg_, "uRight"); uSkyUp_ = loc(skyProg_, "uUp");
    uSkyTan_ = loc(skyProg_, "uTan");

    uTextScreen_ = loc(textProg_, "uScreen"); uTextTex_ = loc(textProg_, "uTex");
    uUiRect_ = loc(uiProg_, "uRect"); uUiScreen_ = loc(uiProg_, "uScreen"); uUiColor_ = loc(uiProg_, "uColor"); uUiMode_ = loc(uiProg_, "uMode"); uUiIcon_ = loc(uiProg_, "uIcon"); uUiUv_ = loc(uiProg_, "uUv");
    if (flagProg_) {
        uFlagRect_ = loc(flagProg_, "uRect"); uFlagScreen_ = loc(flagProg_, "uScreen"); uFlagCloth_ = loc(flagProg_, "uCloth");
        uFlagC0_ = loc(flagProg_, "uC0"); uFlagC1_ = loc(flagProg_, "uC1"); uFlagC2_ = loc(flagProg_, "uC2"); uFlagStyle_ = loc(flagProg_, "uStyle");
        uFlagIconUv_ = loc(flagProg_, "uIconUv"); uFlagIconT_ = loc(flagProg_, "uIconT"); uFlagWave_ = loc(flagProg_, "uWave"); uFlagIcon_ = loc(flagProg_, "uIcon");
    }
    uUiRadius_ = loc(uiProg_, "uRadius"); uUiThickness_ = loc(uiProg_, "uThickness"); uUiSheet_ = loc(uiProg_, "uSheet"); uUiGrid_ = loc(uiProg_, "uGrid");

    {   // atlas da fonte
        static uint8_t px[kAtlasW * kAtlasH];
        std::memset(px, 0, sizeof px);
        auto put = [&](int cell, const uint8_t* rows) {
            int cx = (cell % 16) * 8, cy = (cell / 16) * 8;
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    if (rows[y] & (0x10 >> x)) px[(cy + y) * kAtlasW + cx + 1 + x] = 255;
        };
        for (const Glyph& g : kFont) put(g.c - 32, g.r);
        const uint8_t solid[7] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
        put(kSolidCell, solid);
        {   // marcas de acento: células 65..70 (grave, agudo, circunflexo, til, trema nas linhas 4-5, desenhadas 7 linhas acima da letra; cedilha nas linhas 0-1, 7 abaixo)
            static const uint8_t mk[6][7] = {{0, 0, 0, 0, 0x08, 0x04, 0}, {0, 0, 0, 0, 0x02, 0x04, 0}, {0, 0, 0, 0, 0x04, 0x0A, 0},
                                              {0, 0, 0, 0, 0x0D, 0x16, 0}, {0, 0, 0, 0, 0, 0x0A, 0}, {0x04, 0x0C, 0, 0, 0, 0, 0}};
            for (int k = 0; k < 6; ++k) put(65 + k, mk[k]);
        }
        glGenTextures(1, &fontTex_);
        glBindTexture(GL_TEXTURE_2D, fontTex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (assets_) {   // fonte Cinzel de alta resolução (assets/ui_font.e0i, gerada por tools/make_ui_font.py)
            if (AAsset* fa = AAssetManager_open(assets_, "ui_font.e0i", AASSET_MODE_BUFFER)) {
                const uint8_t* b = (const uint8_t*)AAsset_getBuffer(fa);
                size_t len = (size_t)AAsset_getLength(fa);
                if (len >= 8 && std::memcmp(b, "E0F1", 4) == 0) {
                    int fw = b[4] | (b[5] << 8), fh = b[6] | (b[7] << 8);
                    if (fw == kUiFontW && fh == kUiFontH && len >= 8 + (size_t)fw * fh) {
                        glGenTextures(1, &uiFontTex_);
                        glBindTexture(GL_TEXTURE_2D, uiFontTex_);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, fw, fh, 0, GL_RED, GL_UNSIGNED_BYTE, b + 8);
                        glGenerateMipmap(GL_TEXTURE_2D);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        LOG("ui_font.e0i carregada: %dx%d (Cinzel)", fw, fh);
                    }
                }
                AAsset_close(fa);
            }
        }
        glGenVertexArrays(1, &textVao_);
        glGenBuffers(1, &textVbo_);
        glBindVertexArray(textVao_);
        glBindBuffer(GL_ARRAY_BUFFER, textVbo_);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, nullptr);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)8);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void*)16);
        glBindVertexArray(0);
    }
    {   // quad [-1,1] (UI e céu)
        const float quad2[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        glGenVertexArrays(1, &uiVao_);
        glGenBuffers(1, &uiVbo_);
        glBindVertexArray(uiVao_);
        glBindBuffer(GL_ARRAY_BUFFER, uiVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad2, quad2, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }
    {   // chão / mar: plano único na superfície em y = 0.00 m
        const float S = 1000.0f;  // plano grande; cobre até o horizonte
        const float quad[] = {-S, 0.0f, -S,  S, 0.0f, -S,  S, 0.0f, S,  -S, 0.0f, -S,  S, 0.0f, S,  -S, 0.0f, S};
        glGenVertexArrays(1, &groundVao_);
        glGenBuffers(1, &groundVbo_);
        glBindVertexArray(groundVao_);
        glBindBuffer(GL_ARRAY_BUFFER, groundVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }
    // PERSONAGENS: piratas (tools/rig_pirates.py no Blender + tools/export_rig.py), cada um com o SEU esqueleto (pirate_<m|f>.e0k) e a malha na pose em
    // que foi modelada; as animações do ybot.e0a valem para os dois (ver Skeleton em anim.h). 3 LODs + 1 textura por LOD.
    static const char* kCharFiles[kCharModels][3] = {{"pirate_m.e0m", "pirate_m_lod1.e0m", "pirate_m_lod2.e0m"}, {"pirate_f.e0m", "pirate_f_lod1.e0m", "pirate_f_lod2.e0m"},
                                                               {"pirate_g.e0m", "pirate_g_lod1.e0m", "pirate_g_lod2.e0m"}, {"pirate_c.e0m", "pirate_c_lod1.e0m", "pirate_c_lod2.e0m"}};
    static const char* kCharTex[kCharModels][3] = {{"pirate_m.e0i", "pirate_m_lod1.e0i", "pirate_m_lod2.e0i"}, {"pirate_f.e0i", "pirate_f_lod1.e0i", "pirate_f_lod2.e0i"},
                                                             {"pirate_g.e0i", "pirate_g_lod1.e0i", "pirate_g_lod2.e0i"}, {"pirate_c.e0i", "pirate_c_lod1.e0i", "pirate_c_lod2.e0i"}};
    for (int m = 0; m < kCharModels; ++m) {
        for (int i = 0; i < 3; ++i) {
            charTex_[m][i] = loadTexture(kCharTex[m][i]);
            if (!loadMesh(kCharFiles[m][i], chars_[m][i])) LOG("AVISO: %s não carregou", kCharFiles[m][i]);
            chars_[m][i].tex = charTex_[m][i];
        }
        static const char* kSkel[kCharModels] = {"pirate_m.e0k", "pirate_f.e0k", "pirate_g.e0k", "pirate_c.e0k"};
        if (!skel_[m].valid && assets_) {
            if (AAsset* a = AAssetManager_open(assets_, kSkel[m], AASSET_MODE_BUFFER)) {
                skel_[m].loadFromMemory((const uint8_t*)AAsset_getBuffer(a), (size_t)AAsset_getLength(a));
                AAsset_close(a);
            }
            LOG("%s: %s", kSkel[m], skel_[m].valid ? "esqueleto carregado" : "FALHOU (personagem fica deformado)");
        }
    }
    static const char* kToolFiles[5] = {"tool_hatchet.e0m", "tool_pickaxe.e0m", "tool_torch.e0m", "tool_dagger.e0m", "tool_gun.e0m"};
    for (int i = 0; i < 5; ++i)
        if (!loadMesh(kToolFiles[i], toolMeshes_[i])) LOG("AVISO: ferramenta %s nao carregou", kToolFiles[i]);
    makeCube();
    if (!loadShip(0, "ship.e0s")) LOG("AVISO: ship.e0s nao carregou (sem navio)");
    if (!loadShip(1, "galleon.e0s")) LOG("AVISO: galleon.e0s nao carregou (sem galeao)");
    if (!loadShip(2, "boat.e0s")) LOG("AVISO: boat.e0s nao carregou (sem chalupa)");
    if (!loadShip(3, "brig.e0s")) LOG("AVISO: brig.e0s nao carregou (sem bergantim)");
    if (!loadIcons()) LOG("AVISO: icons.e0i nao carregou (ícones de inventário em formas simples)");
    if (!anims_.valid() && assets_) {
        if (AAsset* a = AAssetManager_open(assets_, "ybot.e0a", AASSET_MODE_BUFFER)) {
            bool ok = anims_.loadFromMemory((const uint8_t*)AAsset_getBuffer(a), (size_t)AAsset_getLength(a));
            AAsset_close(a);
            LOG("ybot.e0a: %s (%d ossos, %d clipes)", ok ? "carregado" : "FALHOU", anims_.numBones(), anims_.numClips());
            anims_.setDefaultSkeleton(&skel_[0]);
        } else {
            LOG("AVISO: ybot.e0a não encontrado");
        }
    }

    // timer de GPU (opcional)
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    gpuTimer_ = false;
    if (ext && std::strstr(ext, "GL_EXT_disjoint_timer_query")) {
        qGen = (FnGenQ)eglGetProcAddress("glGenQueriesEXT"); qDel = (FnDelQ)eglGetProcAddress("glDeleteQueriesEXT");
        qBegin = (FnBeginQ)eglGetProcAddress("glBeginQueryEXT"); qEnd = (FnEndQ)eglGetProcAddress("glEndQueryEXT");
        qGet64 = (FnGetQ64)eglGetProcAddress("glGetQueryObjectui64vEXT"); qGeti = (FnGetQi)eglGetProcAddress("glGetQueryObjectivEXT");
        if (qGen && qDel && qBegin && qEnd && qGet64 && qGeti) { qGen(3, gpuQ_); gpuTimer_ = true; }
    }
    gpuMs_ = -1; gpuPending_[0] = gpuPending_[1] = gpuPending_[2] = false;
    LOG("timer de GPU: %s", gpuTimer_ ? "disponível" : "indisponível (qualidade automática usa FPS)");

    createTargets();
    for (int sl = 0; sl < e0net::kIslandSlots; ++sl) if (islSrc_[sl]) {   // contexto GL recriado: reconstrói o terreno e reesconde os coletáveis já pegos
        terr_[sl].build(*islSrc_[sl]);
        for (size_t i = 0; i < hiddenSmall_[sl].size(); ++i) if (hiddenSmall_[sl][i]) terr_[sl].setSmallHidden((uint32_t)i, true);
        for (size_t i = 0; i < hiddenProp_[sl].size(); ++i) if (hiddenProp_[sl][i]) terr_[sl].hideProp((uint32_t)i);
    }
    ready_ = true;
    return true;
}

void Renderer::setIsland(int slot, const e0net::World* w) {
    if (slot < 0 || slot >= e0net::kIslandSlots) return;
    islSrc_[slot] = w;
    hiddenSmall_[slot].assign(w ? w->objects.size() : 0, 0);
    hiddenProp_[slot].assign(w ? w->objects.size() : 0, 0);
    if (slot == 0) ghosts_.clear();
    if (ready_) { terr_[slot].destroy(); if (w) terr_[slot].build(*w); }
}
void Renderer::setTerrain(const e0net::World* w) { setIsland(0, w); }
void Renderer::clearIslands() { for (int i = 1; i < e0net::kIslandSlots; ++i) setIsland(i, nullptr); ghosts_.clear(); }

void Renderer::setCollectibleHidden(uint32_t gid, bool hidden) {
    const int sl = (int)(gid >> 20); const uint32_t objIdx = gid & 0xFFFFFu;
    if (sl >= e0net::kIslandSlots || objIdx >= hiddenSmall_[sl].size() || hiddenSmall_[sl][objIdx] == (uint8_t)hidden) return;
    hiddenSmall_[sl][objIdx] = (uint8_t)hidden;
    if (ready_) terr_[sl].setSmallHidden(objIdx, hidden);
}

void Renderer::removeWorldProp(uint32_t gid, bool effect) {
    const int sl = (int)(gid >> 20); const uint32_t objIdx = gid & 0xFFFFFu;
    if (sl >= e0net::kIslandSlots || objIdx >= hiddenProp_[sl].size() || hiddenProp_[sl][objIdx]) return;
    hiddenProp_[sl][objIdx] = 1;
    if (!ready_) return;
    if (effect) {
        Ghost g;
        if (terr_[sl].propGhost(objIdx, g.verts, g.base)) { g.t0 = lastNow_; g.spin = (objIdx & 1) ? 1.0f : -1.0f; ghosts_.push_back(std::move(g)); }
    }
    terr_[sl].hideProp(objIdx);
}

void Renderer::restoreWorldProp(uint32_t gid) {
    const int sl = (int)(gid >> 20); const uint32_t objIdx = gid & 0xFFFFFu;
    if (sl >= e0net::kIslandSlots || objIdx >= hiddenProp_[sl].size() || !hiddenProp_[sl][objIdx]) return;
    hiddenProp_[sl][objIdx] = 0;
    if (ready_) terr_[sl].showProp(objIdx);
}

// Efeito de sumir: a cópia do objeto encolhe até o pé, girando e afundando um pouco, em ~0.8 s.
void Renderer::drawGhosts() {
    if (ghosts_.empty()) return;
    static std::vector<uint8_t> buf;
    struct V { float x, y, z; int8_t nx, ny, nz, nw; uint8_t r, g, b, a; };
    buf.clear();
    for (size_t gi = 0; gi < ghosts_.size();) {
        Ghost& g = ghosts_[gi];
        float k = (lastNow_ - g.t0) / 0.8f;
        if (k >= 1.0f) { ghosts_.erase(ghosts_.begin() + (long)gi); continue; }
        ++gi;
        float e = k * k * (3.0f - 2.0f * k);          // suave
        float sc = std::max(0.0f, 1.0f - e), sy = std::max(0.0f, 1.0f - e * e * 0.9f);
        float ang = g.spin * e * 1.6f, cs = std::cos(ang), sn = std::sin(ang);
        const V* src = (const V*)g.verts.data();
        size_t n = g.verts.size() / sizeof(V), off = buf.size();
        buf.resize(off + n * sizeof(V));
        V* dst = (V*)(buf.data() + off);
        for (size_t i = 0; i < n; ++i) {
            V v = src[i];
            float x = (v.x - g.base.x) * sc, z = (v.z - g.base.z) * sc, y = (v.y - g.base.y) * sy;
            v.x = g.base.x + cs * x + sn * z; v.z = g.base.z - sn * x + cs * z; v.y = g.base.y + y - e * 0.25f;
            float nx = v.nx, nz = v.nz;
            v.nx = (int8_t)std::lround(cs * nx + sn * nz); v.nz = (int8_t)std::lround(-sn * nx + cs * nz);
            dst[i] = v;
        }
    }
    if (buf.empty()) return;
    if (!ghostVao_) {
        glGenVertexArrays(1, &ghostVao_); glGenBuffers(1, &ghostVbo_);
        glBindVertexArray(ghostVao_); glBindBuffer(GL_ARRAY_BUFFER, ghostVbo_);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(V), (void*)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V), (void*)16);
    }
    glBindVertexArray(ghostVao_); glBindBuffer(GL_ARRAY_BUFFER, ghostVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf.size(), buf.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(buf.size() / sizeof(V)));
    gfx_.tris += (int)(buf.size() / sizeof(V)) / 3; ++gfx_.draws;
    glBindVertexArray(0);
}

void Renderer::shutdown() {
    for (auto& t : terr_) t.destroy();
    if (gpuTimer_ && qDel) { qDel(3, gpuQ_); gpuQ_[0] = gpuQ_[1] = gpuQ_[2] = 0; }
    gpuTimer_ = false;
    destroyTargets();
    for (auto& ct : charTex_) for (unsigned& t : ct) { if (t) glDeleteTextures(1, &t); t = 0; }
    for (auto& cm : chars_) for (Mesh& m : cm) {
        for (Sub& s : m.subs) { glDeleteBuffers(1, &s.vbo); glDeleteBuffers(1, &s.ibo); glDeleteVertexArrays(1, &s.vao); }
        m.subs.clear();
    }
    for (Mesh& m : toolMeshes_) {
        for (Sub& s : m.subs) { glDeleteBuffers(1, &s.vbo); glDeleteBuffers(1, &s.ibo); glDeleteVertexArrays(1, &s.vao); }
        m.subs.clear();
    }
    for (unsigned& t : thumbTex_) if (t) { glDeleteTextures(1, &t); t = 0; }
    for (StructMesh& M : structMesh_) { if (M.vbo) glDeleteBuffers(1, &M.vbo); if (M.vao) glDeleteVertexArrays(1, &M.vao); M = StructMesh(); }
    for (auto& kv : bakedFlags_) if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    bakedFlags_.clear();
    if (clothProg_) { glDeleteProgram(clothProg_); clothProg_ = 0; }
    for (unsigned* b : {&clothVbo_, &poleVbo_}) if (*b) { glDeleteBuffers(1, b); *b = 0; }
    for (unsigned* a : {&clothVao_, &poleVao_}) if (*a) { glDeleteVertexArrays(1, a); *a = 0; }
    for (unsigned* p : {&groundProg_, &charProg_, &shadowProg_, &skyProg_, &textProg_, &uiProg_, &flagProg_, &gradeProg_, &terrainProg_, &grassProg_, &markerProg_, &gizmoProg_, &splashProg_, &flameProg_, &glowProg_, &propProg_, &shipProg_, &fxProg_})
        if (*p) { glDeleteProgram(*p); *p = 0; }
    static_assert(kShips == e0net::kMaxShips, "um VAO por navio");
    for (int i = 0; i < kShips; ++i) {
        for (unsigned* b : {&shipVbo_[i], &shipIbo_[i], &shipShadowIbo_[i], &wheelVbo_[i]}) if (*b) { glDeleteBuffers(1, b); *b = 0; }
        for (unsigned* v : {&shipVao_[i], &shipShadowVao_[i], &wheelVao_[i]}) if (*v) { glDeleteVertexArrays(1, v); *v = 0; }
        shipCount_[i] = shipShadowCount_[i] = wheelCount_[i] = 0;
        wheelLast_[i] = chestLast_[i] = -999.0f;
    }
    if (texIcons_) { glDeleteTextures(1, &texIcons_); texIcons_ = 0; }
    for (int i = 0; i < 4; ++i) if (texMap_[i]) { glDeleteTextures(1, &texMap_[i]); texMap_[i] = 0; mapRes_[i] = 0; }
    if (texTerrainAlbedo_) { glDeleteTextures(1, &texTerrainAlbedo_); texTerrainAlbedo_ = 0; }
    if (texTerrainNormal_) { glDeleteTextures(1, &texTerrainNormal_); texTerrainNormal_ = 0; }
    if (groundVbo_) glDeleteBuffers(1, &groundVbo_);
    if (groundVao_) glDeleteVertexArrays(1, &groundVao_);
    if (markerVbo_) glDeleteBuffers(1, &markerVbo_);
    if (markerVao_) glDeleteVertexArrays(1, &markerVao_);
    markerVbo_ = markerVao_ = 0;
    if (gizmoVbo_) glDeleteBuffers(1, &gizmoVbo_);
    if (gizmoVao_) glDeleteVertexArrays(1, &gizmoVao_);
    gizmoVbo_ = gizmoVao_ = 0;
    if (splashVbo_) glDeleteBuffers(1, &splashVbo_);
    if (splashVao_) glDeleteVertexArrays(1, &splashVao_);
    splashVbo_ = splashVao_ = 0;
    if (uiVbo_) glDeleteBuffers(1, &uiVbo_);
    if (uiVao_) glDeleteVertexArrays(1, &uiVao_);
    if (textVbo_) glDeleteBuffers(1, &textVbo_);
    if (textVao_) glDeleteVertexArrays(1, &textVao_);
    if (fontTex_) glDeleteTextures(1, &fontTex_);
    if (uiFontTex_) glDeleteTextures(1, &uiFontTex_);
    groundVbo_ = groundVao_ = uiVbo_ = uiVao_ = textVbo_ = textVao_ = fontTex_ = uiFontTex_ = 0;
    ready_ = false;
}

void Renderer::resize(int w, int h) {
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
    glViewport(0, 0, w_, h_);
    if (ready_) createTargets();  // o reflexo depende do tamanho da tela
}

void Renderer::setQuality(int q) {
    q = std::max(0, std::min(2, q));
    if (q == quality_) return;
    quality_ = q;
    if (ready_) createTargets();
    LOG("qualidade gráfica: %s", qualityName(q));
}

// ---------------------------------------------------------------------------------------------
// Alvos de render (sombra)
// ---------------------------------------------------------------------------------------------
void Renderer::destroyTargets() {
    if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
    if (shadowTex_) glDeleteTextures(1, &shadowTex_);
    shadowFbo_ = shadowTex_ = 0;
    shadowRes_ = 0;
}

void Renderer::setCinema(bool on) {
    if (cine_ == on) return;
    cine_ = on;
    if (ready_) createTargets();
}

void Renderer::createTargets() {
    destroyTargets();
    const QParams& Q = cine_ ? kQCine : kQ[quality_];

    // mapa de sombra: textura de profundidade com comparação (PCF 2x2 por hardware via filtro linear)
    shadowRes_ = Q.shadowRes;
    glGenTextures(1, &shadowTex_);
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, shadowRes_, shadowRes_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &shadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG("FBO de sombra incompleto: sombras desligadas");
        glDeleteFramebuffers(1, &shadowFbo_); glDeleteTextures(1, &shadowTex_); shadowFbo_ = shadowTex_ = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOG("alvos de render (%s): sombra %d%s", qualityName(quality_), shadowRes_, shadowFbo_ ? "" : " (OFF)");
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void Renderer::applyLit(const Lit& L, const Vec3& cam) {
    const Environment& E = env_;
    glUniform3f(L.cam, cam.x, cam.y, cam.z);
    glUniform3f(L.ambient, E.ambient.x, E.ambient.y, E.ambient.z);
    glUniform3f(L.lightDir, E.lightDir.x, E.lightDir.y, E.lightDir.z);
    glUniform3f(L.lightCol, E.lightCol.x, E.lightCol.y, E.lightCol.z);
    glUniform3f(L.horizon, E.horizon.x, E.horizon.y, E.horizon.z);
    glUniform3f(L.zenith, E.zenith.x, E.zenith.y, E.zenith.z);
    glUniform3f(L.sunDir, E.sunDir.x, E.sunDir.y, E.sunDir.z);
    glUniform1f(L.night, E.night);
    glUniform1f(L.shadowOn, shadowOn_);
    glUniform1f(L.shadowTexel, shadowTexel_);
    glUniform1f(L.shadowRadius, shadowRadius_);
    glUniform1f(L.pcf, pcf_);
    glUniform3f(L.shadowCenter, shadowCenter_.x, shadowCenter_.y, shadowCenter_.z);
    glUniform2f(L.shadowInv, shadowRes_ ? 1.0f / shadowRes_ : 0.0f, shadowRes_ ? 1.0f / shadowRes_ : 0.0f);
    glUniformMatrix4fv(L.shadowMat, 1, GL_FALSE, shadowMat_.m);
    glUniform1i(L.shadow, 1);  // unidade de textura 1
    glUniform4fv(L.torch, 4, &torch_[0].x);
    glUniform1f(L.fogK, fogK_);
}

void Renderer::drawSky(const Camera& cam, float aspect) {
    Vec3 f = cam.forward(), rt = cam.rightVec(), up = cam.upVec();  // base da câmera já com roll (tremor)
    float ty = std::tan(cam.fovY * 0.5f);
    // Desenhado por último entre os opacos: z = 1 (far) no VS + LEQUAL => só os pixels sem geometria rodam o shader do céu.
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glUseProgram(skyProg_);
    applyLit(litSky_, cam.position);
    glUniform3f(uSkyFwd_, f.x, f.y, f.z);
    glUniform3f(uSkyRight_, rt.x, rt.y, rt.z);
    glUniform3f(uSkyUp_, up.x, up.y, up.z);
    glUniform2f(uSkyTan_, ty * aspect, ty);
    glBindVertexArray(uiVao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ++gfx_.draws;
    glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
}

void Renderer::drawMesh(int lod, const Mat4& mvp, const Mat4& model, const Vec3& body, const Vec4* boneRows, bool depthOnly) {
    const int cm = pendSkin_ ? std::max(0, std::min(kCharModels - 1, pendSkin_->model)) : 0;   // modelo da skin (useSkin antes; sem skin = modelo 0)
    if (chars_[cm][0].subs.empty()) { pendSkin_ = nullptr; return; }
    while (lod > 0 && chars_[cm][lod].subs.empty()) --lod;  // LOD ausente: usa o mais detalhado disponível
    drawMeshRef(chars_[cm][lod], mvp, model, body, boneRows, depthOnly);
    pendSkin_ = nullptr;
}

unsigned Renderer::loadTexture(const char* name) {
    if (!assets_) return 0;
    AAsset* a = AAssetManager_open(assets_, name, AASSET_MODE_BUFFER);
    if (!a) { LOG("textura %s não encontrada", name); return 0; }
    const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a); const size_t len = (size_t)AAsset_getLength(a);
    int w = 0, h = 0;
    unsigned tex = uploadE0I(b, len, &w, &h, true);   // E0I1 (RGBA8) ou E0I2 (ETC2 com mipmaps)
    if (tex) LOG("textura %s carregada: %dx%d (%s)", name, w, h, std::memcmp(b, "E0I2", 4) == 0 ? "ETC2" : "RGBA8");
    AAsset_close(a);
    return tex;
}

// Ferramenta na mão: malha já no espaço de bind com peso 100% no osso da mão direita -> usa os mesmos uBones do personagem.
// A submalha "corpo" (a de mais vértices = cabeça de metal) recebe a cor de metal; o cabo usa a cor de madeira própria.
// Caixa unitária (x,z em [-0.5,0.5], y em [0,1]) no formato E0M2 (osso 0, peso 255): com bones de identidade fica estática.
void Renderer::makeCube() {
    struct V { float p[3], n[3]; uint8_t b[4], w[4]; };
    static const float kN[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    std::vector<V> vs; std::vector<uint32_t> idx;
    for (int f = 0; f < 6; ++f) {
        const float* n = kN[f];
        float ux = n[1] != 0 ? 1 : 0, uy = 0, uz = n[1] != 0 ? 0 : (n[0] != 0 ? 0 : 1);
        float t1[3] = {0, 0, 0}, t2[3] = {0, 0, 0};
        if (n[0] != 0) { t1[2] = 1; t2[1] = 1; } else if (n[1] != 0) { t1[0] = 1; t2[2] = 1; } else { t1[0] = 1; t2[1] = 1; }
        (void)ux; (void)uy; (void)uz;
        uint32_t base = (uint32_t)vs.size();
        for (int k = 0; k < 4; ++k) {
            float a = (k == 0 || k == 3) ? -0.5f : 0.5f, b = (k < 2) ? -0.5f : 0.5f;
            V v{};
            for (int d = 0; d < 3; ++d) v.p[d] = n[d] * 0.5f + t1[d] * a + t2[d] * b;
            v.p[1] += 0.5f;
            for (int d = 0; d < 3; ++d) v.n[d] = n[d];
            v.b[0] = 0; v.w[0] = 255;
            vs.push_back(v);
        }
        // winding: garante face para fora (testa o produto vetorial com a normal)
        float e1[3], e2[3];
        for (int d = 0; d < 3; ++d) { e1[d] = vs[base + 1].p[d] - vs[base].p[d]; e2[d] = vs[base + 2].p[d] - vs[base].p[d]; }
        float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        bool out = cx * n[0] + cy * n[1] + cz * n[2] > 0;
        const uint32_t a[6] = {0, 1, 2, 0, 2, 3}, bq[6] = {0, 2, 1, 0, 3, 2};
        for (int k = 0; k < 6; ++k) idx.push_back(base + (out ? a[k] : bq[k]));
    }
    Sub m; m.count = (int)idx.size(); m.color = {1, 1, 1}; m.body = true;
    glGenVertexArrays(1, &m.vao); glGenBuffers(1, &m.vbo); glGenBuffers(1, &m.ibo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vs.size() * sizeof(V)), vs.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribIPointer(2, 4, GL_UNSIGNED_BYTE, 32, (void*)24);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 32, (void*)28);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    cubeMesh_.subs.clear();
    cubeMesh_.subs.push_back(m);
}

// ---- Navio pirata ----
bool Renderer::loadShip(int i, const char* file) {
    if (!assets_) return false;
    AAsset* a = AAssetManager_open(assets_, file, AASSET_MODE_BUFFER);
    if (!a) return false;
    const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a);
    size_t len = (size_t)AAsset_getLength(a);
    uint32_t nv = 0, ni = 0;
    bool ok = len >= 12 && std::memcmp(b, "E0S1", 4) == 0;
    if (ok) { std::memcpy(&nv, b + 4, 4); std::memcpy(&ni, b + 8, 4); ok = 12 + (size_t)nv * 20 + (size_t)ni * 4 <= len; }
    if (ok) {
        glGenVertexArrays(1, &shipVao_[i]); glGenBuffers(1, &shipVbo_[i]); glGenBuffers(1, &shipIbo_[i]);
        glBindVertexArray(shipVao_[i]);
        glBindBuffer(GL_ARRAY_BUFFER, shipVbo_[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * 20, b + 12, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, 20, (void*)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, (void*)16);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shipIbo_[i]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ni * 4, b + 12 + (size_t)nv * 20, GL_STATIC_DRAW);
        glBindVertexArray(0);
        shipCount_[i] = (int)ni;

        // Índice otimizado para o passe de sombra: descarta triângulos do casco submerso (y < -0.15m) e micro-triângulos sub-texel
        const PropVertex* verts = (const PropVertex*)(b + 12);
        const uint32_t* indices = (const uint32_t*)(b + 12 + (size_t)nv * 20);
        std::vector<uint32_t> shIdx;
        shIdx.reserve(ni);
        for (uint32_t k = 0; k + 2 < ni; k += 3) {
            uint32_t i0 = indices[k], i1 = indices[k + 1], i2 = indices[k + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            const PropVertex &v0 = verts[i0], &v1 = verts[i1], &v2 = verts[i2];
            if (v0.y < -0.15f && v1.y < -0.15f && v2.y < -0.15f) continue;
            float ex1 = v1.x - v0.x, ey1 = v1.y - v0.y, ez1 = v1.z - v0.z;
            float ex2 = v2.x - v0.x, ey2 = v2.y - v0.y, ez2 = v2.z - v0.z;
            float cx = ey1 * ez2 - ez1 * ey2, cy = ez1 * ex2 - ex1 * ez2, cz = ex1 * ey2 - ey1 * ex2;
            if (cx * cx + cy * cy + cz * cz < 1e-5f) continue;
            shIdx.push_back(i0); shIdx.push_back(i1); shIdx.push_back(i2);
        }
        glGenVertexArrays(1, &shipShadowVao_[i]); glGenBuffers(1, &shipShadowIbo_[i]);
        glBindVertexArray(shipShadowVao_[i]);
        glBindBuffer(GL_ARRAY_BUFFER, shipVbo_[i]);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shipShadowIbo_[i]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(shIdx.size() * 4), shIdx.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        shipShadowCount_[i] = (int)shIdx.size();
        wheelLast_[i] = chestLast_[i] = -999.0f;
        LOG("%s: %u vertices, %u triangulos (sombra: %zu tris)", file, nv, ni / 3, shIdx.size() / 3);
    }
    AAsset_close(a);
    return ok;
}

// Caixa orientada para a geometria do navio gerada a cada frame: c = centro; ax/ay/az = meios-eixos
static void addBox(std::vector<PropVertex>& out, Vec3 c, Vec3 ax, Vec3 ay, Vec3 az, uint8_t r, uint8_t g, uint8_t b) {
    static const int F[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};   // +X, -X, +Y, -Y, +Z, -Z
    Vec3 n6[6] = {ax, ax * -1.0f, ay, ay * -1.0f, az, az * -1.0f};
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i) corner[i] = c + ax * ((i & 1) ? 1.0f : -1.0f) + ay * ((i & 2) ? 1.0f : -1.0f) + az * ((i & 4) ? 1.0f : -1.0f);
    for (int f = 0; f < 6; ++f) {
        Vec3 n = normalize(n6[f]);
        PropVertex v[4];
        for (int k = 0; k < 4; ++k) {
            const Vec3& p = corner[F[f][k]];
            v[k] = {p.x, p.y, p.z, (int8_t)std::lround(n.x * 127), (int8_t)std::lround(n.y * 127), (int8_t)std::lround(n.z * 127), 0, r, g, b, 255};
        }
        out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]);
        out.push_back(v[0]); out.push_back(v[2]); out.push_back(v[3]);
    }
}

// Baú pirata (espaço do navio: proa = -Z): corpo, cintas de ferro, fecho dourado e tampa abaulada que gira na dobradiça de trás. open: 0 fechado, 1 aberto.
static void buildChest(std::vector<PropVertex>& out, const e0net::ShipDef& D, float open) {
    const float hx = D.chestHalfB, hz = D.chestHalfA, cx = D.chestB, cz = -D.chestA, y0 = D.deckY, bh = 0.40f * hx / e0net::kChestHalfB;
    auto box = [&](Vec3 c, Vec3 ax, Vec3 ay, Vec3 az, uint8_t r, uint8_t g, uint8_t b) { addBox(out, c, ax, ay, az, r, g, b); };
    const Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    const float wall = 0.035f, yc = y0 + bh * 0.5f;
    box({cx, y0 + 0.03f, cz}, X * hx, Y * 0.03f, Z * hz, 62, 38, 20);                                    // fundo
    box({cx, yc, cz - hz + wall}, X * hx, Y * (bh * 0.5f), Z * wall, 116, 72, 38);                       // frente (para a proa)
    box({cx, yc, cz + hz - wall}, X * hx, Y * (bh * 0.5f), Z * wall, 116, 72, 38);                       // trás
    for (float s : {-1.0f, 1.0f}) box({cx + s * (hx - wall), yc, cz}, X * wall, Y * (bh * 0.5f), Z * hz, 104, 64, 34);   // lados
    for (float s : {-1.0f, 1.0f}) {                                                                      // cintas de ferro
        box({cx + s * hx * 0.62f, yc, cz - hz - 0.008f}, X * 0.045f, Y * (bh * 0.5f + 0.004f), Z * 0.012f, 48, 48, 54);
        box({cx + s * hx * 0.62f, yc, cz + hz + 0.008f}, X * 0.045f, Y * (bh * 0.5f + 0.004f), Z * 0.012f, 48, 48, 54);
        box({cx + s * (hx + 0.008f), yc, cz}, X * 0.012f, Y * (bh * 0.5f + 0.004f), Z * 0.045f, 48, 48, 54);
    }
    box({cx, y0 + bh - 0.05f, cz - hz - 0.016f}, X * 0.06f, Y * 0.065f, Z * 0.016f, 214, 168, 62);       // fecho

    // Tampa: arco de `seg` tábuas no plano (y, z) em torno da dobradiça, girado por `ang`; (d = para a frente a partir da dobradiça, h = altura)
    const float ang = open * 1.45f, ca = std::cos(ang), sa = std::sin(ang), hy = y0 + bh, hzp = cz + hz, arch = 0.20f * hx / e0net::kChestHalfB;
    auto rot = [&](float d, float h) { return Vec3{0.0f, h * ca + d * sa, -d * ca + h * sa}; };          // ponto (d, h) -> deslocamento da dobradiça
    auto at = [&](float x, float d, float h) { Vec3 q = rot(d, h); return Vec3{x, hy + q.y, hzp + q.z}; };
    const int seg = 6;
    auto arc = [&](int i, float& d, float& h) { float t = 3.14159265f * (float)i / seg; d = hz - hz * std::cos(t); h = arch * std::sin(t); };
    for (int i = 0; i < seg; ++i) {
        float d0, h0, d1, h1; arc(i, d0, h0); arc(i + 1, d1, h1);
        float dm = 0.5f * (d0 + d1), hm = 0.5f * (h0 + h1), ld = d1 - d0, lh = h1 - h0, l = std::sqrt(ld * ld + lh * lh);
        Vec3 tan = rot(ld * 0.5f, lh * 0.5f), nor = rot(lh / l, -ld / l) * -1.0f;                         // tangente e normal para fora do arco
        Vec3 c = at(cx, dm, hm);
        uint8_t k = (i & 1) ? 0 : 12;
        box(c, X * hx, nor * 0.016f, tan, (uint8_t)(128 + k), (uint8_t)(80 + k), (uint8_t)(42 + k / 2));
        for (float s : {-1.0f, 1.0f}) box(c + X * (s * hx * 0.62f) + nor * 0.012f, X * 0.045f, nor * 0.012f, tan, 48, 48, 54);
    }
    for (float s : {-1.0f, 1.0f}) {                                                                      // tampos do arco (leque a partir do meio da base)
        const float x = cx + s * hx;
        Vec3 nn = X * s;
        auto vert = [&](Vec3 p) { return PropVertex{p.x, p.y, p.z, (int8_t)std::lround(nn.x * 127), 0, 0, 0, 104, 64, 34, 255}; };
        for (int i = 0; i < seg; ++i) {
            float d0, h0, d1, h1; arc(i, d0, h0); arc(i + 1, d1, h1);
            out.push_back(vert(at(x, hz, 0.0f))); out.push_back(vert(at(x, d0, h0))); out.push_back(vert(at(x, d1, h1)));
        }
    }
    box(at(cx, 2.0f * hz, 0.03f), X * 0.06f, rot(0.0f, 0.05f), rot(0.016f, 0.0f), 214, 168, 62);          // lingueta do fecho (vai com a tampa)
}

// Timão do leme (geometria pequena gerada a cada frame no espaço do navio): pedestal + aro + 8 raios que giram com o leme.
static void buildWheel(std::vector<PropVertex>& out, const e0net::ShipDef& D, float angle) {
    auto box = [&](Vec3 c, Vec3 ax, Vec3 ay, Vec3 az, uint8_t r, uint8_t g, uint8_t b) { addBox(out, c, ax, ay, az, r, g, b); };
    const float k = D.wheelScale, cx = -0.05f, cy = D.wheelY, cz = -D.wheelA;   // timão em cima do toldo da popa (proa = -Z); a roda original do modelo é removida em export_ship.py
    box({cx, cy, cz + 0.02f * k}, {0.05f * k, 0, 0}, {0, 0.05f * k, 0}, {0, 0, 0.09f * k}, 70, 44, 24);                               // cubo
    const int seg = 16; const float R = 0.56f * k;
    for (int i = 0; i < seg; ++i) {   // aro: segmentos retos
        float a0 = angle + (float)i * 6.2831853f / seg, a1 = a0 + 6.2831853f / seg, am = 0.5f * (a0 + a1);
        Vec3 c = {cx + std::cos(am) * R, cy + std::sin(am) * R, cz};
        Vec3 along = {-std::sin(am), std::cos(am), 0.0f};
        box(c, along * (R * 3.1416f / seg * 1.04f), {std::cos(am) * 0.035f * k, std::sin(am) * 0.035f * k, 0}, {0, 0, 0.035f * k}, 120, 78, 40);
    }
    for (int i = 0; i < 8; ++i) {     // raios + cabos de manobra
        float a0 = angle + (float)i * 6.2831853f / 8.0f;
        Vec3 d = {std::cos(a0), std::sin(a0), 0.0f};
        box({cx + d.x * R * 0.5f, cy + d.y * R * 0.5f, cz}, d * (R * 0.5f), {-d.y * 0.025f * k, d.x * 0.025f * k, 0}, {0, 0, 0.025f * k}, 110, 70, 36);
        box({cx + d.x * (R + 0.09f * k), cy + d.y * (R + 0.09f * k), cz}, d * (0.1f * k), {-d.y * 0.04f * k, d.x * 0.04f * k, 0}, {0, 0, 0.04f * k}, 130, 86, 44);
    }
}

// Matriz do casco: pose no mar + naufrágio (desce, aderna e afunda de popa: shipSinkPose)
Mat4 Renderer::shipModel(int i) const {
    const ShipDraw& ship = ships_[i];
    Mat4 m = Mat4::translate({ship.x, 0.0f, ship.z}) * Mat4::rotY(-ship.yaw);
    if (ship.sinkT > 0.0f) {
        float dy, pitch, roll; e0net::shipSinkPose(e0net::shipDef(i), ship.sinkT, dy, pitch, roll);
        m = Mat4::translate({ship.x, dy, ship.z}) * Mat4::rotY(-ship.yaw) * Mat4::rotX(pitch) * Mat4::rotZ(roll);
    }
    return m;
}

void Renderer::drawShip(int i, const Camera& cam, const Mat4& vp, float nowSec, float clipY) {
    const ShipDraw& ship = ships_[i];
    if (!ship.exists || !shipVao_[i] || !shipCount_[i]) return;
    const e0net::ShipDef& D = e0net::shipDef(i);
    {   // o navio é a malha mais pesada do jogo: frustum culling completo de 6 planos + distância máxima
        Vec3 d{ship.x - cam.position.x, 6.0f - cam.position.y, ship.z - cam.position.z};
        float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), R = 36.0f * D.wakeA;
        if (dist > (cine_ ? 1800.0f : 620.0f)) return;
        if (dist > R) {
            const float* m = vp.m;
            const float cx = ship.x, cy = 6.0f, cz = ship.z;
            auto outPlane = [&](float a, float b, float c, float w) {
                float len = std::sqrt(a * a + b * b + c * c);
                return (a * cx + b * cy + c * cz + w) < -R * len;
            };
            if (outPlane(m[3] + m[0], m[7] + m[4], m[11] + m[8],  m[15] + m[12]) ||
                outPlane(m[3] - m[0], m[7] - m[4], m[11] - m[8],  m[15] - m[12]) ||
                outPlane(m[3] + m[1], m[7] + m[5], m[11] + m[9],  m[15] + m[13]) ||
                outPlane(m[3] - m[1], m[7] - m[5], m[11] - m[9],  m[15] - m[13]) ||
                outPlane(m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14])) return;
        }
    }
    const Mat4 model = shipModel(i);
    glUseProgram(shipProg_);
    applyLit(litShip_, cam.position);
    glUniformMatrix4fv(uShipVP_, 1, GL_FALSE, vp.m);
    glUniformMatrix4fv(uShipModel_, 1, GL_FALSE, model.m);
    glUniform1f(uShipClip_, clipY);   // o casco inteiro; o mar (translúcido, desenhado depois) tinge a parte submersa
    glUniform1f(uShipTime_, nowSec);
    glDisable(GL_CULL_FACE);          // velas e bandeira são planos finos
    glBindVertexArray(shipVao_[i]);
    glDrawElements(GL_TRIANGLES, shipCount_[i], GL_UNSIGNED_INT, nullptr);
    gfx_.tris += shipCount_[i] / 3; ++gfx_.draws;
    if (!wheelVao_[i] || std::fabs(ship.wheel - wheelLast_[i]) > 1e-3f || std::fabs(ship.chestOpen - chestLast_[i]) > 1e-3f) {
        static std::vector<PropVertex> wv;
        wv.clear();
        buildWheel(wv, D, ship.wheel);
        buildChest(wv, D, ship.chestOpen);
        if (!wheelVao_[i]) {
            glGenVertexArrays(1, &wheelVao_[i]); glGenBuffers(1, &wheelVbo_[i]);
            glBindVertexArray(wheelVao_[i]); glBindBuffer(GL_ARRAY_BUFFER, wheelVbo_[i]);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PropVertex), (void*)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(PropVertex), (void*)12);
            glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PropVertex), (void*)16);
        }
        glBindVertexArray(wheelVao_[i]); glBindBuffer(GL_ARRAY_BUFFER, wheelVbo_[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(wv.size() * sizeof(PropVertex)), wv.data(), GL_DYNAMIC_DRAW);
        wheelCount_[i] = (int)wv.size();
        wheelLast_[i] = ship.wheel;
        chestLast_[i] = ship.chestOpen;
    } else {
        glBindVertexArray(wheelVao_[i]);
    }
    if (wheelCount_[i] > 0) {
        glDrawArrays(GL_TRIANGLES, 0, wheelCount_[i]);
        gfx_.tris += wheelCount_[i] / 3; ++gfx_.draws;
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}


// ---- MASTROS DE DOMINAÇÃO (shared/conquest.h) ----
void Renderer::bakeIslandFlags() {
    if (!flagProg_ || isleFlags_.empty()) return;
    bool any = false;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const IslandFlagDraw& f : isleFlags_) {
        if (!f.cloth) continue;
        auto it = bakedFlags_.find(f.key);
        if (it != bakedFlags_.end()) { it->second.used = t; continue; }
        if (bakedFlags_.size() >= 48) {   // tira a menos usada
            auto old = bakedFlags_.begin();
            for (auto j = bakedFlags_.begin(); j != bakedFlags_.end(); ++j) if (j->second.used < old->second.used) old = j;
            glDeleteTextures(1, &old->second.tex); bakedFlags_.erase(old);
        }
        const int W = 240, H = 160;
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, W, H);
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
            const int sw = w_, sh = h_; w_ = W; h_ = H;
            glBindVertexArray(uiVao_);
            uiFlag(f.look, W * 0.5f, H * 0.5f, W * 0.5f - 2.0f, H * 0.5f / 1.08f, 0.0f, 0.0f, 1.0f);   // parada: o 3D faz a onda
            glBindVertexArray(0);
            w_ = sw; h_ = sh;
            any = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glBindTexture(GL_TEXTURE_2D, tex); glGenerateMipmap(GL_TEXTURE_2D);
        bakedFlags_[f.key] = {tex, t};
    }
    if (any) { glViewport(0, 0, w_, h_); glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glUseProgram(0); }
}

static void poleCyl(std::vector<PropVertex>& v, Vec3 c, float r0, float r1, float y0, float y1, int n, uint8_t R, uint8_t G, uint8_t B) {
    for (int i = 0; i < n; ++i) {
        const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        auto V = [&](float cs, float sn, float r, float y) { PropVertex p; p.x = c.x + cs * r; p.y = c.y + y; p.z = c.z + sn * r; p.nx = (int8_t)(cs * 127); p.ny = 0; p.nz = (int8_t)(sn * 127); p.nw = 0; p.r = R; p.g = G; p.b = B; p.a = 255; return p; };
        PropVertex a = V(c0, s0, r0, y0), b = V(c1, s1, r0, y0), cc = V(c1, s1, r1, y1), d = V(c0, s0, r1, y1);
        v.push_back(a); v.push_back(cc); v.push_back(b); v.push_back(a); v.push_back(d); v.push_back(cc);
        PropVertex t0 = V(c0, s0, r1, y1), t1 = V(c1, s1, r1, y1), tc = V(0, 0, 0, y1);   // tampa de cima
        for (PropVertex* p : {&t0, &t1, &tc}) { p->nx = 0; p->ny = 127; p->nz = 0; }
        v.push_back(tc); v.push_back(t1); v.push_back(t0);
    }
}

void Renderer::drawIslandFlags(const Camera& cam, const Mat4& vp, float nowSec) {
    if (isleFlags_.empty()) return;
    const float kPoleH = 7.6f, kClothL = 2.9f, kClothH = 1.95f;
    // poste (madeira escura), base de pedra e remate dourado: malha de cores de vértice no programa dos props
    static std::vector<PropVertex> pv; pv.clear();
    for (const IslandFlagDraw& f : isleFlags_) {
        const float dx = f.base.x - cam.position.x, dz = f.base.z - cam.position.z;
        if (dx * dx + dz * dz > 700.0f * 700.0f) continue;
        poleCyl(pv, {f.base.x, f.base.y - 0.3f, f.base.z}, 1.15f, 0.95f, 0.0f, 0.75f, 8, 104, 100, 92);     // base de pedra
        poleCyl(pv, {f.base.x, f.base.y - 0.3f, f.base.z}, 0.80f, 0.60f, 0.75f, 1.05f, 8, 92, 88, 80);
        poleCyl(pv, f.base, 0.11f, 0.075f, 0.6f, kPoleH, 7, 86, 58, 34);                                   // mastro
        poleCyl(pv, f.base, 0.16f, 0.16f, kPoleH, kPoleH + 0.10f, 8, 222, 172, 60);                          // remate dourado
        poleCyl(pv, f.base, 0.16f, 0.02f, kPoleH + 0.10f, kPoleH + 0.42f, 8, 232, 182, 66);
    }
    if (!pv.empty()) {
        if (!poleVao_) {
            glGenVertexArrays(1, &poleVao_); glGenBuffers(1, &poleVbo_);
            glBindVertexArray(poleVao_); glBindBuffer(GL_ARRAY_BUFFER, poleVbo_);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PropVertex), (void*)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(PropVertex), (void*)12);
            glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PropVertex), (void*)16);
        }
        glBindVertexArray(poleVao_); glBindBuffer(GL_ARRAY_BUFFER, poleVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pv.size() * sizeof(PropVertex)), pv.data(), GL_DYNAMIC_DRAW);
        glUseProgram(propProg_);
        applyLit(litProp_, cam.position);
        glUniformMatrix4fv(uPropVP_, 1, GL_FALSE, vp.m);
        glUniform1f(uPropClip_, -2.35f);
        glUniform1f(uPropTime_, nowSec);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)pv.size());
        gfx_.tris += (int)pv.size() / 3; ++gfx_.draws;
        glBindVertexArray(0);
    }
    if (!clothProg_) return;
    if (!clothVao_) {   // grade 14 x 6 (s, v) em tiras de triângulos
        std::vector<float> g;
        const int NS = 14, NV = 6;
        for (int j = 0; j < NV; ++j) for (int i = 0; i < NS; ++i) {
            const float s0 = (float)i / NS, s1 = (float)(i + 1) / NS, v0 = (float)j / NV, v1 = (float)(j + 1) / NV;
            const float q[12] = {s0, v0, s1, v0, s1, v1, s0, v0, s1, v1, s0, v1};
            g.insert(g.end(), q, q + 12);
        }
        clothCount_ = (int)g.size() / 2;
        glGenVertexArrays(1, &clothVao_); glGenBuffers(1, &clothVbo_);
        glBindVertexArray(clothVao_); glBindBuffer(GL_ARRAY_BUFFER, clothVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g.size() * sizeof(float)), g.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
    }
    glUseProgram(clothProg_);
    applyLit(litCloth_, cam.position);
    glUniformMatrix4fv(uClothVP_, 1, GL_FALSE, vp.m);
    glUniform1i(uClothTex_, 2);
    const float wa = 0.7f + 0.35f * std::sin(nowSec * 0.05f);   // o vento muda devagar (o mesmo em todas as ilhas)
    const Vec3 dir{std::cos(wa), 0.0f, std::sin(wa)};
    glUniform3f(uClothDir_, dir.x, dir.y, dir.z);
    glUniform2f(uClothSize_, kClothL, kClothH);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(clothVao_);
    glActiveTexture(GL_TEXTURE2);
    for (const IslandFlagDraw& f : isleFlags_) {
        if (!f.cloth) continue;
        const float dx = f.base.x - cam.position.x, dz = f.base.z - cam.position.z;
        if (dx * dx + dz * dz > 700.0f * 700.0f) continue;
        auto it = bakedFlags_.find(f.key);
        if (it == bakedFlags_.end()) continue;
        glBindTexture(GL_TEXTURE_2D, it->second.tex);
        const float y = f.base.y + 1.15f + std::max(0.0f, std::min(1.0f, f.prog)) * (kPoleH - 1.15f - kClothH - 0.12f);
        glUniform3f(uClothBase_, f.base.x + dir.x * 0.09f, y, f.base.z + dir.z * 0.09f);
        glUniform1f(uClothTime_, nowSec + f.seed);
        glDrawArrays(GL_TRIANGLES, 0, clothCount_);
        gfx_.tris += clothCount_ / 3; ++gfx_.draws;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void Renderer::drawShipShadow(int i, const Mat4& lvp) {
    const ShipDraw& ship = ships_[i];
    const unsigned vao = shipShadowVao_[i] ? shipShadowVao_[i] : shipVao_[i];
    const int cnt = shipShadowVao_[i] ? shipShadowCount_[i] : shipCount_[i];
    if (!ship.exists || !vao || !cnt) return;
    const Mat4 model = shipModel(i);
    glUniform1i(uShadowSkinned_, 0);
    glUniformMatrix4fv(uShadowMVP_, 1, GL_FALSE, (lvp * model).m);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, cnt, GL_UNSIGNED_INT, nullptr);
    if (wheelVao_[i] && wheelCount_[i]) {   // timão e baú (buffer já em cache)
        glBindVertexArray(wheelVao_[i]);
        glDrawArrays(GL_TRIANGLES, 0, wheelCount_[i]);
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    gfx_.tris += cnt / 3; ++gfx_.draws;
}

// Itens soltos que usam malhas de props_data.h (graveto, pedrinha, pedra, tora): vértices girados/escalados/movidos na CPU.
void Renderer::drawDropProps() {
    static std::vector<PropVertex> scratch;
    scratch.clear();
    for (const DropVis& d : drops_) {
        const PropModel* m = nullptr; float sc = 1.0f;
        switch (d.model) {
            case 1: m = &kPropStick; break;
            case 2: m = &kPropPebbles[d.seed % 3]; sc = d.scale; break;
            case 3: m = &kPropRocks[2]; sc = 0.30f; break;
            case 4: m = &kPropLog; sc = 0.20f; break;
            case 7: m = &kPropCoconutItem; break;
            case 8: m = &kPropFrondItem; break;
            default: break;
        }
        if (!m) continue;
        const float cs = std::cos(d.yaw), sn = std::sin(d.yaw);
        for (size_t i = 0; i < m->count; ++i) {
            PropVertex v = m->vertices[i];
            float x = v.x * sc, y = v.y * sc, z = v.z * sc;
            if (d.shine) {   // pepita/diamante: o modelo da pedrinha pintado com a cor do item (mantém o sombreado) + brilho no shader
                float lum = (v.r + v.g + v.b) * (1.0f / 765.0f) * 1.5f + 0.25f;
                v.r = (uint8_t)std::min(255.0f, d.color.x * lum * 255.0f); v.g = (uint8_t)std::min(255.0f, d.color.y * lum * 255.0f); v.b = (uint8_t)std::min(255.0f, d.color.z * lum * 255.0f);
                v.a = d.shine == 2 ? 240 : 252;
            }
            v.x = d.pos.x + cs * x + sn * z; v.y = d.pos.y + y; v.z = d.pos.z - sn * x + cs * z;
            float nx = v.nx, nz = v.nz;
            v.nx = (int8_t)std::lround(cs * nx + sn * nz); v.nz = (int8_t)std::lround(-sn * nx + cs * nz);
            scratch.push_back(v);
        }
    }
    if (scratch.empty()) return;
    if (!dropVao_) {
        glGenVertexArrays(1, &dropVao_); glGenBuffers(1, &dropVbo_);
        glBindVertexArray(dropVao_); glBindBuffer(GL_ARRAY_BUFFER, dropVbo_);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PropVertex), (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(PropVertex), (void*)12);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PropVertex), (void*)16);
    }
    glBindVertexArray(dropVao_); glBindBuffer(GL_ARRAY_BUFFER, dropVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scratch.size() * sizeof(PropVertex)), scratch.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)scratch.size());
    gfx_.tris += (int)scratch.size() / 3; ++gfx_.draws;
    glBindVertexArray(0);
}

static Mat4 charModel(const CharInst& c) {   // translate(pos) * rotY(-yaw) [* local: corpo caído carregado]
    Mat4 m = Mat4::translate(c.pos) * Mat4::rotY(-c.yaw);
    return c.hasLocal ? m * c.local : m;
}

// Ferramentas GUARDADAS no corpo: machadinha e picareta em X nas costas (osso Spine2 = 3), tocha na cintura esquerda (osso Hips = 0);
// futuramente a faca na cintura direita (CarryKnife). As malhas estão no espaço de bind da mão (osso 34, peso 255), então para pendurá-las
// em outro osso b troca-se a matriz de skinning do osso 34 por  S_b * A * inv(HG):  A = onde o quadro da ferramenta fica no corpo em
// repouso (espaço de bind), HG = quadro da ferramenta na pegada (kHandGrip). Quadro da ferramenta: Y = cabo->cabeça, Z = gume.
namespace {
struct M4 { float m[16]; };   // linha a linha
M4 mul(const M4& a, const M4& b) { M4 r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float s = 0; for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j]; r.m[i * 4 + j] = s; } return r; }
M4 rigidInverse(const M4& a) {
    M4 r = {};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = a.m[j * 4 + i];
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = -(r.m[i * 4] * a.m[3] + r.m[i * 4 + 1] * a.m[7] + r.m[i * 4 + 2] * a.m[11]);
    r.m[15] = 1; return r;
}
// quadro com origem p, eixo Y = u (cabo->cabeça) e Z = componente de `eHint` perpendicular a u (gume); X = Y x Z
M4 toolFrame(Vec3 p, Vec3 u, Vec3 eHint) {
    u = normalize(u); Vec3 e = normalize(eHint - u * dot(eHint, u)); Vec3 x = cross(u, e);
    return M4{{x.x, u.x, e.x, p.x, x.y, u.y, e.y, p.y, x.z, u.z, e.z, p.z, 0, 0, 0, 1}};
}
Mat4 matFromRows(const Vec4& r0, const Vec4& r1, const Vec4& r2) {
    Mat4 b;
    b.m[0] = r0.x; b.m[4] = r0.y; b.m[8]  = r0.z; b.m[12] = r0.w;
    b.m[1] = r1.x; b.m[5] = r1.y; b.m[9]  = r1.z; b.m[13] = r1.w;
    b.m[2] = r2.x; b.m[6] = r2.y; b.m[10] = r2.z; b.m[14] = r2.w;
    b.m[3] = 0.0f; b.m[7] = 0.0f; b.m[11] = 0.0f; b.m[15] = 1.0f;
    return b;
}
}  // namespace

void Renderer::drawCharPreview(const CharInst& c, int vx, int vy, int vw, int vh, const Camera& cam, int lod, int sx, int sy, int sw, int sh) {
    if (vw <= 0 || vh <= 0 || !charProg_) return;
    if (sw <= 0 || sh <= 0) { sx = vx; sy = vy; sw = vw; sh = vh; }
    glViewport(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST); glScissor(sx, sy, sw, sh);
    glDepthMask(GL_TRUE); glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    const Mat4 vp = cam.projection((float)vw / (float)vh) * cam.view();
    glUseProgram(charProg_);
    const float so = shadowOn_; shadowOn_ = 0.0f;
    applyLit(litChar_, cam.position);
    shadowOn_ = so;
    glUniform1f(uCharClip_, -1000.0f);
    const Mat4 model = charModel(c);
    useSkin(c.skin);
    drawMesh(std::max(0, std::min(2, lod)), vp * model, model, c.color, c.boneRows, false);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w_, h_);
}

void Renderer::drawTool(uint8_t held, const Mat4& mvp, const Mat4& model, const Vec4* boneRows, bool depthOnly) {
    if (held == 0 || held > 5) return;
    static const Vec3 kMetal[5] = {{0.56f, 0.55f, 0.55f}, {0.52f, 0.52f, 0.54f}, {0.30f, 0.21f, 0.13f}, {0.42f, 0.43f, 0.44f}, {0.40f, 0.22f, 0.12f}};
    if (boneRows) {
        const Vec4* rb = boneRows + 34 * 3;
        Mat4 B = matFromRows(rb[0], rb[1], rb[2]);
        drawMeshRef(toolMeshes_[held - 1], mvp * B, model * B, kMetal[held - 1], nullptr, depthOnly, 1.2f);
    } else {
        drawMeshRef(toolMeshes_[held - 1], mvp, model, kMetal[held - 1], nullptr, depthOnly, 1.2f);
    }
}

void Renderer::drawMeshRef(const Mesh& mesh, const Mat4& mvp, const Mat4& model, const Vec3& body, const Vec4* boneRows, bool depthOnly, float alpha) {
    if (mesh.subs.empty()) return;
    if (depthOnly) {
        glUniformMatrix4fv(uShadowMVP_, 1, GL_FALSE, mvp.m);
        if (boneRows) {
            glUniform1i(uShadowSkinned_, 1);
            glUniform4fv(uShadowBones_, kBoneRows, &boneRows[0].x);
        } else {
            glUniform1i(uShadowSkinned_, 0);
        }
    } else {
        glUniformMatrix4fv(uCharMVP_, 1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(uCharModel_, 1, GL_FALSE, model.m);
        if (boneRows) {
            glUniform1i(uCharSkinned_, 1);
            glUniform4fv(uCharBones_, kBoneRows, &boneRows[0].x);
        } else {
            glUniform1i(uCharSkinned_, 0);
        }
        glUniform1f(uCharAlpha_, alpha);
    }
    const SkinDef* sk = (depthOnly || !mesh.tex) ? nullptr : pendSkin_;   // a skin (material) só vale para a malha do personagem
    if (!depthOnly) {
        if (mesh.tex) { glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, mesh.tex); glActiveTexture(GL_TEXTURE0); glUniform1i(uCharTex_, 5); }
        glUniform1f(uCharTexOn_, mesh.tex ? 1.0f : 0.0f);
    }
    if (sk) {
        static const auto t0 = std::chrono::steady_clock::now();
        glUniform4f(uCharSkin_, sk->metal, sk->glow, sk->tintK, 0.0f);
        glUniform3f(uCharSkin2_, sk->tint.x, sk->tint.y, sk->tint.z);
        glUniform1f(uCharSkinT_, std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count());
    }
    for (const Sub& s : mesh.subs) {
        if (!depthOnly) { Vec3 c = mesh.tex ? Vec3{1.0f, 1.0f, 1.0f} : (s.body ? body : s.color); glUniform3f(uCharColor_, c.x, c.y, c.z); }
        glBindVertexArray(s.vao);
        glDrawElements(GL_TRIANGLES, s.count, GL_UNSIGNED_INT, nullptr);
        gfx_.tris += s.count / 3;
        ++gfx_.draws;
    }
    if (sk) glUniform4f(uCharSkin_, 0, 0, 0, 0);
    if (!depthOnly && mesh.tex) glUniform1f(uCharTexOn_, 0.0f);
    glBindVertexArray(0);
}


// ---- MELHORIAS DA ILHA (shared/isle_build.h): estruturas procedurais com cores de vértice ----
namespace {
struct SGeo {
    std::vector<PropVertex>& v; float cx, cy, cz, c, s;
    SGeo(std::vector<PropVertex>& out, const e0net::IsleStruct& t) : v(out), cx(t.x), cy(t.y), cz(t.z), c(std::cos(e0net::structYaw(t.rot))), s(std::sin(e0net::structYaw(t.rot))) {}
    SGeo(std::vector<PropVertex>& out, float x, float y, float z, float cs, float sn) : v(out), cx(x), cy(y), cz(z), c(cs), s(sn) {}   // eixo local x = (cs, -sn), z = (sn, cs)
    Vec3 W(float lx, float ly, float lz) const { return {cx + lx * c + lz * s, cy + ly, cz - lx * s + lz * c}; }
    Vec3 N(float nx, float ny, float nz) const { return {nx * c + nz * s, ny, -nx * s + nz * c}; }
    void vert(const Vec3& p, const Vec3& n, const uint8_t* col) {
        PropVertex q; q.x = p.x; q.y = p.y; q.z = p.z;
        const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) + 1e-6f;
        q.nx = (int8_t)(n.x / l * 127.0f); q.ny = (int8_t)(n.y / l * 127.0f); q.nz = (int8_t)(n.z / l * 127.0f); q.nw = 0;
        q.r = col[0]; q.g = col[1]; q.b = col[2]; q.a = col[3]; v.push_back(q);
    }
    // triângulo em coordenadas LOCAIS com normal local
    void tri(Vec3 a, Vec3 b, Vec3 d, Vec3 n, const uint8_t* col) { const Vec3 wn = N(n.x, n.y, n.z); vert(W(a.x, a.y, a.z), wn, col); vert(W(b.x, b.y, b.z), wn, col); vert(W(d.x, d.y, d.z), wn, col); }
    void quad(Vec3 a, Vec3 b, Vec3 d, Vec3 e, Vec3 n, const uint8_t* col) { tri(a, b, d, n, col); tri(a, d, e, n, col); }
    void box(float x0, float y0, float z0, float x1, float y1, float z1, const uint8_t* col, bool bottom = false) {
        quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1}, col);
        quad({x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {0, 0, -1}, col);
        quad({x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {1, 0, 0}, col);
        quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1, 0, 0}, col);
        uint8_t tc[4] = {(uint8_t)std::min(255, col[0] + 14), (uint8_t)std::min(255, col[1] + 14), (uint8_t)std::min(255, col[2] + 12), col[3]};
        quad({x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, {0, 1, 0}, tc);
        if (bottom) quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {0, -1, 0}, col);
    }
    // tronco vertical (n lados) com ponta em cone opcional
    void log(float x, float z, float y0, float y1, float r, int n, const uint8_t* col, float tip, const uint8_t* tipCol) {
        for (int i = 0; i < n; ++i) {
            const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n, am = (a0 + a1) * 0.5f;
            const Vec3 nn{std::cos(am), 0, std::sin(am)};
            const Vec3 p0{x + std::cos(a0) * r, y0, z + std::sin(a0) * r}, p1{x + std::cos(a1) * r, y0, z + std::sin(a1) * r};
            quad(p1, p0, {p0.x, y1, p0.z}, {p1.x, y1, p1.z}, nn, col);
            if (tip > 0) tri({p1.x, y1, p1.z}, {p0.x, y1, p0.z}, {x, y1 + tip, z}, {nn.x, 0.6f, nn.z}, tipCol);
            else tri({p1.x, y1, p1.z}, {p0.x, y1, p0.z}, {x, y1, z}, {0, 1, 0}, tipCol);
        }
    }
    // tronco/viga entre dois pontos locais (seção quadrada girada 45 graus: barato e redondo o bastante de longe)
    void beam(Vec3 a, Vec3 b, float r, const uint8_t* col) {
        Vec3 d{b.x - a.x, b.y - a.y, b.z - a.z};
        const float L = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) + 1e-6f; d = {d.x / L, d.y / L, d.z / L};
        Vec3 up = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 u{d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x};
        float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z); u = {u.x / ul, u.y / ul, u.z / ul};
        Vec3 w{d.y * u.z - d.z * u.y, d.z * u.x - d.x * u.z, d.x * u.y - d.y * u.x};
        for (int i = 0; i < 4; ++i) {
            const float a0 = 1.5707963f * i, a1 = a0 + 1.5707963f, am = a0 + 0.785398f;
            auto off = [&](float an) { return Vec3{(u.x * std::cos(an) + w.x * std::sin(an)) * r, (u.y * std::cos(an) + w.y * std::sin(an)) * r, (u.z * std::cos(an) + w.z * std::sin(an)) * r}; };
            const Vec3 o0 = off(a0), o1 = off(a1), nm = off(am);
            quad({a.x + o0.x, a.y + o0.y, a.z + o0.z}, {a.x + o1.x, a.y + o1.y, a.z + o1.z}, {b.x + o1.x, b.y + o1.y, b.z + o1.z}, {b.x + o0.x, b.y + o0.y, b.z + o0.z}, nm, col);
        }
    }
};
inline uint32_t sHash(uint32_t a) { a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16; return a; }

void buildWall(std::vector<PropVertex>& out, const e0net::IsleWall& W, uint32_t hs, float deep = 1.6f, float pierDeep = 5.0f);
void buildStruct(std::vector<PropVertex>& out, const e0net::IsleStruct& t, float deep = -2.4f, int slot = -1) {
    SGeo g(out, t);
    const uint32_t hs = sHash((uint32_t)(t.x * 37.0f) ^ ((uint32_t)(t.z * 53.0f) << 8) ^ t.kind);
    auto wood = [&](int i, uint8_t* c) { const uint32_t h = sHash(hs + (uint32_t)i * 977u); const int d = (int)(h % 26) - 13; c[0] = (uint8_t)(112 + d); c[1] = (uint8_t)(79 + d * 3 / 4); c[2] = (uint8_t)(50 + d / 2); c[3] = 255; };
    uint8_t W0[4], tipC[4] = {168, 134, 92, 255}, dark[4] = {70, 50, 32, 255}, plank[4] = {138, 102, 66, 255}, plank2[4] = {124, 90, 58, 255};
    uint8_t stone[4] = {120, 116, 108, 255}, roof[4] = {112, 62, 38, 255}, roof2[4] = {96, 54, 34, 255}, rope[4] = {178, 152, 102, 255};
    // deep: fundação/estacas entram no chão (cobre o desnível do terreno); nas miniaturas fica no chão
    switch (t.kind) {
        case e0net::SkWall:
            if (const e0net::IsleWall* W = e0net::wallGeomOf(slot)) buildWall(out, *W, hs);
            break;
        case e0net::SkTower: {
            const float P = 1.25f, plat = 4.3f;
            for (int i = 0; i < 4; ++i) { wood(i, W0); g.log((i & 1) ? P : -P, (i & 2) ? P : -P, deep, plat + 1.0f, 0.19f, 6, W0, 0.0f, W0); }
            for (int f = 0; f < 4; ++f) {   // travessas em X nas 4 faces
                const float ax = f < 2 ? -P : P, az = f < 2 ? -P : P;
                if (f % 2 == 0) { g.beam({-P, 0.4f, f < 2 ? -P : P}, {P, plat - 0.3f, f < 2 ? -P : P}, 0.07f, dark); g.beam({P, 0.4f, f < 2 ? -P : P}, {-P, plat - 0.3f, f < 2 ? -P : P}, 0.07f, dark); }
                else { g.beam({ax, 0.4f, -P}, {ax, plat - 0.3f, P}, 0.07f, dark); g.beam({ax, 0.4f, P}, {ax, plat - 0.3f, -P}, 0.07f, dark); }
                (void)az;
            }
            g.box(-1.6f, plat, -1.6f, 1.6f, plat + 0.2f, 1.6f, plank, true);                       // plataforma
            for (int sd = 0; sd < 4; ++sd) {                                                        // parapeito
                const float a = 1.55f;
                if (sd == 0) g.box(-a, plat + 0.85f, -a, a, plat + 0.97f, -a + 0.1f, plank2);
                if (sd == 1) g.box(-a, plat + 0.85f, a - 0.1f, a, plat + 0.97f, a, plank2);
                if (sd == 2) g.box(-a, plat + 0.85f, -a, -a + 0.1f, plat + 0.97f, a, plank2);
                if (sd == 3) g.box(a - 0.1f, plat + 0.85f, -a, a, plat + 0.97f, a, plank2);
            }
            const float ry = plat + 1.0f, top = plat + 2.5f, o = 1.9f;                               // telhado em pirâmide
            g.tri({-o, ry, o}, {o, ry, o}, {0, top, 0}, {0, 0.7f, 1}, roof);
            g.tri({o, ry, -o}, {-o, ry, -o}, {0, top, 0}, {0, 0.7f, -1}, roof2);
            g.tri({o, ry, o}, {o, ry, -o}, {0, top, 0}, {1, 0.7f, 0}, roof);
            g.tri({-o, ry, -o}, {-o, ry, o}, {0, top, 0}, {-1, 0.7f, 0}, roof2);
            g.beam({-0.35f, 0.0f, P + 0.25f}, {-0.35f, plat + 0.2f, P + 0.25f}, 0.05f, dark);         // escada
            g.beam({0.35f, 0.0f, P + 0.25f}, {0.35f, plat + 0.2f, P + 0.25f}, 0.05f, dark);
            for (int k = 0; k < 11; ++k) g.box(-0.36f, 0.35f + k * 0.38f, P + 0.2f, 0.36f, 0.41f + k * 0.38f, P + 0.3f, plank);
            break;
        }
        case e0net::SkWorkshop: {
            g.box(-3.3f, deep, -2.6f, 3.3f, 0.15f, 2.6f, stone);                                    // base de pedra
            g.box(-3.2f, 0.15f, -2.5f, 3.2f, 3.0f, -2.15f, plank);                                  // fundo
            g.box(-3.2f, 0.15f, -2.5f, -2.85f, 3.0f, 0.6f, plank2);                                 // laterais (frente aberta)
            g.box(2.85f, 0.15f, -2.5f, 3.2f, 3.0f, 0.6f, plank2);
            for (int i = 0; i < 6; ++i) g.box(-3.21f, 0.4f + i * 0.46f, -2.51f, 3.21f, 0.44f + i * 0.46f, -2.14f, dark);   // frestas das tábuas
            g.log(-3.0f, 2.3f, 0.15f, 3.0f, 0.15f, 6, dark, 0.0f, dark);                            // esteios da frente
            g.log(3.0f, 2.3f, 0.15f, 3.0f, 0.15f, 6, dark, 0.0f, dark);
            const float ridge = 4.2f, eave = 2.85f, ox = 3.55f, oz = 3.0f;                         // telhado de duas águas
            g.quad({-ox, eave, -oz}, {-ox, ridge, 0}, {ox, ridge, 0}, {ox, eave, -oz}, {0, 0.9f, -0.66f}, roof2);
            g.quad({ox, eave, oz}, {ox, ridge, 0}, {-ox, ridge, 0}, {-ox, eave, oz}, {0, 0.9f, 0.66f}, roof);
            g.tri({3.2f, 3.0f, -2.5f}, {3.2f, 3.0f, 2.5f}, {3.2f, ridge - 0.1f, 0}, {1, 0, 0}, plank2);
            g.tri({-3.2f, 3.0f, 2.5f}, {-3.2f, 3.0f, -2.5f}, {-3.2f, ridge - 0.1f, 0}, {-1, 0, 0}, plank2);
            g.box(-1.3f, 0.78f, -1.1f, 1.3f, 0.92f, -0.3f, plank);                                  // bancada
            for (int i = 0; i < 4; ++i) g.box((i & 1) ? 1.1f : -1.25f, 0.15f, (i & 2) ? -0.45f : -1.05f, (i & 1) ? 1.25f : -1.1f, 0.78f, (i & 2) ? -0.3f : -0.9f, dark);
            uint8_t steel[4] = {176, 178, 182, 255};
            g.box(0.2f, 0.92f, -0.85f, 0.95f, 0.94f, -0.65f, steel);                                // serrote
            g.box(-0.9f, 0.92f, -0.8f, -0.55f, 1.05f, -0.55f, rope);                                // novelo de corda
            for (int i = 0; i < 3; ++i) { wood(i + 20, W0); g.beam({1.6f + i * 0.0f, 0.35f + i * 0.34f, -1.9f + (i == 2 ? 0.17f : 0.0f) + i * 0.0f}, {2.6f, 0.35f + i * 0.34f, 1.6f}, 0.17f, W0); }   // toras empilhadas
            g.beam({-2.4f, 0.75f, 0.6f}, {-1.6f, 0.75f, 0.6f}, 0.09f, plank);                       // cavalete
            g.beam({-2.3f, 0.15f, 0.35f}, {-2.3f, 0.75f, 0.6f}, 0.05f, dark); g.beam({-2.3f, 0.15f, 0.85f}, {-2.3f, 0.75f, 0.6f}, 0.05f, dark);
            g.beam({-1.7f, 0.15f, 0.35f}, {-1.7f, 0.75f, 0.6f}, 0.05f, dark); g.beam({-1.7f, 0.15f, 0.85f}, {-1.7f, 0.75f, 0.6f}, 0.05f, dark);
            break;
        }
        case e0net::SkStorehouse: {
            g.box(-2.7f, deep, -2.2f, 2.7f, 0.2f, 2.2f, stone);
            g.box(-2.6f, 0.2f, -2.1f, 2.6f, 2.8f, 2.1f, plank);
            for (int i = 0; i < 5; ++i) g.box(-2.61f, 0.55f + i * 0.48f, -2.11f, 2.61f, 0.59f + i * 0.48f, 2.11f, dark);
            g.box(-0.75f, 0.2f, 2.1f, 0.75f, 2.2f, 2.16f, dark);                                     // porta
            g.box(-0.08f, 0.9f, 2.16f, 0.08f, 1.5f, 2.2f, rope);
            for (int sd = -1; sd <= 1; sd += 2) for (int k = -1; k <= 1; k += 2) g.log(sd * 2.6f, k * 2.1f, 0.2f, 2.8f, 0.13f, 6, dark, 0.0f, dark);
            const float ridge = 4.0f, eave = 2.7f, ox = 2.95f, oz = 2.55f;
            g.quad({-ox, eave, -oz}, {-ox, ridge, 0}, {ox, ridge, 0}, {ox, eave, -oz}, {0, 0.9f, -0.66f}, roof2);
            g.quad({ox, eave, oz}, {ox, ridge, 0}, {-ox, ridge, 0}, {-ox, eave, oz}, {0, 0.9f, 0.66f}, roof);
            g.tri({2.6f, 2.8f, -2.1f}, {2.6f, 2.8f, 2.1f}, {2.6f, ridge - 0.1f, 0}, {1, 0, 0}, plank2);
            g.tri({-2.6f, 2.8f, 2.1f}, {-2.6f, 2.8f, -2.1f}, {-2.6f, ridge - 0.1f, 0}, {-1, 0, 0}, plank2);
            uint8_t crate[4] = {150, 112, 70, 255}, barrel[4] = {104, 70, 44, 255};
            g.box(1.2f, 0.0f, 2.4f, 1.9f, 0.7f, 3.1f, crate); g.box(1.3f, 0.7f, 2.5f, 1.85f, 1.25f, 3.0f, crate);
            g.log(-1.6f, 2.8f, 0.0f, 0.95f, 0.36f, 8, barrel, 0.0f, plank);
            g.log(-2.3f, 2.6f, 0.0f, 0.95f, 0.36f, 8, barrel, 0.0f, plank);
            break;
        }
        case e0net::SkCampfire: {
            for (int i = 0; i < 9; ++i) {
                const float a = 6.2831853f * i / 9.0f, x = std::cos(a) * 0.58f, z = std::sin(a) * 0.58f;
                const uint8_t d = (uint8_t)(sHash(hs + i) % 30);
                uint8_t sc[4] = {(uint8_t)(100 + d), (uint8_t)(97 + d), (uint8_t)(92 + d), 255};
                g.box(x - 0.14f, -0.1f, z - 0.12f, x + 0.14f, 0.18f, z + 0.12f, sc);
            }
            uint8_t ash[4] = {46, 40, 36, 255}, ember[4] = {255, 120, 30, 252};
            g.box(-0.42f, -0.05f, -0.42f, 0.42f, 0.03f, 0.42f, ash);
            g.box(-0.22f, 0.03f, -0.22f, 0.22f, 0.08f, 0.22f, ember);
            for (int i = 0; i < 5; ++i) { const float a = 6.2831853f * i / 5.0f + 0.3f; wood(i, W0); g.beam({std::cos(a) * 0.45f, 0.02f, std::sin(a) * 0.45f}, {std::cos(a) * 0.05f, 0.62f, std::sin(a) * 0.05f}, 0.06f, W0); }
            break;
        }
    }
}
}  // namespace


// MURALHA em volta da ilha: troncos pontudos seguindo o relevo, travessas, PORTÃO com arco e CAIS de tábuas até o mar fundo
namespace {
void buildWall(std::vector<PropVertex>& out, const e0net::IsleWall& W, uint32_t hs, float deep, float pierDeep) {   // deep/pierDeep: estacas dentro do chão / até o fundo
    SGeo g(out, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);   // coordenadas de mundo
    uint8_t W0[4], tipC[4] = {168, 134, 92, 255}, dark[4] = {70, 50, 32, 255}, plank[4] = {138, 102, 66, 255}, plank2[4] = {120, 88, 56, 255}, rope[4] = {178, 152, 102, 255};
    const e0net::World& w = *W.w;
    const int n = (int)W.x.size();
    int post = 0;
    for (int i = 0; i < n; ++i) {
        if (i == W.gate) continue;
        const int j = (i + 1) % n;
        const float ax = W.x[i], az = W.z[i], bx = W.x[j], bz = W.z[j], L = std::hypot(bx - ax, bz - az);
        const int k = std::max(1, (int)std::lround(L / 0.46f));
        const float ix = (bz - az) / (L + 1e-5f), iz = -(bx - ax) / (L + 1e-5f);   // perpendicular (o lado de dentro é o oposto da normal do portão... basta um lado)
        for (int q = 0; q < k; ++q, ++post) {
            const float t = (q + 0.5f) / (float)k, x = ax + (bx - ax) * t, z = az + (bz - az) * t;
            const float gh = std::max(0.0f, w.heightAt(x, z));
            const uint32_t h = sHash(hs + (uint32_t)post * 977u);
            const int d = (int)(h % 26) - 13; W0[0] = (uint8_t)(112 + d); W0[1] = (uint8_t)(79 + d * 3 / 4); W0[2] = (uint8_t)(50 + d / 2); W0[3] = 255;
            g.log(x, z, gh - deep, gh + 2.55f + 0.35f * (float)(h % 100) / 100.0f, 0.21f, 5, W0, 0.42f, tipC);
        }
        const float ha = std::max(0.0f, w.heightAt(ax, az)), hb = std::max(0.0f, w.heightAt(bx, bz));   // travessas (lado de dentro)
        g.beam({ax - ix * 0.24f, ha + 0.85f, az - iz * 0.24f}, {bx - ix * 0.24f, hb + 0.85f, bz - iz * 0.24f}, 0.07f, dark);
        g.beam({ax - ix * 0.24f, ha + 2.05f, az - iz * 0.24f}, {bx - ix * 0.24f, hb + 2.05f, bz - iz * 0.24f}, 0.07f, dark);
    }
    // PORTÃO: eixo local x ao longo do vão, z para o mar (normal do portão)
    SGeo q(out, W.gx, W.gy, W.gz, W.nz, W.nx);
    const float hw = W.half;
    q.log(-hw, 0.0f, -deep, 3.9f, 0.30f, 8, dark, 0.0f, dark);
    q.log(hw, 0.0f, -deep, 3.9f, 0.30f, 8, dark, 0.0f, dark);
    q.box(-hw - 0.3f, 3.35f, -0.22f, hw + 0.3f, 3.7f, 0.22f, plank2);   // verga
    q.box(-0.9f, 3.75f, -0.07f, 0.9f, 4.15f, 0.07f, plank);             // placa
    for (int sd = -1; sd <= 1; sd += 2) {                                // folhas abertas para dentro (-z)
        const float x0 = sd * (hw - 0.25f), x1 = sd * (hw - 0.14f);
        q.box(std::min(x0, x1), 0.1f, -hw + 0.3f, std::max(x0, x1), 2.9f, -0.15f, plank);
        q.box(std::min(x0, x1) - 0.02f, 0.7f, -hw + 0.35f, std::max(x0, x1) + 0.02f, 0.85f, -0.2f, dark);
        q.box(std::min(x0, x1) - 0.02f, 2.2f, -hw + 0.35f, std::max(x0, x1) + 0.02f, 2.35f, -0.2f, dark);
    }
    // CAIS: tabuado na altura kPierDeckY, estacas até o fundo, cabeços de amarração e um lampião na ponta
    SGeo p(out, W.gx, 0.0f, W.gz, W.nz, W.nx);
    const float dy = e0net::kPierDeckY, hwP = e0net::kPierHalfW;
    for (float u = W.p0 - 0.5f; u < W.p1; u += 0.55f) {
        const uint32_t h = sHash(hs + (uint32_t)(u * 10.0f));
        const uint8_t b = (uint8_t)(h % 18);
        uint8_t pc[4] = {(uint8_t)(132 + b), (uint8_t)(98 + b), (uint8_t)(62 + b / 2), 255};
        p.box(-hwP, dy - 0.12f, u, hwP, dy, u + 0.5f, pc, true);
    }
    for (float u = W.p0; u <= W.p1 + 0.01f; u += 3.0f) for (int sd = -1; sd <= 1; sd += 2) {
        p.log(sd * (hwP - 0.12f), u + 0.25f, -pierDeep, dy - 0.12f, 0.16f, 6, dark, 0.0f, dark);
    }
    p.beam({-hwP + 0.05f, dy - 0.3f, W.p0}, {-hwP + 0.05f, dy - 0.3f, W.p1}, 0.07f, dark);
    p.beam({hwP - 0.05f, dy - 0.3f, W.p0}, {hwP - 0.05f, dy - 0.3f, W.p1}, 0.07f, dark);
    for (int sd = -1; sd <= 1; sd += 2) {   // cabeços de amarração na ponta + corda enrolada
        p.log(sd * (hwP - 0.3f), W.p1 - 0.6f, dy, dy + 0.55f, 0.13f, 6, dark, 0.0f, dark);
        p.box(sd * (hwP - 0.3f) - 0.17f, dy + 0.38f, W.p1 - 0.77f, sd * (hwP - 0.3f) + 0.17f, dy + 0.48f, W.p1 - 0.43f, rope);
    }
    p.log(-hwP + 0.25f, W.p1 - 2.5f, dy, dy + 2.6f, 0.07f, 5, dark, 0.0f, dark);   // poste do lampião
    uint8_t lamp[4] = {255, 200, 90, 252};
    p.box(-hwP + 0.13f, dy + 2.35f, W.p1 - 2.62f, -hwP + 0.37f, dy + 2.65f, W.p1 - 2.38f, lamp);
}
}  // namespace
void Renderer::drawStructs(const Camera& cam, const Mat4& vp, float nowSec) {
    static std::vector<PropVertex> tmp;
    bool any = false;
    for (int sl = 1; sl < e0net::kIslandSlots; ++sl) {
        StructMesh& M = structMesh_[sl];
        const auto& list = e0net::isleStructs(sl);
        if (list.empty() && !M.built) continue;
        if (!M.built || M.gen != e0net::isleStructGen(sl)) {
            tmp.clear();
            for (const e0net::IsleStruct& t : list) buildStruct(tmp, t, -2.4f, sl);
            if (!M.vao) {
                glGenVertexArrays(1, &M.vao); glGenBuffers(1, &M.vbo);
                glBindVertexArray(M.vao); glBindBuffer(GL_ARRAY_BUFFER, M.vbo);
                glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PropVertex), (void*)0);
                glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(PropVertex), (void*)12);
                glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PropVertex), (void*)16);
            }
            glBindVertexArray(M.vao); glBindBuffer(GL_ARRAY_BUFFER, M.vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tmp.size() * sizeof(PropVertex)), tmp.empty() ? nullptr : tmp.data(), GL_STATIC_DRAW);
            glBindVertexArray(0);
            M.count = (int)tmp.size(); M.gen = e0net::isleStructGen(sl); M.built = true;
        }
        if (!M.count) continue;
        const e0net::IsleStruct& f = list.front();
        if (std::hypot(f.x - cam.position.x, f.z - cam.position.z) > 900.0f) continue;
        if (!any) {
            any = true;
            glUseProgram(propProg_);
            applyLit(litProp_, cam.position);
            glUniformMatrix4fv(uPropVP_, 1, GL_FALSE, vp.m);
            glUniform1f(uPropClip_, -2.35f);
            glUniform1f(uPropTime_, nowSec);
            glDisable(GL_CULL_FACE);
        }
        glBindVertexArray(M.vao);
        glDrawArrays(GL_TRIANGLES, 0, M.count);
        gfx_.tris += M.count / 3; ++gfx_.draws;
    }
    if (any) { glBindVertexArray(0); glEnable(GL_CULL_FACE); }
}

// MINIATURAS 3D dos cartões (tela de MELHORIAS DA ILHA): cada estrutura desenhada 1x num FBO com luz de meio-dia, fundo transparente
void Renderer::bakeStructThumbs() {
    if (!wantThumbs_) return;
    wantThumbs_ = false;
    const int W = 256, H = 192;
    const Environment saveEnv = env_; const float saveShadow = shadowOn_, saveFog = fogK_; Vec4 saveTorch[4]; std::copy(torch_, torch_ + 4, saveTorch);
    env_ = makeEnvironment(450.0); shadowOn_ = 0.0f; fogK_ = 0.0f; for (Vec4& t : torch_) t = Vec4{0, 0, 0, 0};
    GLuint depth = 0; glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    std::vector<PropVertex> pv;
    GLuint vao = 0, vbo = 0; glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PropVertex), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(PropVertex), (void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PropVertex), (void*)16);
    for (int k = 0; k < e0net::SkKindCount; ++k) {
        if (thumbTex_[k]) continue;
        const e0net::StructDef& D = e0net::structDef(k);
        e0net::IsleStruct t; t.kind = (uint8_t)k;
        pv.clear();
        if (k == e0net::SkWall) {   // miniatura da MURALHA: um pedaço de paliçada em curva + portão
            e0net::World flat; flat.hcm.assign((size_t)e0net::World::kVerts * e0net::World::kVerts, 0); flat.biome.assign(flat.hcm.size(), 0);
            e0net::IsleWall Wm; Wm.ok = true; Wm.w = &flat;
            for (int q = 0; q < 12; ++q) { const float a = 6.2831853f * q / 12.0f + 0.2617994f; Wm.x.push_back(std::sin(a) * 6.0f); Wm.z.push_back(std::cos(a) * 6.0f); }   // anel pequeno fechado
            Wm.gate = 11; Wm.gx = (Wm.x[11] + Wm.x[0]) * 0.5f; Wm.gz = (Wm.z[11] + Wm.z[0]) * 0.5f; Wm.gy = 0; Wm.nx = 0; Wm.nz = 1; Wm.half = std::hypot(Wm.x[0] - Wm.x[11], Wm.z[0] - Wm.z[11]) * 0.5f;
            Wm.p0 = 0.5f; Wm.p1 = 5.0f;
            buildWall(pv, Wm, 7u, 0.02f, 0.0f);
        } else buildStruct(pv, t, -0.05f);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pv.size() * sizeof(PropVertex)), pv.data(), GL_STATIC_DRAW);
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, W, H);
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
            const bool wall = k == e0net::SkWall;
            const float R = wall ? 8.5f : std::sqrt(D.hx * D.hx + D.hz * D.hz + D.height * D.height * 0.25f) + 0.3f;
            const Vec3 ctr = wall ? Vec3{0.0f, 0.8f, 1.5f} : Vec3{0.0f, D.height * 0.42f, 0.0f}, dir = wall ? normalize(Vec3{0.35f, 0.55f, 0.95f}) : normalize(Vec3{0.62f, 0.52f, 0.78f});
            Camera cam; cam.position = ctr + dir * (R * 2.75f); cam.fovY = 0.62f; cam.zNear = 0.1f; cam.zFar = 200.0f;
            const Vec3 f = normalize(ctr - cam.position);
            cam.yaw = std::atan2(f.x, -f.z); cam.pitch = std::asin(f.y);
            const Mat4 vp = cam.projection((float)W / (float)H) * cam.view();
            glUseProgram(propProg_);
            applyLit(litProp_, cam.position);
            glUniformMatrix4fv(uPropVP_, 1, GL_FALSE, vp.m);
            glUniform1f(uPropClip_, -100.0f);
            glUniform1f(uPropTime_, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)pv.size());
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
        thumbTex_[k] = tex;
    }
    glBindVertexArray(0); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao); glDeleteRenderbuffers(1, &depth);
    env_ = saveEnv; shadowOn_ = saveShadow; fogK_ = saveFog; std::copy(saveTorch, saveTorch + 4, torch_);
    glViewport(0, 0, w_, h_); glEnable(GL_CULL_FACE);
}

void Renderer::uiTexture(unsigned tex, float cx, float cy, float halfW, float halfH, float alpha) {
    if (!tex) return;
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, tex); glUniform1i(uUiIcon_, 6); glActiveTexture(GL_TEXTURE0);
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiThickness_, 0.0f);
    glUniform4f(uUiUv_, 0.0f, 1.0f, 1.0f, 0.0f);   // textura de FBO: v = 0 embaixo
    glUniform4f(uUiColor_, 1.0f, 1.0f, 1.0f, alpha);
    glUniform1i(uUiMode_, 7);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::drawStructShadow(const Vec3& focus, float reach, const Mat4& lvp) {
    bool set = false;
    for (int sl = 1; sl < e0net::kIslandSlots; ++sl) {
        const StructMesh& M = structMesh_[sl];
        const auto& list = e0net::isleStructs(sl);
        if (!M.count || list.empty()) continue;
        bool near = false;
        for (const e0net::IsleStruct& t : list) if (std::fabs(t.x - focus.x) < reach + 8.0f && std::fabs(t.z - focus.z) < reach + 8.0f) { near = true; break; }
        if (!near) continue;
        if (!set) { set = true; glUniform1i(uShadowSkinned_, 0); glUniformMatrix4fv(uShadowMVP_, 1, GL_FALSE, lvp.m); glDisable(GL_CULL_FACE); }
        glBindVertexArray(M.vao);
        glDrawArrays(GL_TRIANGLES, 0, M.count);
        ++gfx_.draws;
    }
    if (set) { glBindVertexArray(0); glEnable(GL_CULL_FACE); }
}

void Renderer::drawStructGhost(const Mat4& vp) {
    if (!gizmoProg_ || !gizmoVao_) return;
    static std::vector<PropVertex> pv; static std::vector<float> gv;
    static StructGhost lastG; static uint32_t lastGen = 0; static int lastCount = 0; static unsigned lastVbo = 0;
    const uint32_t gen = ghost_.slot > 0 ? e0net::isleStructGen(ghost_.slot) : 0;
    const bool same = lastCount > 0 && lastG.active == ghost_.active && lastG.ok == ghost_.ok && lastG.kind == ghost_.kind && lastG.rot == ghost_.rot && lastG.selId == ghost_.selId &&
                      lastG.slot == ghost_.slot && lastG.pos.x == ghost_.pos.x && lastG.pos.y == ghost_.pos.y && lastG.pos.z == ghost_.pos.z && lastGen == gen && lastVbo == gizmoVbo_;
    if (!ghost_.active && !ghost_.selId) { lastCount = 0; return; }
    if (same) {   // nada mudou: só redesenha (a prévia da MURALHA tem dezenas de milhares de vértices)
        glUseProgram(gizmoProg_); glUniformMatrix4fv(uGizmoVP_, 1, GL_FALSE, vp.m);
        glBindVertexArray(gizmoVao_);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_CULL_FACE); glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -4.0f);
        glDrawArrays(GL_TRIANGLES, 0, lastCount);
        glDisable(GL_POLYGON_OFFSET_FILL); glDepthMask(GL_TRUE); glEnable(GL_CULL_FACE); glDisable(GL_BLEND); glBindVertexArray(0);
        return;
    }
    lastG = ghost_; lastGen = gen; lastVbo = gizmoVbo_;
    pv.clear();
    float r = 0, g = 0, b = 0, a = 0.5f;
    if (ghost_.active) {
        e0net::IsleStruct t; t.kind = ghost_.kind; t.rot = ghost_.rot; t.x = ghost_.pos.x; t.y = ghost_.pos.y; t.z = ghost_.pos.z;
        buildStruct(pv, t, -2.4f, ghost_.slot);
        if (ghost_.ok) { r = 0.25f; g = 1.0f; b = 0.40f; } else { r = 1.0f; g = 0.22f; b = 0.18f; }
    } else if (ghost_.selId) {   // estrutura selecionada (demolir): contorno dourado por cima
        for (int sl = 1; sl < e0net::kIslandSlots && pv.empty(); ++sl)
            for (const e0net::IsleStruct& t : e0net::isleStructs(sl)) if (t.id == ghost_.selId) { buildStruct(pv, t, -2.4f, sl); break; }
        r = 1.0f; g = 0.80f; b = 0.25f; a = 0.38f;
    }
    if (pv.empty()) { lastCount = 0; return; }
    gv.clear(); gv.reserve(pv.size() * 7);
    for (const PropVertex& p : pv) {
        const float sh = 0.55f + 0.45f * std::max(0.0f, (p.ny / 127.0f) * 0.8f + 0.2f);
        gv.insert(gv.end(), {p.x, p.y, p.z, r * sh, g * sh, b * sh, a});
    }
    glUseProgram(gizmoProg_);
    glUniformMatrix4fv(uGizmoVP_, 1, GL_FALSE, vp.m);
    glBindVertexArray(gizmoVao_); glBindBuffer(GL_ARRAY_BUFFER, gizmoVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gv.size() * sizeof(float)), gv.data(), GL_DYNAMIC_DRAW);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -4.0f);
    lastCount = (int)(gv.size() / 7);
    glDrawArrays(GL_TRIANGLES, 0, lastCount);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE); glEnable(GL_CULL_FACE); glDisable(GL_BLEND);
    glBindVertexArray(0);
}

void Renderer::gpuTimerBegin() {
    if (!gpuTimer_) return;
    for (int i = 0; i < 3; ++i) {  // recolhe resultados prontos de frames anteriores (sem bloquear)
        if (!gpuPending_[i]) continue;
        GLint avail = 0;
        qGeti(gpuQ_[i], GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
        if (!avail) continue;
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        unsigned long long ns = 0;
        qGet64(gpuQ_[i], GL_QUERY_RESULT_EXT, &ns);
        gpuPending_[i] = false;
        if (!disjoint) { float ms = (float)(ns * 1e-6); gpuMs_ = gpuMs_ < 0 ? ms : gpuMs_ * 0.85f + ms * 0.15f; }
    }
    if (!gpuPending_[gpuIdx_]) qBegin(GL_TIME_ELAPSED_EXT, gpuQ_[gpuIdx_]);
    else gpuIdx_ = -1;  // sem slot livre neste frame: não mede
}

void Renderer::gpuTimerEnd() {
    if (!gpuTimer_) return;
    if (gpuIdx_ >= 0) { qEnd(GL_TIME_ELAPSED_EXT); gpuPending_[gpuIdx_] = true; gpuIdx_ = (gpuIdx_ + 1) % 3; }
    else gpuIdx_ = 0;
}

void Renderer::renderFrame(const Camera& cam, const std::vector<CharInst>& chars, const Vec3& focus) {
    const QParams& Q = cine_ ? kQCine : kQ[quality_];
    const Environment& E = env_;
    const int q = quality_;
    gfx_ = GfxStats{};
    gfx_.quality = q;
    updateTorches(cam, chars);
    bakeIslandFlags();
    bakeStructThumbs();
    gpuTimerBegin();
    gfx_.gpuMs = gpuMs_;
    const float aspect = (float)w_ / (float)h_;
    const Mat4 proj = cam.projection(aspect), view = cam.view();

    // ---- 1) passe de sombra: só casters perto do foco, em LOD baixo ----
    float lightMax = std::max(E.lightCol.x, std::max(E.lightCol.y, E.lightCol.z));
    bool doShadow = shadowFbo_ && lightMax > 0.01f;
    shadowOn_ = 0;
    shadowRadius_ = Q.shadowR;
    shadowCenter_ = focus;
    pcf_ = Q.pcf;
    if (doShadow) {
        Vec3 L = E.lightDir;                          // direção PARA a luz
        if (L.y < 0.15f) { L.y = 0.15f; L = normalize(L); }  // sol rasante: limita o comprimento da sombra
        Vec3 f = focus + Vec3{0, 1.0f, 0};
        const float eyeD = cine_ ? 100.0f : 60.0f;   // raio maior: precisa de mais profundidade no mapa
        Vec3 eye = f + L * eyeD;
        Vec3 up = std::fabs(L.y) > 0.98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Mat4 lv = Mat4::lookAt(eye, f, up);
        // ancora o centro numa grade de texels no espaço da luz (evita cintilar quando o jogador anda)
        float texel = 2.0f * Q.shadowR / (float)shadowRes_;
        Vec3 lr{lv.m[0], lv.m[4], lv.m[8]}, lu{lv.m[1], lv.m[5], lv.m[9]};
        float a = dot(f, lr), b = dot(f, lu);
        float a2 = std::floor(a / texel) * texel, b2 = std::floor(b / texel) * texel;
        f = f + lr * (a2 - a) + lu * (b2 - b);
        lv = Mat4::lookAt(f + L * 60.0f, f, up);
        Mat4 lvp = Mat4::ortho(-Q.shadowR, Q.shadowR, -Q.shadowR, Q.shadowR, 1.0f, eyeD * 2.0f) * lv;
        Mat4 bias;  // clip [-1,1] -> textura [0,1]
        bias.m[0] = bias.m[5] = bias.m[10] = 0.5f; bias.m[12] = bias.m[13] = bias.m[14] = 0.5f;
        shadowMat_ = bias * lvp;
        shadowTexel_ = texel;

        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);  // não amostrar enquanto renderiza nele
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glViewport(0, 0, shadowRes_, shadowRes_);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.5f, 5.0f);
        glUseProgram(shadowProg_);
        const int maxShadowChars = chars.size() > 24 ? 10 : (chars.size() > 14 ? 14 : 32);   // multidão: só os mais perto projetam sombra (chars vem ordenado por distância)
        for (const CharInst& c : chars) {
            if (c.hidden) continue;
            if (gfx_.shadowCasters >= maxShadowChars) break;
            float dx = c.pos.x - focus.x, dz = c.pos.z - focus.z;
            float d2 = dx * dx + dz * dz;
            if (d2 > (Q.shadowR + 3.0f) * (Q.shadowR + 3.0f)) continue;
            Mat4 model = charModel(c);
            useSkin(c.skin);   // a sombra é a do MODELO da skin (homem/mulher)
            drawMesh(chars.size() > 16 ? 2 : Q.shadowLod, lvp * model, model, c.color, c.boneRows, true);
            if (d2 < 12.0f * 12.0f) {
                drawTool(c.held, lvp * model, model, c.attachRows ? c.attachRows : c.boneRows, true);
            }
            ++gfx_.shadowCasters;
        }
        drawStructShadow(focus, Q.shadowR + 8.0f, lvp);   // MELHORIAS DA ILHA: as estruturas projetam sombra
        for (int i = 0; i < kShips; ++i) if (ships_[i].exists) {   // o navio (mastros, velas, casco) projeta sombra no convés e no mar
            float dx = ships_[i].x - focus.x, dz = ships_[i].z - focus.z, reach = Q.shadowR + 24.0f * e0net::shipDef(i).wakeA;
            if (dx * dx + dz * dz < reach * reach) drawShipShadow(i, lvp);
        }
        if (anyTerrain()) {  // árvores, pedras e construções também projetam sombra (só as perto do jogador)
            glUniform1i(uShadowSkinned_, 0);
            glUniformMatrix4fv(uShadowMVP_, 1, GL_FALSE, lvp.m);
            TerrainRenderer::Counts sc;
            for (auto& t : terr_) if (!t.empty()) t.drawPropsAround(focus, Q.shadowR + 3.0f, sc);
            gfx_.draws += sc.propChunks;
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        shadowOn_ = 1;
        gfx_.shadowOn = true; gfx_.shadowRes = shadowRes_; gfx_.shadowRadius = Q.shadowR;
    }

    // ---- 3) passe principal ----
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w_, h_);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, shadowTex_);
    glActiveTexture(GL_TEXTURE0);
    glClearColor(E.horizon.x, E.horizon.y, E.horizon.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);   // o céu é desenhado DEPOIS dos opacos (z = far, LEQUAL): só pinta o que sobrou (sem overdraw)

    Mat4 vp = proj * view;
    static const auto t0Scene = std::chrono::steady_clock::now();
    const float nowSec = clock_ >= 0.0 ? (float)clock_ : std::chrono::duration<float>(std::chrono::steady_clock::now() - t0Scene).count();
    { static float fxLast = -1.0f; updateFx(fxLast < 0.0f ? 0.016f : std::max(0.0f, std::min(0.05f, nowSec - fxLast))); fxLast = nowSec; }
    lastNow_ = nowSec;
    updateSplashes(nowSec, chars);

    if (anyTerrain()) {
        glDisable(GL_CULL_FACE);
        // 1. Terreno com texturas PBR (Sand, Dirt, Grass, Rock)
        glUseProgram(terrainProg_);
        applyLit(litTerrain_, cam.position);
        glUniformMatrix4fv(uTerrainVP_, 1, GL_FALSE, vp.m);
        glUniform1f(uTerrainClip_, -2.35f);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texTerrainAlbedo_);
        glUniform1i(uTerrainAlbedo_, 3);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texTerrainNormal_);
        glUniform1i(uTerrainNormal_, 4);

        TerrainRenderer::Params tp;
        tp.vp = vp; tp.cam = cam.position; tp.viewDist = Q.viewDist; tp.lod0 = Q.terrLod0; tp.lod1 = Q.terrLod1; tp.propDist = Q.propDist;
        TerrainRenderer::Counts tc;
        for (auto& t : terr_) if (!t.empty()) t.drawTerrain(tp, tc);
        glEnable(GL_CULL_FACE);

        // 2. Objetos com cores de vértice (árvores, rochas, construções, coletáveis)
        glUseProgram(propProg_);
        applyLit(litProp_, cam.position);
        glUniformMatrix4fv(uPropVP_, 1, GL_FALSE, vp.m);
        glUniform1f(uPropClip_, -2.35f);
        glUniform1f(uPropTime_, nowSec);
        for (auto& t : terr_) if (!t.empty()) t.drawProps(tp, tc);
        tp.smallDist = Q.smallDist;
        glDisable(GL_CULL_FACE);
        for (auto& t : terr_) if (!t.empty()) t.drawSmall(tp, tc);   // gravetos e pedrinhas coletáveis
        drawGhosts();                 // árvores/pedras/toras sumindo
        drawDropProps();              // itens soltos com modelo (graveto, pedrinha, pedra, tora)
        glEnable(GL_CULL_FACE);

        // 3. Tufos de grama instanciados
        if (Q.grassDist > 0 && Q.grassDensity > 0) {
            glDisable(GL_CULL_FACE);
            glUseProgram(grassProg_);
            applyLit(litGrass_, cam.position);
            glUniformMatrix4fv(uGrassVP_, 1, GL_FALSE, vp.m);
            glUniform1f(uGrassClip_, -0.02f);
            glUniform1f(uGrassTime_, nowSec);
            glUniform1f(uGrassDist_, Q.grassDist);
            tp.grassDist = Q.grassDist; tp.grassDensity = Q.grassDensity;
            for (auto& t : terr_) if (!t.empty()) t.drawGrass(tp, tc);
            glEnable(GL_CULL_FACE);
        }
        gfx_.grassChunks = tc.grassChunks; gfx_.grassTufts = tc.grassTufts; gfx_.smallChunks = tc.smallChunks; gfx_.smallTris = tc.smallTris;
        gfx_.terrChunks = tc.chunks; gfx_.terrTris = tc.tris; gfx_.propChunks = tc.propChunks; gfx_.propTris = tc.propTris; gfx_.terrCulled = tc.culled;
        gfx_.draws += tc.chunks + tc.propChunks + tc.smallChunks + tc.grassChunks; gfx_.tris += tc.tris + tc.propTris + tc.smallTris + tc.grassTufts * 7;
    }

    for (int i = 0; i < kShips; ++i) drawShip(i, cam, vp, nowSec);
    drawIslandFlags(cam, vp, nowSec);
    drawStructs(cam, vp, nowSec);   // MELHORIAS DA ILHA: paliçadas, torres, marcenaria...

    glUseProgram(charProg_);
    applyLit(litChar_, cam.position);
    glUniform1f(uCharClip_, -2.35f);
    {
        // Planos do frustum da câmera para descartar personagens fora da tela
        struct Plane { float a, b, c, d; };
        Plane fp[5];
        const float* m = vp.m;
        const float raw[5][4] = {
            {m[3] + m[0], m[7] + m[4], m[11] + m[8],  m[15] + m[12]},
            {m[3] - m[0], m[7] - m[4], m[11] - m[8],  m[15] - m[12]},
            {m[3] + m[1], m[7] + m[5], m[11] + m[9],  m[15] + m[13]},
            {m[3] - m[1], m[7] - m[5], m[11] - m[9],  m[15] - m[13]},
            {m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]},
        };
        for (int p = 0; p < 5; ++p) {
            float il = 1.0f / std::max(1e-6f, std::sqrt(raw[p][0] * raw[p][0] + raw[p][1] * raw[p][1] + raw[p][2] * raw[p][2]));
            fp[p] = {raw[p][0] * il, raw[p][1] * il, raw[p][2] * il, raw[p][3] * il};
        }
        const bool crowd = chars.size() > 14;
        const float lodNear = crowd ? std::min(Q.lodNear, 4.8f) : Q.lodNear;
        const float lodFar  = crowd ? std::min(Q.lodFar, 13.0f) : Q.lodFar;
        int lod0Budget = crowd ? 2 : 12;                       // eu + o mais perto (chars vem ordenado por distância)
        int lod1Budget = crowd ? (chars.size() > 24 ? 6 : 10) : 28;
        const float maxCharDist = Q.viewDist + 40.0f;

        for (size_t ci = 0; ci < chars.size(); ++ci) {
            const CharInst& c = chars[ci];
            if (c.hidden) continue;
            float dx = c.pos.x - cam.position.x, dy = c.pos.y - cam.position.y, dz = c.pos.z - cam.position.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > maxCharDist * maxCharDist) continue;
            if (ci > 0 && d2 > 3.5f * 3.5f) {
                const float cx = c.pos.x, cy = c.pos.y + 0.9f, cz = c.pos.z, rad = 1.9f;
                if (fp[0].a * cx + fp[0].b * cy + fp[0].c * cz + fp[0].d < -rad ||
                    fp[1].a * cx + fp[1].b * cy + fp[1].c * cz + fp[1].d < -rad ||
                    fp[2].a * cx + fp[2].b * cy + fp[2].c * cz + fp[2].d < -rad ||
                    fp[3].a * cx + fp[3].b * cy + fp[3].c * cz + fp[3].d < -rad ||
                    fp[4].a * cx + fp[4].b * cy + fp[4].c * cz + fp[4].d < -rad) continue;
            }
            float d = std::sqrt(d2);
            int lod = d < lodNear ? 0 : (d < lodFar ? 1 : 2);
            if (lod == 0) {
                if (lod0Budget > 0) --lod0Budget;
                else lod = 1;
            }
            if (lod == 1) {
                if (lod1Budget > 0) --lod1Budget;
                else lod = 2;
            }
            ++gfx_.lod[lod];
            Mat4 model = charModel(c);  // rotY(-yaw): frente local -Z -> (sin yaw, 0, -cos yaw)
            Mat4 mvp = vp * model;
            useSkin(c.skin);
            drawMesh(lod, mvp, model, c.color, c.boneRows, false);
            if (d < 45.0f) drawTool(c.held, mvp, model, c.attachRows ? c.attachRows : c.boneRows, false);
        }
    }

    for (const DropVis& d : drops_) {   // itens soltos no chão: caixa (sem modelo) e ferramentas (.e0m)
        float dx = d.pos.x - cam.position.x, dz = d.pos.z - cam.position.z;
        if (dx * dx + dz * dz > 60.0f * 60.0f) continue;
        if (d.model == 5 || d.model == 6 || d.model == 9 || d.model == 10 || d.model == 11) {
            const int ti = d.model == 11 ? 4 : (d.model == 10 ? 3 : (d.model == 9 ? 2 : d.model - 5));   // 5 machadinha, 6 picareta, 9 tocha, 10 adaga, 11 bacamarte
            const Mesh& tm = toolMeshes_[ti];
            if (tm.subs.empty()) continue;
            // as malhas estão no espaço de bind da mão: recentra e deita (eixo maior na horizontal) com a base no chão
            Vec3 e = {tm.bmax.x - tm.bmin.x, tm.bmax.y - tm.bmin.y, tm.bmax.z - tm.bmin.z};
            Vec3 c0 = {(tm.bmin.x + tm.bmax.x) * 0.5f, (tm.bmin.y + tm.bmax.y) * 0.5f, (tm.bmin.z + tm.bmax.z) * 0.5f};
            bool vert = e.y > e.x && e.y > e.z;
            Mat4 model = Mat4::translate({d.pos.x, d.pos.y + (vert ? e.x : e.y) * 0.5f + 0.01f, d.pos.z}) * Mat4::rotY(d.yaw);
            if (vert) { Mat4 rz; float cz = 0.0f, sz = 1.0f; rz.m[0] = cz; rz.m[1] = sz; rz.m[4] = -sz; rz.m[5] = cz; model = model * rz; }
            model = model * Mat4::translate({-c0.x, -c0.y, -c0.z});
            static const Vec3 kMetal[5] = {{0.56f, 0.55f, 0.55f}, {0.52f, 0.52f, 0.54f}, {0.30f, 0.21f, 0.13f}, {0.42f, 0.43f, 0.44f}, {0.40f, 0.22f, 0.12f}};
            drawMeshRef(tm, vp * model, model, kMetal[ti], nullptr, false, 1.2f);
        } else if (d.model == 0) {
            Mat4 model = Mat4::translate(d.pos) * Mat4::rotY(d.yaw) * Mat4::scale(d.size);
            drawMeshRef(cubeMesh_, vp * model, model, d.color, nullptr, false);
        }
    }

    drawSky(cam, aspect);   // depois de todos os opacos e antes da água translúcida (que mistura sobre ele)

    glUseProgram(groundProg_);
    glUniform1f(uGroundTime_, nowSec);
    glUniform2f(uGroundOff_, cam.position.x, cam.position.z);
    applyLit(litGround_, cam.position);
    glUniformMatrix4fv(uGroundVP_, 1, GL_FALSE, vp.m);
    glUniform1f(uGroundGloss_, 0.9f);
    glUniform2f(uGroundScreenInv_, 1.0f / w_, 1.0f / h_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);   // o quad do mar tem winding voltado p/ baixo: sem isso ele some (culling)
    glBindVertexArray(groundVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    ++gfx_.draws; gfx_.tris += 2;

    drawSplashes(cam, vp);
    drawFlames(cam, vp);
    drawFx(cam, vp);

    drawStructGhost(vp);   // prévia (verde/vermelha) da estrutura sendo posicionada

    if (gradeK_ > 0.01f && gradeProg_) {   // filtro de cor pirata: multiplica a cena 3D (a HUD vem depois, sem filtro)
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
        glUseProgram(gradeProg_);
        glUniform1f(uGradeK_, gradeK_); glUniform1f(uGradeAspect_, (float)w_ / (float)std::max(1, h_));
        glBindVertexArray(uiVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND);
    }

    gpuTimerEnd();
}

void Renderer::burst(const Vec3& pos, int kind) {
    static unsigned seed = 987654u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8 & 0xFFFF) / 65535.0f; };
    auto add = [&](SplashParticle p) { if (splashes_.size() < 470) splashes_.push_back(p); };
    const int n = kind == 2 ? 10 : (kind == 1 ? 34 : 30);
    for (int i = 0; i < n; ++i) {
        const float ang = rnd() * 6.2831853f, out = kind == 2 ? 0.6f + rnd() * 1.6f : 0.6f + rnd() * (kind == 1 ? 5.0f : 3.2f);
        SplashParticle p; p.flat = false;
        p.pos = pos; p.pos.y = pos.y + 0.1f;
        p.vel = {std::cos(ang) * out, kind == 2 ? 0.8f + rnd() * 1.5f : 6.0f + rnd() * 9.0f, std::sin(ang) * out};
        p.size = kind == 2 ? 0.7f + rnd() * 0.8f : 0.55f + rnd() * 0.75f; p.age = 0; p.life = kind == 2 ? 0.9f : 1.9f; p.maxAlpha = kind == 2 ? 0.55f : 0.8f;
        add(p);
    }
    if (kind == 0) for (int i = 0; i < 4; ++i) {   // ondas que se abrem na água
        SplashParticle p; p.flat = true; p.pos = {pos.x, 0.012f, pos.z}; p.vel = {0, 0, 0};
        p.size = 1.2f + 1.1f * (float)i; p.age = -0.12f * (float)i; p.life = 1.8f; p.maxAlpha = 0.7f; add(p);
    }
}

void Renderer::updateSplashes(float nowSec, const std::vector<CharInst>& chars) {
    float dt = lastSplashTime_ < 0.0f ? 0.016f : std::max(0.0f, std::min(0.05f, nowSec - lastSplashTime_));
    lastSplashTime_ = nowSec;

    for (size_t i = 0; i < splashes_.size();) {
        SplashParticle& p = splashes_[i];
        p.age += dt;
        if (p.age >= p.life) {
            p = splashes_.back();
            splashes_.pop_back();
        } else {
            p.pos.x += p.vel.x * dt;
            p.pos.z += p.vel.z * dt;
            if (p.flat) {   // espuma: fica NA superfície da água, desliza devagar e vai parando
                p.pos.y = 0.012f;
                p.vel.x *= (1.0f - 2.2f * dt); p.vel.z *= (1.0f - 2.2f * dt);
                ++i;
            } else {        // gotícula: sobe com o respingo, cai com gravidade de verdade e SOME ao tocar a água (não fica flutuando)
                p.pos.y += p.vel.y * dt;
                p.vel.y -= 9.8f * dt;
                p.vel.x *= (1.0f - 0.8f * dt); p.vel.z *= (1.0f - 0.8f * dt);
                if (p.pos.y <= 0.0f && p.vel.y < 0.0f) { p = splashes_.back(); splashes_.pop_back(); }
                else ++i;
            }
        }
    }

    // Emissão: apenas para personagens nadando com velocidade perceptível (speed >= 0.35 m/s)
    // Parado/boiando: NÃO emite nada (sem rastro artificial)
    int nChars = std::min((int)chars.size(), e0net::kMaxPlayers + 1);
    const Vec3 refPos = !chars.empty() ? chars[0].pos : Vec3{};
    int swimmersEmitted = 0;
    for (int i = 0; i < nChars; ++i) {
        const CharInst& c = chars[i];
        if (c.pos.y >= -0.15f || c.speed < 0.35f) {
            splashTimer_[i] = 0.0f;
            continue;
        }
        float dx = c.pos.x - refPos.x, dz = c.pos.z - refPos.z;
        if (dx * dx + dz * dz > 55.0f * 55.0f || ++swimmersEmitted > 24) continue;

        splashTimer_[i] -= dt;
        if (splashTimer_[i] <= 0.0f) {
            splashTimer_[i] = std::max(0.05f, 0.16f / std::max(c.speed, 0.5f));   // um conjunto de discos por ~0,16 m percorrido

            if (splashes_.size() < 440) {
                float fwdX = -std::sin(c.yaw);
                float fwdZ = -std::cos(c.yaw);
                float sideX = std::cos(c.yaw);
                float sideZ = -std::sin(c.yaw);

                static unsigned sSeed = 1234567u;
                auto randf = [&]() { sSeed = sSeed * 1664525u + 1013904223u; return (float)(sSeed & 0xFFFF) / 65535.0f; };

                // RASTRO NA ÁGUA (esteira): discos de espuma achatados na superfície, sem gotas no ar. Um fio no centro atrás do corpo + duas linhas laterais que abrem em "V"
                // (ondas da proa); cada disco abre e some devagar, então o conjunto vira uma trilha que fica para trás enquanto você nada.
                auto foam = [&](float side, float back, float latVel, float size, float life, float alpha) {
                    if (splashes_.size() >= 440) return;
                    SplashParticle f; f.flat = true;
                    f.pos = {c.pos.x + sideX * side + fwdX * back, 0.012f, c.pos.z + sideZ * side + fwdZ * back};
                    f.vel = {sideX * latVel - fwdX * 0.10f, 0.0f, sideZ * latVel - fwdZ * 0.10f};
                    f.size = size; f.age = 0.0f; f.life = life; f.maxAlpha = alpha;
                    splashes_.push_back(f);
                };
                const float jit = (randf() - 0.5f) * 0.10f;
                foam(jit, -0.40f - randf() * 0.15f, 0.0f, 0.10f + randf() * 0.04f, 1.5f + randf() * 0.4f, 0.42f);            // centro
                foam(-0.22f + jit, -0.25f - randf() * 0.10f, -0.22f, 0.07f + randf() * 0.03f, 1.1f + randf() * 0.3f, 0.34f);  // linha esquerda do V
                foam(0.22f + jit, -0.25f - randf() * 0.10f, 0.22f, 0.07f + randf() * 0.03f, 1.1f + randf() * 0.3f, 0.34f);     // linha direita do V
            }
        }
    }

    // ESTEIRA DO NAVIO: espuma achatada na água na proa (onda de proa), nos costados e na popa; 1 conjunto a cada ~1,2 m navegados
    for (int si = 0; si < kShips; ++si) {
        const ShipDraw& ship = ships_[si];
        if (!ship.exists) continue;
        const e0net::ShipDef& D = e0net::shipDef(si);
        static float lxs[kShips], lzs[kShips], accs[kShips], spds[kShips]; static bool hads[kShips];
        float &lx = lxs[si], &lz = lzs[si], &acc = accs[si], &spd = spds[si]; bool& had = hads[si];
        float moved = std::sqrt((ship.x - lx) * (ship.x - lx) + (ship.z - lz) * (ship.z - lz));
        if (!had || moved > 30.0f) moved = 0.0f;
        had = true; lx = ship.x; lz = ship.z;
        spd += (moved / std::max(dt, 1e-3f) - spd) * 0.2f;
        acc += moved;
        if (spd > 0.8f && acc >= 1.2f) {
            acc = 0.0f;
            e0net::ShipState sp; sp.x = ship.x; sp.z = ship.z; sp.yaw = ship.yaw;
            static unsigned wSeed = 987654321u;
            auto rnd = [&]() { wSeed = wSeed * 1664525u + 1013904223u; return (float)(wSeed & 0xFFFF) / 65535.0f - 0.5f; };
            auto emit = [&](float a, float b, float latVel, float size, float life, float alpha) {
                if (splashes_.size() >= 470) return;
                SplashParticle f; f.flat = true;
                float wx, wz; e0net::shipLocalToWorld(sp, a * D.wakeA + rnd() * 0.4f, b * D.wakeB + rnd() * 0.3f, wx, wz);
                float vx, vz; e0net::shipLocalToWorld(sp, 0.0f, latVel, vx, vz); vx -= ship.x; vz -= ship.z;
                f.pos = {wx, 0.012f, wz}; f.vel = {vx, 0.0f, vz};
                f.size = size * D.wakeB; f.age = 0.0f; f.life = life; f.maxAlpha = alpha * std::min(1.0f, spd / 4.0f);
                splashes_.push_back(f);
            };
            emit(11.6f, -1.4f, -1.3f, 0.38f, 2.0f, 0.55f); emit(11.6f, 1.4f, 1.3f, 0.38f, 2.0f, 0.55f);       // onda de proa
            emit(8.0f, -3.2f, -0.6f, 0.30f, 1.7f, 0.42f); emit(8.0f, 3.2f, 0.6f, 0.30f, 1.7f, 0.42f);
            emit(0.0f, -3.9f, -0.5f, 0.30f, 1.6f, 0.38f); emit(0.0f, 3.9f, 0.5f, 0.30f, 1.6f, 0.38f);         // costados
            emit(-8.0f, -3.6f, -0.4f, 0.28f, 1.5f, 0.34f); emit(-8.0f, 3.6f, 0.4f, 0.28f, 1.5f, 0.34f);
            emit(-13.6f, 0.0f, 0.0f, 0.60f, 3.4f, 0.50f); emit(-14.0f, -1.6f, -0.35f, 0.45f, 3.0f, 0.42f); emit(-14.0f, 1.6f, 0.35f, 0.45f, 3.0f, 0.42f);   // esteira
        }
        if (ship.sinkT > 0.0f && ship.sinkT < e0net::kShipSinkTime + 2.0f) {   // NAUFRÁGIO: espuma/bolhas fervendo em volta do casco que afunda
            static unsigned bSeed = 13579u;
            auto rb = [&]() { bSeed = bSeed * 1664525u + 1013904223u; return (float)(bSeed & 0xFFFF) / 65535.0f; };
            e0net::ShipState sp; sp.x = ship.x; sp.z = ship.z; sp.yaw = ship.yaw;
            const int n = (int)(dt * 28.0f * (0.5f + D.wakeA)) + (rb() < dt * 28.0f ? 1 : 0);
            for (int k = 0; k < n && splashes_.size() < 470; ++k) {
                float a = D.hull[0][0] + (D.hull[8][0] - D.hull[0][0]) * rb(), hb = e0net::shipHullHalfB(D, a), b = (rb() * 2.0f - 1.0f) * (hb + 1.5f);
                float wx, wz; e0net::shipLocalToWorld(sp, a, b, wx, wz);
                SplashParticle f; f.flat = true;
                f.pos = {wx, 0.012f, wz}; f.vel = {(rb() - 0.5f) * 1.2f, 0.0f, (rb() - 0.5f) * 1.2f};
                f.size = (0.35f + rb() * 0.5f) * D.wakeB; f.age = 0.0f; f.life = 1.6f + rb() * 1.6f; f.maxAlpha = 0.6f;
                splashes_.push_back(f);
            }
        }
    }
}

// Tochas acesas (CharInst::held == 3): posição da chama = ponto de bind skinado pelo osso da mão (índice 34) -> mundo. Até 4, as mais
// próximas da câmera; w = intensidade com tremular (a soma de senos de fases diferentes por tocha evita que pisquem juntas).
void Renderer::updateTorches(const Camera& cam, const std::vector<CharInst>& chars) {
    static const auto t0 = std::chrono::steady_clock::now();
    const float t = clock_ >= 0.0 ? (float)clock_ : std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    for (int i = 0; i < 4; ++i) torch_[i] = Vec4{0, 0, 0, 0};
    nFlames_ = 0;
    float bestD[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    for (size_t ci = 0; ci < chars.size(); ++ci) {
        const CharInst& c = chars[ci];
        if (c.held != 3) continue;
        const Vec4* r = c.attachRows ? c.attachRows + 34 * 3 : (c.boneRows ? c.boneRows + 34 * 3 : nullptr);
        Vec3 p{kTorchFlame[0], kTorchFlame[1], kTorchFlame[2]};
        if (r) p = {r[0].x * p.x + r[0].y * p.y + r[0].z * p.z + r[0].w, r[1].x * p.x + r[1].y * p.y + r[1].z * p.z + r[1].w,
                    r[2].x * p.x + r[2].y * p.y + r[2].z * p.z + r[2].w};
        Mat4 ry = Mat4::rotY(-c.yaw);
        Vec3 w = c.pos + Vec3{ry.m[0] * p.x + ry.m[4] * p.y + ry.m[8] * p.z, ry.m[1] * p.x + ry.m[5] * p.y + ry.m[9] * p.z, ry.m[2] * p.x + ry.m[6] * p.y + ry.m[10] * p.z};
        float dx = w.x - cam.position.x, dy = w.y - cam.position.y, dz = w.z - cam.position.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 80.0f * 80.0f) continue;
        float ph = (float)ci * 1.7f;
        float fl = (0.45f + 0.55f * env_.night) * (0.88f + 0.07f * std::sin(t * 9.0f + ph) + 0.05f * std::sin(t * 17.3f + ph * 2.1f) + 0.04f * std::sin(t * 31.0f + ph * 0.6f));
        int slot = -1;                                             // ordena por distância (insere e empurra)
        for (int k = 0; k < 4; ++k) if (d2 < bestD[k]) { slot = k; break; }
        if (slot < 0) continue;
        for (int k = 3; k > slot; --k) { bestD[k] = bestD[k - 1]; torch_[k] = torch_[k - 1]; }
        bestD[slot] = d2; torch_[slot] = Vec4{w.x, w.y, w.z, fl};
    }
    for (int sl = 1; sl < e0net::kIslandSlots; ++sl)   // MELHORIAS DA ILHA: as FOGUEIRAS acendem como tochas (luz + chama)
        for (const e0net::IsleStruct& f : e0net::isleStructs(sl)) {
            if (f.kind != e0net::SkCampfire) continue;
            const Vec3 w{f.x, f.y + 0.42f, f.z};
            const float dx = w.x - cam.position.x, dy = w.y - cam.position.y, dz = w.z - cam.position.z, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 90.0f * 90.0f) continue;
            const float ph = f.x * 0.37f + f.z * 0.11f;
            const float fl = (0.35f + 0.95f * env_.night) * (0.9f + 0.08f * std::sin(t * 7.0f + ph) + 0.05f * std::sin(t * 13.1f + ph * 1.7f));
            int slot = -1;
            for (int k = 0; k < 4; ++k) if (d2 < bestD[k]) { slot = k; break; }
            if (slot < 0) continue;
            for (int k = 3; k > slot; --k) { bestD[k] = bestD[k - 1]; torch_[k] = torch_[k - 1]; }
            bestD[slot] = d2; torch_[slot] = Vec4{w.x, w.y, w.z, fl};
        }
    for (int k = 0; k < 4; ++k) if (torch_[k].w > 0.0f) ++nFlames_;
    if (lastNow_ - flashAt_ < 0.07f && lastNow_ >= flashAt_) {   // clarão do tiro: uma "tocha" fortíssima por 70 ms (ilumina chão e personagens em volta)
        const int k = std::min(nFlames_, 3);
        torch_[k] = Vec4{flashPos_.x, flashPos_.y, flashPos_.z, 2.6f * (1.0f - (lastNow_ - flashAt_) / 0.07f)};
        if (k == nFlames_) ++nFlames_;
    }
}

// Chama + halo (billboards aditivos, sem escrever profundidade; o halo some atrás do terreno pelo teste de profundidade)
// ---- BACAMARTE: efeitos ----
void Renderer::gunMuzzle(const CharInst& c, Vec3& pos, Vec3& dir) {
    Vec3 p{kGunMuzzle[0], kGunMuzzle[1], kGunMuzzle[2]}, d{kGunMuzzleDir[0], kGunMuzzleDir[1], kGunMuzzleDir[2]};
    if (c.boneRows) {
        const Vec4* r = (c.attachRows ? c.attachRows : c.boneRows) + 34 * 3;
        p = {r[0].x * p.x + r[0].y * p.y + r[0].z * p.z + r[0].w, r[1].x * p.x + r[1].y * p.y + r[1].z * p.z + r[1].w, r[2].x * p.x + r[2].y * p.y + r[2].z * p.z + r[2].w};
        d = {r[0].x * d.x + r[0].y * d.y + r[0].z * d.z, r[1].x * d.x + r[1].y * d.y + r[1].z * d.z, r[2].x * d.x + r[2].y * d.y + r[2].z * d.z};
    }
    Mat4 m = Mat4::translate(c.pos) * Mat4::rotY(-c.yaw);
    if (c.hasLocal) m = m * c.local;
    pos = {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12], m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13], m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
    dir = normalize(Vec3{m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z, m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z, m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z});
}

static float fxRnd() { static uint32_t s = 0x1234567u; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(s & 0xFFFFFF) / 16777216.0f; }

void Renderer::gunShot(const Vec3& m, const Vec3& dir) {
    auto add = [&](const FxPart& p) { if (fx_.size() < 600) fx_.push_back(p); };
    // clarão: 3 discos aditivos ao longo do cano (o maior na boca), vida curtíssima
    add({m + dir * 0.10f, dir * 1.0f, 0.42f, 1.5f, 0, 0.07f, 0, 0, 1.0f, 0.78f, 0.40f, 1.0f, true});
    add({m + dir * 0.38f, dir * 2.0f, 0.26f, 1.0f, 0, 0.06f, 0, 0, 1.0f, 0.62f, 0.22f, 0.9f, true});
    add({m + dir * 0.65f, dir * 3.0f, 0.14f, 0.5f, 0, 0.05f, 0, 0, 1.0f, 0.55f, 0.18f, 0.8f, true});
    for (int i = 0; i < 6; ++i)   // fagulhas
        add({m + dir * 0.15f, (dir * (5.0f + 6.0f * fxRnd()) + Vec3{fxRnd() - 0.5f, fxRnd() * 0.8f, fxRnd() - 0.5f} * 3.0f), 0.035f, 0.0f, 0, 0.18f + 0.15f * fxRnd(), 1.5f, 6.0f, 1.0f, 0.7f, 0.3f, 1.0f, true});
    // fumaça de pólvora negra: nuvem cinza que sai do cano, freia e sobe devagar
    for (int i = 0; i < 9; ++i) {
        float k = fxRnd();
        Vec3 v = dir * (1.0f + 6.0f * k) + Vec3{fxRnd() - 0.5f, 0.6f * fxRnd(), fxRnd() - 0.5f} * 1.2f;
        float c = 0.68f + 0.12f * fxRnd();
        add({m + dir * (0.1f + 0.2f * k), v, 0.12f + 0.08f * fxRnd(), 0.9f + 0.6f * fxRnd(), -0.02f * (float)i, 1.6f + 1.2f * fxRnd(), 2.6f, -0.35f, c, c * 0.98f, c * 0.95f, 0.42f, false});
    }
    flashAt_ = lastNow_; flashPos_ = m + dir * 0.3f;
}

void Renderer::gunTracer(const Vec3& a, const Vec3& b) {
    Vec3 d = b - a; float len = std::sqrt(dot(d, d));
    if (len < 0.3f || tracers_.size() >= 64) return;
    tracers_.push_back({a, b, 0.0f, len / 380.0f + 0.05f, len});
}

void Renderer::gunImpact(const Vec3& p, const Vec3& dir, int kind) {
    auto add = [&](const FxPart& q) { if (fx_.size() < 600) fx_.push_back(q); };
    if (kind == 1) {   // terra: poeira e torrões
        for (int i = 0; i < 7; ++i) {
            Vec3 v{(fxRnd() - 0.5f) * 1.6f, 0.8f + 1.8f * fxRnd(), (fxRnd() - 0.5f) * 1.6f};
            add({p, v - dir * 0.8f, 0.07f + 0.05f * fxRnd(), 0.8f, 0, 0.6f + 0.5f * fxRnd(), 2.0f, 2.0f, 0.55f, 0.46f, 0.33f, 0.55f, false});
        }
        for (int i = 0; i < 5; ++i)
            add({p, Vec3{(fxRnd() - 0.5f) * 2.5f, 1.5f + 2.5f * fxRnd(), (fxRnd() - 0.5f) * 2.5f}, 0.025f, 0.0f, 0, 0.5f, 0.3f, 9.8f, 0.30f, 0.24f, 0.17f, 0.95f, false});
    } else if (kind == 2) {   // água: respingo branco
        for (int i = 0; i < 9; ++i)
            add({Vec3{p.x, 0.02f, p.z}, Vec3{(fxRnd() - 0.5f) * 1.2f, 1.8f + 2.6f * fxRnd(), (fxRnd() - 0.5f) * 1.2f}, 0.05f + 0.04f * fxRnd(), 0.2f, 0, 0.55f + 0.2f * fxRnd(), 0.4f, 9.8f, 0.92f, 0.96f, 1.0f, 0.75f, false});
    } else if (kind == 3 || kind == 4) {   // sangue (cabeça: mais e maior)
        const int n = kind == 4 ? 22 : 9;
        for (int i = 0; i < n; ++i) {
            Vec3 v = dir * (1.0f + 2.5f * fxRnd()) + Vec3{fxRnd() - 0.5f, fxRnd() * 0.9f, fxRnd() - 0.5f} * (kind == 4 ? 3.2f : 2.0f);
            add({p, v, (kind == 4 ? 0.06f : 0.045f) + 0.03f * fxRnd(), 0.25f, 0, 0.35f + 0.35f * fxRnd(), 0.8f, 9.8f, 0.42f + 0.1f * fxRnd(), 0.02f, 0.02f, 0.92f, false});
        }
        add({p, dir * 0.4f, kind == 4 ? 0.30f : 0.18f, 1.2f, 0, 0.35f, 2.0f, 0.0f, 0.5f, 0.03f, 0.03f, 0.55f, false});   // névoa vermelha
    }
}

void Renderer::updateFx(float dt) {
    for (size_t i = 0; i < fx_.size();) {
        FxPart& p = fx_[i];
        p.age += dt;
        if (p.age >= p.life) { p = fx_.back(); fx_.pop_back(); continue; }
        if (p.age > 0.0f) {
            p.vel = p.vel * std::max(0.0f, 1.0f - p.drag * dt);
            p.vel.y -= p.grav * dt;
            p.pos = p.pos + p.vel * dt;
            p.size += p.grow * dt;
        }
        ++i;
    }
    for (size_t i = 0; i < tracers_.size();) {
        tracers_[i].age += dt;
        if (tracers_[i].age >= tracers_[i].life) { tracers_[i] = tracers_.back(); tracers_.pop_back(); } else ++i;
    }
}

void Renderer::drawFx(const Camera& cam, const Mat4& vp) {
    if ((fx_.empty() && tracers_.empty()) || !fxProg_) return;
    static std::vector<float> add, alp;
    add.clear(); alp.clear();
    const Vec3 r = cam.rightVec(), u = cam.upVec();
    auto quad = [](std::vector<float>& v, const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float u0, float u1, float rr, float g, float b, float a) {
        const float q[6][9] = {{p0.x, p0.y, p0.z, u0, 0, rr, g, b, a}, {p1.x, p1.y, p1.z, u1, 0, rr, g, b, a}, {p2.x, p2.y, p2.z, u1, 1, rr, g, b, a},
                               {p0.x, p0.y, p0.z, u0, 0, rr, g, b, a}, {p2.x, p2.y, p2.z, u1, 1, rr, g, b, a}, {p3.x, p3.y, p3.z, u0, 1, rr, g, b, a}};
        v.insert(v.end(), &q[0][0], &q[0][0] + 54);
    };
    for (const FxPart& p : fx_) {
        if (p.age < 0.0f) continue;
        const float t = p.age / p.life;
        float a = p.a * (p.add ? (1.0f - t) : (1.0f - t) * (1.0f - t) * std::min(1.0f, p.age * 12.0f + 0.3f));
        if (a <= 0.01f) continue;
        const Vec3 vr = r * p.size, vu = u * p.size;
        quad(p.add ? add : alp, p.pos - vr - vu, p.pos + vr - vu, p.pos + vr + vu, p.pos - vr + vu, 0.0f, 1.0f, p.r, p.g, p.b, a);
    }
    for (const FxTracer& tr : tracers_) {   // o chumbo voa a ~380 m/s: um traço de até 7 m que corre de a até b
        const float head = std::min(tr.len, tr.age * 380.0f), tail = std::max(0.0f, head - 7.0f);
        if (head - tail < 0.05f) continue;
        const Vec3 d = (tr.b - tr.a) * (1.0f / tr.len);
        const Vec3 A = tr.a + d * tail, B = tr.a + d * head, mid = (A + B) * 0.5f;
        Vec3 side = cross(d, cam.position - mid); float sl = std::sqrt(dot(side, side));
        if (sl < 1e-4f) continue;
        const float dist = std::sqrt(dot(cam.position - mid, cam.position - mid)), wdt = std::max(0.012f, dist * 0.0022f);
        side = side * (wdt / sl);
        const float fade = 1.0f - tr.age / tr.life;
        quad(add, A - side, B - side, B + side, A + side, 2.0f, 3.0f, 1.0f, 0.86f, 0.55f, 0.85f * fade + 0.15f);
    }
    const size_t cap = 640 * 6 * 9;
    if (add.size() > cap) add.resize(cap);
    if (alp.size() > cap) alp.resize(cap);
    glUseProgram(fxProg_);
    glUniformMatrix4fv(uFxVP_, 1, GL_FALSE, vp.m);
    glBindVertexArray(fxVao_);
    glBindBuffer(GL_ARRAY_BUFFER, fxVbo_);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    if (!alp.empty()) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBufferSubData(GL_ARRAY_BUFFER, 0, alp.size() * sizeof(float), alp.data());
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(alp.size() / 9));
        ++gfx_.draws; gfx_.tris += (int)(alp.size() / 27);
    }
    if (!add.empty()) {
        glBlendFunc(GL_ONE, GL_ONE);
        glBufferSubData(GL_ARRAY_BUFFER, 0, add.size() * sizeof(float), add.data());
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(add.size() / 9));
        ++gfx_.draws; gfx_.tris += (int)(add.size() / 27);
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void Renderer::drawFlames(const Camera& cam, const Mat4& vp) {
    if (nFlames_ == 0 || !flameProg_ || !glowProg_ || !splashVao_ || !splashVbo_) return;
    Vec3 r = cam.rightVec(), u = cam.upVec();
    const float t = lastNow_;
    float flame[4 * 36], glow[4 * 36];
    int n = 0;
    for (int k = 0; k < nFlames_; ++k) {
        Vec3 b{torch_[k].x, torch_[k].y, torch_[k].z};
        float a = std::min(1.0f, torch_[k].w);
        float hgt = 0.26f * (0.9f + 0.18f * std::sin(t * 11.0f + (float)k)), wid = 0.15f;
        // chama: base em b, ponta para CIMA do mundo (alinhada ao eixo y), largura voltada para a câmera
        Vec3 fl = normalize(Vec3{r.x, 0.0f, r.z});
        Vec3 p0 = b - fl * wid, p1 = b + fl * wid, p2 = p1 + Vec3{0, hgt, 0}, p3 = p0 + Vec3{0, hgt, 0};
        float fv[36] = {p0.x, p0.y, p0.z, 0, 0, a, p1.x, p1.y, p1.z, 1, 0, a, p2.x, p2.y, p2.z, 1, 1, a,
                        p0.x, p0.y, p0.z, 0, 0, a, p2.x, p2.y, p2.z, 1, 1, a, p3.x, p3.y, p3.z, 0, 1, a};
        std::memcpy(flame + n * 36, fv, sizeof fv);
        Vec3 c = b + Vec3{0, hgt * 0.35f, 0}; float gs = 0.55f;
        Vec3 g0 = c - r * gs - u * gs, g1 = c + r * gs - u * gs, g2 = c + r * gs + u * gs, g3 = c - r * gs + u * gs;
        float ga = 0.55f * a;
        float gv[36] = {g0.x, g0.y, g0.z, 0, 0, ga, g1.x, g1.y, g1.z, 1, 0, ga, g2.x, g2.y, g2.z, 1, 1, ga,
                        g0.x, g0.y, g0.z, 0, 0, ga, g2.x, g2.y, g2.z, 1, 1, ga, g3.x, g3.y, g3.z, 0, 1, ga};
        std::memcpy(glow + n * 36, gv, sizeof gv);
        ++n;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(splashVao_);
    glBindBuffer(GL_ARRAY_BUFFER, splashVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * 36 * sizeof(float), glow);
    glUseProgram(glowProg_); glUniformMatrix4fv(uGlowVP_, 1, GL_FALSE, vp.m);
    glDrawArrays(GL_TRIANGLES, 0, 6 * n);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * 36 * sizeof(float), flame);
    glUseProgram(flameProg_); glUniformMatrix4fv(uFlameVP_, 1, GL_FALSE, vp.m); glUniform1f(uFlameTime_, t);
    glDrawArrays(GL_TRIANGLES, 0, 6 * n);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    gfx_.draws += 2; gfx_.tris += 4 * n;
}

void Renderer::drawSplashes(const Camera& cam, const Mat4& vp) {
    if (splashes_.empty() || !splashProg_ || !splashVao_ || !splashVbo_) return;

    Vec3 r = cam.rightVec();
    Vec3 u = cam.upVec();

    static std::vector<float> vdata;
    vdata.clear();
    vdata.reserve(splashes_.size() * 36);

    for (const SplashParticle& p : splashes_) {
        float t = std::min(1.0f, p.age / std::max(p.life, 0.01f));
        float alpha = p.flat ? p.maxAlpha * (1.0f - t) * std::min(1.0f, p.age * 8.0f) : p.maxAlpha * (1.0f - 0.5f * t);
        if (alpha <= 0.01f) continue;
        float sz = p.flat ? p.size * (1.0f + 2.4f * t) : p.size;   // espuma abre como ondulação; gotícula mantém o tamanho

        Vec3 vr = p.flat ? Vec3{sz, 0.0f, 0.0f} : r * sz;
        Vec3 vu = p.flat ? Vec3{0.0f, 0.0f, sz} : u * sz;

        Vec3 p0 = p.pos - vr - vu;
        Vec3 p1 = p.pos + vr - vu;
        Vec3 p2 = p.pos + vr + vu;
        Vec3 p3 = p.pos - vr + vu;

        vdata.insert(vdata.end(), {p0.x, p0.y, p0.z, 0.0f, 0.0f, alpha});
        vdata.insert(vdata.end(), {p1.x, p1.y, p1.z, 1.0f, 0.0f, alpha});
        vdata.insert(vdata.end(), {p2.x, p2.y, p2.z, 1.0f, 1.0f, alpha});

        vdata.insert(vdata.end(), {p0.x, p0.y, p0.z, 0.0f, 0.0f, alpha});
        vdata.insert(vdata.end(), {p2.x, p2.y, p2.z, 1.0f, 1.0f, alpha});
        vdata.insert(vdata.end(), {p3.x, p3.y, p3.z, 0.0f, 1.0f, alpha});
    }

    if (vdata.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, splashVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vdata.size() * sizeof(float), vdata.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(splashProg_);
    glUniformMatrix4fv(uSplashVP_, 1, GL_FALSE, vp.m);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glBindVertexArray(splashVao_);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(vdata.size() / 6));
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    ++gfx_.draws;
    gfx_.tris += (int)(vdata.size() / 18);
}

void Renderer::drawWorldLine(const Camera& cam, const Vec3& base, const Vec3& top, float alpha, float widthPx) {
    if (!markerProg_ || alpha <= 0.01f) return;
    Vec3 mid = (base + top) * 0.5f;
    Vec3 toCam = cam.position - mid;
    float dist = std::sqrt(dot(toCam, toCam));
    float wm = dist * 2.0f * std::tan(cam.fovY * 0.5f) * widthPx / (float)h_;   // largura em metros p/ ocupar `widthPx` pixels
    Vec3 flat{toCam.x, 0.0f, toCam.z};
    float fl = std::sqrt(dot(flat, flat));
    Vec3 right = fl > 1e-4f ? normalize(cross(Vec3{0, 1, 0}, flat)) : Vec3{1, 0, 0};
    Vec3 hw = right * (wm * 0.5f);
    float a0 = alpha, a1 = alpha * 0.85f;
    const float v[6][4] = {
        {base.x - hw.x, base.y, base.z - hw.z, a0}, {base.x + hw.x, base.y, base.z + hw.z, a0}, {top.x + hw.x, top.y, top.z + hw.z, a1},
        {base.x - hw.x, base.y, base.z - hw.z, a0}, {top.x + hw.x, top.y, top.z + hw.z, a1}, {top.x - hw.x, top.y, top.z - hw.z, a1}};
    Mat4 vp = cam.projection((float)w_ / (float)h_) * cam.view();
    glUseProgram(markerProg_);
    glUniformMatrix4fv(uMarkerVP_, 1, GL_FALSE, vp.m);
    glBindVertexArray(markerVao_);
    glBindBuffer(GL_ARRAY_BUFFER, markerVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);   // lê a profundidade (some atrás do terreno) mas não escreve
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// Formato .e0m (E0M2 com pesos de skinning ou E0M1 legado; ver tools/export_mesh.py)
bool Renderer::loadMesh(const char* name, Mesh& out) {
    if (!assets_) return false;
    AAsset* a = AAssetManager_open(assets_, name, AASSET_MODE_BUFFER);
    if (!a) { LOG("asset %s não encontrado", name); return false; }
    const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a);
    size_t len = (size_t)AAsset_getLength(a), o = 0;
    auto need = [&](size_t n) { return o + n <= len; };
    bool v2 = need(8) && std::memcmp(b, "E0M2", 4) == 0;
    const bool v3 = need(8) && std::memcmp(b, "E0M3", 4) == 0;   // E0M3 = E0M2 + UV (2 floats) no fim de cada vértice: personagens com textura
    if (v3) v2 = true;
    bool v1 = !v2 && need(8) && std::memcmp(b, "E0M1", 4) == 0;
    bool ok = v1 || v2;
    size_t stride = v3 ? 40 : (v2 ? 32 : 24);
    uint32_t nSub = 0;
    if (ok) { std::memcpy(&nSub, b + 4, 4); o = 8; }
    size_t bestV = 0; int best = -1; long tris = 0;
    for (uint32_t i = 0; ok && i < nSub; ++i) {
        float col[3]; uint32_t nv, ni;
        if (!need(20)) { ok = false; break; }
        std::memcpy(col, b + o, 12); std::memcpy(&nv, b + o + 12, 4); std::memcpy(&ni, b + o + 16, 4); o += 20;
        size_t vb = (size_t)nv * stride, ib = (size_t)ni * 4;
        if (!need(vb + ib)) { ok = false; break; }
        Sub m;
        m.count = (int)ni; m.color = {col[0], col[1], col[2]};
        glGenVertexArrays(1, &m.vao); glGenBuffers(1, &m.vbo); glGenBuffers(1, &m.ibo);
        glBindVertexArray(m.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vb, b + o, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, nullptr);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void*)12);
        if (v2) {
            glEnableVertexAttribArray(2); glVertexAttribIPointer(2, 4, GL_UNSIGNED_BYTE, (GLsizei)stride, (void*)24);
            glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (void*)28);
        }
        if (v3) { glEnableVertexAttribArray(4); glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void*)32); }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ib, b + o + vb, GL_STATIC_DRAW);
        glBindVertexArray(0);
        o += vb + ib;
        tris += ni / 3;
        for (uint32_t k = 0; k < nv; ++k) {
            float p3[3]; std::memcpy(p3, b + o + (size_t)k * stride, 12);
            out.bmin = {std::min(out.bmin.x, p3[0]), std::min(out.bmin.y, p3[1]), std::min(out.bmin.z, p3[2])};
            out.bmax = {std::max(out.bmax.x, p3[0]), std::max(out.bmax.y, p3[1]), std::max(out.bmax.z, p3[2])};
        }
        if (nv > bestV) { bestV = nv; best = (int)out.subs.size(); }  // submalha com mais vértices = corpo (recebe a cor do jogador)
        out.subs.push_back(m);
    }
    AAsset_close(a);
    if (!ok) { LOG("%s inválido", name); return false; }
    if (best >= 0) out.subs[best].body = true;
    LOG("%s (%s) carregado: %u submalhas, %ld triângulos", name, v3 ? "E0M3" : (v2 ? "E0M2" : "E0M1"), nSub, tris);
    return true;
}

// Atlas de ícones: 'E0I1' + w u16 + h u16 + RGBA8 (linhas de cima para baixo), gerado por tools/make_icons.py
bool Renderer::loadIcons() {
    if (!assets_) return false;
    AAsset* a = AAssetManager_open(assets_, "icons.e0i", AASSET_MODE_BUFFER);
    if (!a) return false;
    const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a); size_t len = (size_t)AAsset_getLength(a);
    bool ok = len >= 8 && std::memcmp(b, "E0I1", 4) == 0;
    int w = 0, h = 0;
    if (ok) { w = b[4] | (b[5] << 8); h = b[6] | (b[7] << 8); ok = w > 0 && h > 0 && len >= 8 + (size_t)w * h * 4; }
    if (ok) {
        if (texIcons_) glDeleteTextures(1, &texIcons_);
        glGenTextures(1, &texIcons_);
        glBindTexture(GL_TEXTURE_2D, texIcons_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b + 8);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        iconRows_ = std::max(1, h / 128);
        LOG("icons.e0i carregado: %dx%d (%d linhas)", w, h, iconRows_);
    }
    AAsset_close(a);
    return ok;
}

void Renderer::uiCornerGlow(float cx, float cy, float half, float dirx, float diry, float red, float green, float blue, float alpha) {
    glUniform4f(uUiRect_, cx, cy, half, half);
    glUniform4f(uUiUv_, dirx, diry, 0.0f, 0.0f);   // direção do canto (±1, ±1) em relação ao centro do quadrado
    glUniform4f(uUiColor_, red, green, blue, alpha);
    glUniform1i(uUiMode_, 6);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiArc(float cx, float cy, float r, float thickness, float frac, float red, float green, float blue, float alpha) {
    glUniform4f(uUiRect_, cx, cy, r, r);
    glUniform4f(uUiUv_, frac, 0.0f, 0.0f, 0.0f);
    glUniform1f(uUiThickness_, thickness);
    glUniform4f(uUiColor_, red, green, blue, alpha);
    glUniform1i(uUiMode_, 9);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiGrad(float cx, float cy, float halfW, float halfH, float radius, float r0, float g0, float b0, float a0,
                      float r1, float g1, float b1, float a1, float dirx, float diry, float grunge, float seed) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiRadius_, radius);
    glUniform4f(uUiColor_, r0, g0, b0, a0);
    glUniform4f(uUiSheet_, r1, g1, b1, a1);   // modo 10 reaproveita uSheet como 2ª cor (o modo 7 sempre o redefine)
    glUniform4f(uUiUv_, dirx, diry, grunge, seed);
    glUniform1i(uUiMode_, 10);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiCompass(float cx, float cy, float r, float alpha) {   // rosa dos ventos procedural (modo 12)
    glUniform4f(uUiRect_, cx, cy, r, r);
    glUniform4f(uUiColor_, 1.0f, 1.0f, 1.0f, alpha);
    glUniform1i(uUiMode_, 12);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiGlow(float cx, float cy, float halfW, float halfH, float red, float green, float blue, float alpha) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform4f(uUiColor_, red, green, blue, alpha);
    glUniform1i(uUiMode_, 11);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiWoodPanel(float cx, float cy, float halfW, float halfH, float radius, int style, float alpha, float seed) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiRadius_, radius);
    glUniform1f(uUiThickness_, (float)style);
    glUniform4f(uUiColor_, 1.0f, 1.0f, 1.0f, alpha);
    glUniform4f(uUiUv_, seed, 0.0f, 0.0f, 0.0f);
    glUniform1i(uUiMode_, 13);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiButton(float cx, float cy, float halfW, float halfH, float radius, int style, float alpha) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiRadius_, radius);
    glUniform1f(uUiThickness_, (float)style);
    glUniform4f(uUiColor_, 1.0f, 1.0f, 1.0f, alpha);
    glUniform1i(uUiMode_, 14);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiCloseBtn(float cx, float cy, float r, float alpha) {
    glUniform4f(uUiRect_, cx, cy, r, r);
    glUniform4f(uUiColor_, 1.0f, 1.0f, 1.0f, alpha);
    glUniform1i(uUiMode_, 15);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiDivider(float cx, float cy, float halfW, float halfH, float alpha) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform4f(uUiColor_, 0.88f, 0.70f, 0.34f, alpha);
    glUniform1i(uUiMode_, 16);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiIcon(int index, float cx, float cy, float half, float alpha) { uiIconTint(index, cx, cy, half, 1.0f, 1.0f, 1.0f, alpha); }

void Renderer::uiIconTint(int index, float cx, float cy, float half, float r, float g, float b, float alpha) {
    if (!texIcons_ || index < 0 || index >= 4 * iconRows_) return;
    const float cu = 1.0f / 4.0f, cv = 1.0f / (float)iconRows_;
    float u0 = (float)(index % 4) * cu, v0 = (float)(index / 4) * cv;
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, texIcons_);
    glUniform1i(uUiIcon_, 7);
    glActiveTexture(GL_TEXTURE0);
    glUniform4f(uUiRect_, cx, cy, half, half);
    glUniform4f(uUiUv_, u0, v0, u0 + cu, v0 + cv);
    glUniform4f(uUiColor_, r, g, b, alpha);
    glUniform1i(uUiMode_, 5);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiFlag(const FlagLook& f, float cx, float cy, float halfW, float halfH, float time, float wave, float alpha) {
    if (!flagProg_ || halfW < 1.0f || halfH < 1.0f) return;
    glUseProgram(flagProg_);
    glUniform2f(uFlagScreen_, (float)w_, (float)h_);
    const float padY = halfH * (0.08f + 0.16f * wave), padX = 2.0f;   // folga para a onda e o antisserrilhado
    glUniform4f(uFlagRect_, cx, cy, halfW + padX, halfH + padY);
    glUniform2f(uFlagCloth_, halfW, halfH);
    glUniform3f(uFlagC0_, f.c0.x, f.c0.y, f.c0.z);
    glUniform3f(uFlagC1_, f.c1.x, f.c1.y, f.c1.z);
    glUniform3f(uFlagC2_, f.c2.x, f.c2.y, f.c2.z);
    const bool icon = texIcons_ && f.icon >= 0 && f.icon < 4 * iconRows_;
    glUniform4i(uFlagStyle_, f.pattern, f.edge, f.wear, (f.mirror ? 1 : 0) | (f.outline ? 2 : 0) | (icon ? 4 : 0));
    if (icon) {
        const float cu = 1.0f / 4.0f, cv = 1.0f / (float)iconRows_;
        const float u0 = (float)(f.icon % 4) * cu, v0 = (float)(f.icon / 4) * cv;
        glUniform4f(uFlagIconUv_, u0, v0, u0 + cu, v0 + cv);
        glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, texIcons_); glActiveTexture(GL_TEXTURE0);
    }
    glUniform1i(uFlagIcon_, 7);
    glUniform4f(uFlagIconT_, f.iconX, f.iconY, f.iconSize, f.seed);
    glUniform3f(uFlagWave_, time, wave, alpha);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(uiProg_);
}

void Renderer::mapUpload(int slot, int res, const uint8_t* rgba) {
    if (slot < 0 || slot > 3 || !rgba) return;
    glActiveTexture(GL_TEXTURE6);
    if (!texMap_[slot]) {
        glGenTextures(1, &texMap_[slot]);
        glBindTexture(GL_TEXTURE_2D, texMap_[slot]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        mapRes_[slot] = 0;
    }
    glBindTexture(GL_TEXTURE_2D, texMap_[slot]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (mapRes_[slot] != res) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, res, res, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba); mapRes_[slot] = res; }
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, res, res, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::uiMap(int slot, float cx, float cy, float halfW, float halfH, bool circle, float alpha, float u0, float v0, float u1, float v1) {
    if (slot < 0 || slot > 3 || !texMap_[slot]) return;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, texMap_[slot]);
    glUniform1i(uUiIcon_, 6);
    glActiveTexture(GL_TEXTURE0);
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiThickness_, circle ? 1.0f : 0.0f);
    glUniform4f(uUiUv_, u0, v0, u1, v1);
    glUniform4f(uUiColor_, 1.0f, 1.0f, 1.0f, alpha);
    glUniform1i(uUiMode_, 7);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiMapSheet(int slot, bool paper, float cx, float cy, float halfW, float halfH, float sheetCx, float sheetCy, float sheetR,
                          float gridCx, float gridCz, float mPerPx, float gridStep, float u0, float v0, float u1, float v1, float sheetH) {
    if (slot < 0 || slot > 3 || !texMap_[slot]) return;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, texMap_[slot]);
    glUniform1i(uUiIcon_, 6);
    glActiveTexture(GL_TEXTURE0);
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiThickness_, 3.0f);
    glUniform4f(uUiUv_, u0, v0, u1, v1);
    glUniform4f(uUiColor_, 0.0f, 0.0f, paper ? 1.0f : 0.0f, 1.0f);
    glUniform4f(uUiSheet_, sheetCx, sheetCy, sheetR, sheetH);   // sheetH > 0: folha retangular (sheetR = meia-largura)
    glUniform4f(uUiGrid_, gridCx, gridCz, mPerPx, gridStep);
    glUniform1i(uUiMode_, 7);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiMapTilt(int slot, float cx, float cy, float R, float alpha, float uc, float vc, float scale, float yaw, float gridX, float gridZ, bool grid) {
    if (slot < 0 || slot > 3 || !texMap_[slot]) return;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, texMap_[slot]);
    glUniform1i(uUiIcon_, 6);
    glActiveTexture(GL_TEXTURE0);
    glUniform4f(uUiRect_, cx, cy, R, R);
    glUniform1f(uUiThickness_, 2.0f);
    glUniform4f(uUiUv_, uc, vc, scale, yaw);
    glUniform4f(uUiColor_, gridX, gridZ, grid ? 1.0f : 0.0f, alpha);
    glUniform1i(uUiMode_, 7);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiMarker(int kind, float cx, float cy, float r, float yaw) {
    glUniform4f(uUiRect_, cx, cy, r, r);
    glUniform1f(uUiThickness_, (float)kind);
    glUniform4f(uUiUv_, yaw, 0.0f, 0.0f, 0.0f);
    if (kind == 0) glUniform4f(uUiColor_, 0.62f, 0.09f, 0.07f, 1.0f);
    else if (kind == 1) glUniform4f(uUiColor_, 0.12f, 0.22f, 0.40f, 1.0f);
    else if (kind == 3) glUniform4f(uUiColor_, 0.10f, 0.55f, 0.18f, 1.0f);   // tripulante (verde)
    else glUniform4f(uUiColor_, 0.43f, 0.24f, 0.08f, 1.0f);
    glUniform1i(uUiMode_, 8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::beginUi() {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(uiProg_);
    glUniform2f(uUiScreen_, (float)w_, (float)h_);
    glBindVertexArray(uiVao_);
}

void Renderer::uiShape(UiShape s, float cx, float cy, float r, float red, float green, float blue, float alpha) {
    glUniform4f(uUiRect_, cx, cy, r, r);
    glUniform4f(uUiColor_, red, green, blue, alpha);
    glUniform1i(uUiMode_, (int)s);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiRoundRect(float cx, float cy, float halfW, float halfH, float radius,
                          float red, float green, float blue, float alpha) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiRadius_, radius);
    glUniform4f(uUiColor_, red, green, blue, alpha);
    glUniform1i(uUiMode_, 3 /*UiRoundRectFill*/);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::uiRoundRectBorder(float cx, float cy, float halfW, float halfH, float radius, float thickness,
                                float red, float green, float blue, float alpha) {
    glUniform4f(uUiRect_, cx, cy, halfW, halfH);
    glUniform1f(uUiRadius_, radius);
    glUniform1f(uUiThickness_, thickness);
    glUniform4f(uUiColor_, red, green, blue, alpha);
    glUniform1i(uUiMode_, 4 /*UiRoundRectBorder*/);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::endUi() {
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ---- texto ----
void Renderer::quadUv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, float r, float g, float b, float a) {
    const float x1 = x + w, y1 = y + h;
    const float q[6][8] = {{x, y, u0, v0, r, g, b, a},   {x1, y, u1, v0, r, g, b, a},  {x, y1, u0, v1, r, g, b, a},
                           {x1, y, u1, v0, r, g, b, a},  {x1, y1, u1, v1, r, g, b, a}, {x, y1, u0, v1, r, g, b, a}};
    tbatch_.insert(tbatch_.end(), &q[0][0], &q[0][0] + 48);
}

void Renderer::quad(float x, float y, float w, float h, int cell, float r, float g, float b, float a) {
    float cx = (float)((cell % 16) * 8), cy = (float)((cell / 16) * 8);
    float u0 = (cx + 1) / kAtlasW, u1 = (cx + 6) / kAtlasW, v0 = cy / kAtlasH, v1 = (cy + 7) / kAtlasH;
    quadUv(x, y, w, h, u0, v0, u1, v1, r, g, b, a);
}

float Renderer::monoTextWidth(const char* s, int scale) {
    int n = (int)std::strlen(s);
    return n > 0 ? (float)((n * 6 - 1) * scale) : 0.0f;
}

float Renderer::textWidth(const char* s, int scale) {
    if (!s || !*s) return 0.0f;
    float w = 0.0f;
    for (const unsigned char* c = (const unsigned char*)s; *c; ++c) w += kUiGlyphs[*c].adv;
    return w * (float)scale;
}

void Renderer::beginText() { tbatch_.clear(); textMono_ = (uiFontTex_ == 0); }
void Renderer::beginMonoText() { tbatch_.clear(); textMono_ = true; }

void Renderer::rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    if (textMono_) quad(x, y, w, h, kSolidCell, r, g, b, a);
    else quadUv(x, y, w, h, kUiSolidU, kUiSolidV, kUiSolidU, kUiSolidV, r, g, b, a);
}

void Renderer::monoRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    quad(x, y, w, h, kSolidCell, r, g, b, a);
}

void Renderer::monoText(float x, float y, int scale, const char* s, float r, float g, float b, float a, bool shadow) {
    auto draw = [&](float ox, float oy, float cr, float cg, float cb, float ca) {
        float px = x + ox;
        for (const char* c = s; *c; ++c) {
            int ch = (unsigned char)*c, mark = 0;
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            else if (ch >= 0xC0) {
                static const char kBase[] = "AAAAAAACEEEEIIIIDNOOOOO?OUUUUYPS";
                static const uint8_t kMark[32] = {1, 2, 3, 4, 5, 0, 0, 6, 1, 2, 3, 5, 1, 2, 3, 5, 0, 4, 1, 2, 3, 4, 5, 0, 0, 1, 2, 3, 5, 2, 0, 0};
                if (ch == 0xFF) { ch = 'Y'; mark = 5; }
                else { if (ch >= 0xE0 && ch != 0xF7) ch -= 0x20; if (ch >= 0xC0 && ch <= 0xDF) { mark = kMark[ch - 0xC0]; ch = kBase[ch - 0xC0]; } }
            }
            if (ch < 32 || ch > 95) ch = '?';
            if (mark) {
                const float fy = mark == 6 ? y + oy + 7.0f * scale : y + oy - 7.0f * scale;
                quad(std::floor(px), std::floor(fy), 5.0f * scale, 7.0f * scale, 64 + mark, cr, cg, cb, ca);
            }
            if (ch != ' ') quad(std::floor(px), std::floor(y + oy), 5.0f * scale, 7.0f * scale, ch - 32, cr, cg, cb, ca);
            px += 6.0f * scale;
        }
    };
    if (shadow) draw((float)scale, (float)scale, 0, 0, 0, a * 0.85f);
    draw(0, 0, r, g, b, a);
}

void Renderer::text(float x, float y, int scale, const char* s, float r, float g, float b, float a, bool shadow) {
    if (!s || !*s) return;
    if (textMono_) { monoText(x, y, scale, s, r, g, b, a, shadow); return; }
    const float sc = (float)scale;
    auto draw = [&](float ox, float oy, float cr, float cg, float cb, float ca) {
        float px = x + ox;
        for (const unsigned char* c = (const unsigned char*)s; *c; ++c) {
            const UiGlyph& g = kUiGlyphs[*c];
            if (g.w > 0.0f) quadUv(px + g.xo * sc, y + oy + g.yo * sc, g.w * sc, g.h * sc, g.u0, g.v0, g.u1, g.v1, cr, cg, cb, ca);
            px += g.adv * sc;
        }
    };
    if (shadow) draw(std::max(1.2f, sc * 0.42f), std::max(1.4f, sc * 0.52f), 0.04f, 0.02f, 0.01f, a * 0.78f);
    draw(0.0f, 0.0f, r, g, b, a);
}

void Renderer::endText() {
    if (tbatch_.empty() || !textProg_) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(textProg_);
    glUniform2f(uTextScreen_, (float)w_, (float)h_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textMono_ ? fontTex_ : uiFontTex_);
    glUniform1i(uTextTex_, 0);
    glBindVertexArray(textVao_);
    glBindBuffer(GL_ARRAY_BUFFER, textVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tbatch_.size() * sizeof(float)), tbatch_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(tbatch_.size() / 8));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::endMonoText() { endText(); }

}  // namespace e0
