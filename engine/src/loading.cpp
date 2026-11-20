#include "loading.h"
#include "e0i.h"

#include <GLES3/gl3.h>
#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ui_font.h"
#include "font5x7.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated", __VA_ARGS__)

namespace e0 {

static const char* kVS = R"(#version 300 es
uniform vec4 uRect;      // x, y, w, h em pixels (canto superior esquerdo)
uniform vec2 uScreen;
out vec2 vP;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vP = c;
    vec2 px = uRect.xy + c * uRect.zw;
    gl_Position = vec4(px.x / uScreen.x * 2.0 - 1.0, 1.0 - px.y / uScreen.y * 2.0, 0.0, 1.0);
})";

static const char* kFS = R"(#version 300 es
precision mediump float;
in vec2 vP;
uniform int uMode;        // 0 imagem (cobre a tela), 1 círculo indeterminado, 2 texto (textura RGBA), 3 cor sólida com cantos arredondados
uniform vec4 uColor;
uniform vec4 uUv;         // modo 0: u0 v0 u1 v1 do recorte; modo 3: raio do canto em fração da altura
uniform float uTime;
uniform sampler2D uTex;
out vec4 o;
void main() {
    if (uMode == 0) {
        vec2 uv = mix(uUv.xy, uUv.zw, vP);
        vec3 c = texture(uTex, uv).rgb;
        float v = smoothstep(1.25, 0.35, length((vP - 0.5) * vec2(1.6, 1.0)));      // vinheta leve
        o = vec4(c * mix(0.72, 1.0, v), uColor.a);
    } else if (uMode == 1) {
        vec2 p = (vP - 0.5) * 2.0;
        float r = length(p);
        float ring = smoothstep(0.62, 0.68, r) * (1.0 - smoothstep(0.92, 0.98, r));
        float a = atan(p.y, p.x);                                                     // -pi..pi
        float start = uTime * 4.2;
        float sweep = 0.8 + 3.2 * (0.5 + 0.5 * sin(uTime * 2.4));                     // o arco cresce e encolhe
        float d = mod(a - start + 6.2831853 * 4.0, 6.2831853);                        // distância angular desde a cauda
        float inArc = smoothstep(0.0, 0.12, d) * (1.0 - smoothstep(sweep - 0.12, sweep, d));
        float k = ring * (0.22 + 0.78 * inArc);
        o = vec4(uColor.rgb, uColor.a * k);
    } else if (uMode == 2) {
        float a = texture(uTex, mix(uUv.xy, uUv.zw, vP)).r;
        o = vec4(uColor.rgb, uColor.a * a);
    } else {
        // modo 3: painel rústico carvalho escuro com borda fina de latão (uUv = w, h, radius, border)
        vec2 sz = uUv.xy * 0.5;
        vec2 q = abs((vP - 0.5) * uUv.xy) - sz + vec2(uUv.z);
        float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - uUv.z;
        float aa = fwidth(d) + 1e-4;
        float fill = 1.0 - smoothstep(-aa, aa, d);
        float brd = uUv.w > 0.0 ? fill * smoothstep(-uUv.w - aa, -uUv.w + aa, d) : 0.0;
        vec3 bg = mix(vec3(0.12, 0.09, 0.065), vec3(0.06, 0.045, 0.032), vP.y);
        vec3 col = mix(bg, vec3(0.84, 0.66, 0.32), brd * 0.90);
        o = vec4(col, uColor.a * fill);
    }
})";

static unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[512]; glGetShaderInfoLog(s, sizeof b, nullptr, b); LOG("loading: shader falhou: %s", b); glDeleteShader(s); return 0; }
    return s;
}

bool LoadingScreen::init(AAssetManager* am) {
    unsigned vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    prog_ = glCreateProgram(); glAttachShader(prog_, vs); glAttachShader(prog_, fs); glLinkProgram(prog_);
    glDeleteShader(vs); glDeleteShader(fs);
    int ok = 0; glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) { LOG("loading: link falhou"); glDeleteProgram(prog_); prog_ = 0; return false; }
    uRect_ = glGetUniformLocation(prog_, "uRect"); uScreen_ = glGetUniformLocation(prog_, "uScreen"); uMode_ = glGetUniformLocation(prog_, "uMode");
    uColor_ = glGetUniformLocation(prog_, "uColor"); uTime_ = glGetUniformLocation(prog_, "uTime"); uUv_ = glGetUniformLocation(prog_, "uUv");
    glGenVertexArrays(1, &vao_);
    if (am) {   // arte de fundo (E0I1: w,h u16 + RGBA8); sem ela fica um fundo azul-escuro liso
        if (AAsset* a = AAssetManager_open(am, "loading.e0i", AASSET_MODE_BUFFER)) {
            const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a); size_t len = (size_t)AAsset_getLength(a);
            texImg_ = uploadE0I(b, len, &imgW_, &imgH_, false);   // E0I1 ou E0I2 (ETC2)
            if (!texImg_) imgW_ = imgH_ = 0;
            AAsset_close(a);
        } else LOG("loading: loading.e0i nao encontrado");
    }
    glGenTextures(1, &texTxt_);
    txtScale_ = 0;   // >0 indica que ui_font.e0i foi carregada em texTxt_
    if (am) {
        if (AAsset* fa = AAssetManager_open(am, "ui_font.e0i", AASSET_MODE_BUFFER)) {
            const uint8_t* b = (const uint8_t*)AAsset_getBuffer(fa); size_t len = (size_t)AAsset_getLength(fa);
            if (len >= 8 && std::memcmp(b, "E0F1", 4) == 0) {
                int fw = b[4] | (b[5] << 8), fh = b[6] | (b[7] << 8);
                if (fw == kUiFontW && fh == kUiFontH && len >= 8 + (size_t)fw * fh) {
                    glBindTexture(GL_TEXTURE_2D, texTxt_);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, fw, fh, 0, GL_RED, GL_UNSIGNED_BYTE, b + 8);
                    glGenerateMipmap(GL_TEXTURE_2D);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    txtScale_ = 1;
                }
            }
            AAsset_close(fa);
        }
    }
    return true;
}

void LoadingScreen::shutdown() {
    if (texImg_) glDeleteTextures(1, &texImg_);
    if (texTxt_) glDeleteTextures(1, &texTxt_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    texImg_ = texTxt_ = vao_ = prog_ = 0; txt_.clear();
}

// Rasteriza o texto (fonte 5x7 ampliada) numa textura RGBA (só o alfa importa). Contorno escuro de 1 "pixel de fonte" já embutido não é preciso: o painel escuro atrás garante a leitura.
void LoadingScreen::buildText(const char* s, int scale) {
    const int n = (int)std::strlen(s), adv = 6 * scale;
    txtW_ = std::max(1, n * adv); txtH_ = 7 * scale; txtScale_ = scale;
    std::vector<uint8_t> px((size_t)txtW_ * txtH_ * 4, 0);
    for (int i = 0; i < n; ++i) {
        char c = (char)std::toupper((unsigned char)s[i]);
        for (const Glyph& g : kFont) {
            if (g.c != c) continue;
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    if (g.r[y] & (1 << (4 - x)))
                        for (int dy = 0; dy < scale; ++dy)
                            for (int dx = 0; dx < scale; ++dx) {
                                size_t o = (((size_t)(y * scale + dy)) * txtW_ + (size_t)(i * adv + x * scale + dx)) * 4;
                                px[o] = px[o + 1] = px[o + 2] = 255; px[o + 3] = 255;
                            }
            break;
        }
    }
    glBindTexture(GL_TEXTURE_2D, texTxt_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, txtW_, txtH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    txt_ = s;
}

void LoadingScreen::draw(int w, int h, const char* stage, float t, float alpha) {
    if (!prog_ || w <= 0 || h <= 0 || alpha <= 0.002f) return;
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, w, h);
    glUseProgram(prog_); glBindVertexArray(vao_);
    glUniform2f(uScreen_, (float)w, (float)h); glUniform1f(uTime_, t);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    auto quad = [&](float x, float y, float qw, float qh) { glUniform4f(uRect_, x, y, qw, qh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); };

    // 1) arte de fundo, "cover" (preenche a tela cortando o excesso, centralizado)
    if (texImg_) {
        float sa = (float)w / (float)h, ia = (float)imgW_ / (float)imgH_, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (sa > ia) { float f = ia / sa; v0 = (1 - f) * 0.5f; v1 = 1 - v0; } else { float f = sa / ia; u0 = (1 - f) * 0.5f; u1 = 1 - u0; }
        glBindTexture(GL_TEXTURE_2D, texImg_);
        glUniform1i(uMode_, 0); glUniform4f(uColor_, 1, 1, 1, alpha); glUniform4f(uUv_, u0, v0, u1, v1);
        quad(0, 0, (float)w, (float)h);
    } else {
        glUniform1i(uMode_, 3); glUniform4f(uColor_, 1, 1, 1, alpha); glUniform4f(uUv_, (float)w, (float)h, 0.0f, 0.0f);
        quad(0, 0, (float)w, (float)h);
    }

    // 2) canto inferior direito: placa de carvalho + latão com [texto da etapa em Cinzel] [anel de bússola]
    const float u = (float)h / 1080.0f;
    const float sc = std::max(2.4f, 4.2f * u);
    float tw = 0.0f;
    for (const unsigned char* c = (const unsigned char*)stage; *c; ++c) tw += kUiGlyphs[*c].adv * sc;
    const float th = 7.0f * sc;
    const float margin = 44.0f * u, d = 58.0f * u, padX = 28.0f * u, padY = 18.0f * u, gap = 24.0f * u;
    const float panelH = std::max(d, th) + 2.0f * padY, panelW = padX * 2.0f + tw + gap + d;
    const float px = w - margin - panelW, py = h - margin - panelH;
    // painel carvalho + moldura latão
    glUniform1i(uMode_, 3); glUniform4f(uColor_, 1, 1, 1, 0.88f * alpha); glUniform4f(uUv_, panelW, panelH, 14.0f * u, 2.2f * u);
    quad(px, py, panelW, panelH);
    // texto em Cinzel
    if (txtScale_ > 0) {
        glBindTexture(GL_TEXTURE_2D, texTxt_);
        glUniform1i(uMode_, 2);
        const float tx0 = px + padX, ty0 = py + (panelH - th) * 0.5f;
        auto drawStr = [&](float ox, float oy, float r, float g, float b, float a) {
            glUniform4f(uColor_, r, g, b, a);
            float cx = tx0 + ox;
            for (const unsigned char* c = (const unsigned char*)stage; *c; ++c) {
                const UiGlyph& g0 = kUiGlyphs[*c];
                if (g0.w > 0.0f) {
                    glUniform4f(uUv_, g0.u0, g0.v0, g0.u1, g0.v1);
                    quad(cx + g0.xo * sc, ty0 + oy + g0.yo * sc, g0.w * sc, g0.h * sc);
                }
                cx += g0.adv * sc;
            }
        };
        drawStr(1.8f * u, 2.2f * u, 0.04f, 0.02f, 0.01f, 0.82f * alpha);
        drawStr(0.0f, 0.0f, 0.94f, 0.89f, 0.78f, alpha);
    }
    glUniform1i(uMode_, 1); glUniform4f(uColor_, 0.90f, 0.72f, 0.34f, alpha);
    quad(px + padX + tw + gap, py + (panelH - d) * 0.5f, d, d);

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

}  // namespace e0
