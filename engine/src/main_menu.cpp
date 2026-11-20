#include "main_menu.h"
#include "e0i.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <arpa/inet.h>
#include <errno.h>
#include <jni.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "menu_atlas.h"
#include "menu_links.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated", __VA_ARGS__)

namespace e0 {

namespace {

constexpr int kDefaultPort = 7777;   // = e0net::kDefaultPort (shared/protocol.h); o TCP do download do mundo usa a mesma porta

const char* kVS = R"(#version 300 es
uniform vec4 uRect;      // x, y, w, h em pixels (canto superior esquerdo)
uniform vec2 uScreen;
out vec2 vP;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vP = c;
    vec2 px = uRect.xy + c * uRect.zw;
    gl_Position = vec4(px.x / uScreen.x * 2.0 - 1.0, 1.0 - px.y / uScreen.y * 2.0, 0.0, 1.0);
})";

// Tudo sai PRÉ-MULTIPLICADO (blend ONE, ONE_MINUS_SRC_ALPHA); alfa 0 com cor = brilho aditivo.
const char* kFS = R"(#version 300 es
precision mediump float;
in vec2 vP;
uniform int uMode;      // 0 arte de fundo, 1 degradê escuro à esquerda/embaixo, 2 sprite do atlas, 3 barra com fade, 4 ponto de luz aditivo, 5 painel suave, 6 botão arredondado nítido
uniform vec4 uBox;      // modo 6: largura, altura, raio, borda (px)
uniform vec4 uColor2;   // modo 6: cor da borda
uniform vec4 uColor;
uniform vec4 uUv;
uniform float uTime;
uniform sampler2D uImg;
uniform sampler2D uAtl;
out vec4 o;
void main() {
    if (uMode == 0) {
        vec3 c = texture(uImg, mix(uUv.xy, uUv.zw, vP)).rgb;
        float v = smoothstep(1.30, 0.40, length((vP - 0.5) * vec2(1.5, 1.0)));      // vinheta
        o = vec4(c * mix(0.62, 1.0, v) * uColor.rgb, 1.0);
    } else if (uMode == 1) {
        float gx = 1.0 - smoothstep(0.0, 0.66, vP.x);
        float gy = smoothstep(0.66, 1.0, vP.y);
        float top = (1.0 - smoothstep(0.0, 0.25, vP.y)) * 0.30;
        float a = clamp(0.84 * pow(gx, 1.35) + 0.52 * gy + top, 0.0, 0.93) * uColor.a;
        o = vec4(vec3(0.012, 0.026, 0.055) * a, a);
    } else if (uMode == 2) {
        vec4 t = texture(uAtl, mix(uUv.xy, uUv.zw, vP));
        o = vec4(t.rgb * uColor.rgb * uColor.a, t.a * uColor.a);
    } else if (uMode == 3) {
        float a = uColor.a * (1.0 - vP.x) * smoothstep(0.0, 0.02, vP.x);
        o = vec4(uColor.rgb * a, a);
    } else if (uMode == 6) {
        vec2 p = (vP - 0.5) * uBox.xy;
        vec2 q = abs(p) - uBox.xy * 0.5 + vec2(uBox.z);
        float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - uBox.z;
        float aa = fwidth(d) + 1e-4;
        float fill = 1.0 - smoothstep(-aa, aa, d);
        float bord = uBox.w > 0.0 ? fill * smoothstep(-uBox.w - aa, -uBox.w + aa, d) : 0.0;
        vec4 c = mix(vec4(uColor.rgb * uColor.a, uColor.a), vec4(uColor2.rgb * uColor2.a, uColor2.a), bord);
        o = c * fill;
    } else if (uMode == 5) {
        vec2 q = abs(vP - 0.5) * 2.0;
        float a = uColor.a * (1.0 - smoothstep(0.55, 1.0, max(q.x, q.y)));
        o = vec4(uColor.rgb * a, a);
    } else {
        float d = clamp(length(vP - 0.5) * 2.0, 0.0, 1.0);
        float a = uColor.a * (1.0 - d) * (1.0 - d);
        o = vec4(uColor.rgb * a, 0.0);
    }
})";

unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[512]; glGetShaderInfoLog(s, sizeof b, nullptr, b); LOG("menu: shader falhou: %s", b); glDeleteShader(s); return 0; }
    return s;
}

float smooth01(float x) { x = std::max(0.0f, std::min(1.0f, x)); return x * x * (3.0f - 2.0f * x); }
float fract(float x) { return x - std::floor(x); }

// ---- JNI (sem mexer no resto do Java): a thread do jogo se anexa à JVM só durante a chamada ----
struct Jni {
    JavaVM* vm = nullptr; JNIEnv* env = nullptr; bool attached = false, ok = false;
    explicit Jni(ANativeActivity* a) {
        if (!a || !a->vm) return;
        vm = a->vm;
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { env = nullptr; return; }
            attached = true;
        }
        ok = env != nullptr;
    }
    ~Jni() {
        if (env && env->ExceptionCheck()) env->ExceptionClear();
        if (attached) vm->DetachCurrentThread();
    }
};

// Abre um link no navegador (Intent ACTION_VIEW).
bool openUrl(ANativeActivity* a, const char* url) {
    if (!url || !*url) return false;
    Jni j(a);
    if (!j.ok) return false;
    JNIEnv* env = j.env;
    jclass uriC = env->FindClass("android/net/Uri");
    jclass intC = env->FindClass("android/content/Intent");
    if (!uriC || !intC) return false;
    jmethodID parse = env->GetStaticMethodID(uriC, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jmethodID ctor = env->GetMethodID(intC, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jclass actC = env->GetObjectClass(a->clazz);
    jmethodID start = env->GetMethodID(actC, "startActivity", "(Landroid/content/Intent;)V");
    if (!parse || !ctor || !start) return false;
    jstring js = env->NewStringUTF(url), ja = env->NewStringUTF("android.intent.action.VIEW");
    jobject uri = env->CallStaticObjectMethod(uriC, parse, js);
    jobject in = env->NewObject(intC, ctor, ja, uri);
    env->CallVoidMethod(a->clazz, start, in);
    return !env->ExceptionCheck();
}

// Métodos estáticos de GameActivity.java (showTextInput / takeInputResult / shareText).
bool jniShowInput(ANativeActivity* a, const char* title, const std::string& initial, const char* hint) {
    Jni j(a);
    if (!j.ok) return false;
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "showTextInput", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (!m) { env->ExceptionClear(); return false; }
    env->CallStaticVoidMethod(c, m, env->NewStringUTF(title), env->NewStringUTF(initial.c_str()), env->NewStringUTF(hint));
    return !env->ExceptionCheck();
}
bool jniTakeInput(ANativeActivity* a, std::string& out) {   // true se há resultado ("\x01" = cancelado)
    Jni j(a);
    if (!j.ok) { out = "\x01"; return true; }
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "takeInputResult", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); out = "\x01"; return true; }
    jstring r = (jstring)env->CallStaticObjectMethod(c, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); out = "\x01"; return true; }
    if (!r) return false;
    const char* s = env->GetStringUTFChars(r, nullptr);
    out = s ? s : "";
    if (s) env->ReleaseStringUTFChars(r, s);
    return true;
}
bool jniShare(ANativeActivity* a, const std::string& text) {
    Jni j(a);
    if (!j.ok) return false;
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "shareText", "(Ljava/lang/String;)V");
    if (!m) { env->ExceptionClear(); return false; }
    env->CallStaticVoidMethod(c, m, env->NewStringUTF(text.c_str()));
    return !env->ExceptionCheck();
}

// Tempo (ms) para abrir uma conexão TCP com o servidor; -1 se falhou/estourou o limite.
int tcpProbe(const std::string& ip, int port, int timeoutMs) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) { close(fd); return -1; }
    const auto t0 = std::chrono::steady_clock::now();
    int r = connect(fd, (sockaddr*)&sa, sizeof sa);
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r < 0) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeoutMs) <= 0) { close(fd); return -1; }
        int err = 0; socklen_t l = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) { close(fd); return -1; }
    }
    const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    close(fd);
    return std::max(1, ms);
}

}  // namespace

MainMenu::MainMenu() {}
MainMenu::~MainMenu() { stopProbe(); }

bool MainMenu::parseServer(const std::string& s, std::string& ip, int& port) {
    const size_t c = s.find(':');
    ip = s.substr(0, c); port = kDefaultPort;
    in_addr tmp;
    if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &tmp) != 1) return false;
    if (c != std::string::npos) {
        const std::string p = s.substr(c + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
        port = std::atoi(p.c_str());
        if (port < 1 || port > 65535) return false;
    }
    return true;
}

std::string MainMenu::effectiveServer() const { return opts.server.empty() ? defaultServer_ : opts.server; }
std::string MainMenu::displayServer() const {
    std::string s = effectiveServer();
    const std::string suf = ":" + std::to_string(kDefaultPort);
    if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0) s.erase(s.size() - suf.size());
    return s;
}

void MainMenu::startProbe() {
    if (probe_) return;
    probe_ = std::make_unique<Probe>();
    retarget();
    Probe* P = probe_.get();
    P->th = std::thread([P]() {
        while (!P->stop.load()) {
            std::string ip; int port, ver;
            { std::lock_guard<std::mutex> g(P->mu); ip = P->ip; port = P->port; ver = P->version.load(); }
            int next = 4000;
            if (ip.empty()) { P->state = 3; }
            else {
                const int ms = tcpProbe(ip, port, 2000);
                if (P->version.load() == ver) { P->ms = ms; P->state = ms >= 0 ? 1 : 2; }
                if (ms < 0) next = 2500;
            }
            for (int w = 0; w < next && !P->stop.load() && P->version.load() == ver; w += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void MainMenu::stopProbe() {
    if (!probe_) return;
    probe_->stop = true;
    if (probe_->th.joinable()) probe_->th.join();
    probe_.reset();
}

void MainMenu::retarget() {
    if (!probe_) return;
    std::string ip; int port = kDefaultPort;
    const std::string eff = effectiveServer();
    if (!eff.empty() && !parseServer(eff, ip, port)) ip.clear();
    { std::lock_guard<std::mutex> g(probe_->mu); probe_->ip = ip; probe_->port = port; }
    probe_->state = ip.empty() ? 3 : 0;
    probe_->version++;
}

bool MainMenu::init(AAssetManager* am, ANativeActivity* act, unsigned imgTex, int imgW, int imgH, const std::string& defaultServer) {
    act_ = act; texImg_ = imgTex; imgW_ = imgW; imgH_ = imgH; defaultServer_ = defaultServer;
    unsigned vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    prog_ = glCreateProgram(); glAttachShader(prog_, vs); glAttachShader(prog_, fs); glLinkProgram(prog_);
    glDeleteShader(vs); glDeleteShader(fs);
    int ok = 0; glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) { LOG("menu: link falhou"); glDeleteProgram(prog_); prog_ = 0; return false; }
    uRect_ = glGetUniformLocation(prog_, "uRect"); uScreen_ = glGetUniformLocation(prog_, "uScreen"); uMode_ = glGetUniformLocation(prog_, "uMode");
    uColor_ = glGetUniformLocation(prog_, "uColor"); uTime_ = glGetUniformLocation(prog_, "uTime"); uUv_ = glGetUniformLocation(prog_, "uUv");
    uBox_ = glGetUniformLocation(prog_, "uBox"); uColor2_ = glGetUniformLocation(prog_, "uColor2");
    glGenVertexArrays(1, &vao_);
    bool atlasOk = false;
    if (am) {
        if (AAsset* a = AAssetManager_open(am, "menu.e0i", AASSET_MODE_BUFFER)) {
            const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a); size_t len = (size_t)AAsset_getLength(a);
            if (len >= 8 && std::memcmp(b, "E0I1", 4) == 0) {
                int aw = b[4] | (b[5] << 8), ah = b[6] | (b[7] << 8);
                if (aw == kMenuAtlasW && ah == kMenuAtlasH && len >= 8 + (size_t)aw * ah * 4) {
                    glGenTextures(1, &texAtlas_); glBindTexture(GL_TEXTURE_2D, texAtlas_);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, b + 8);
                    glGenerateMipmap(GL_TEXTURE_2D);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    atlasOk = true;
                } else LOG("menu: atlas com tamanho inesperado (%dx%d); rode tools/make_menu.py", aw, ah);
            }
            AAsset_close(a);
        } else LOG("menu: menu.e0i nao encontrado");
    }
    if (!atlasOk) { shutdown(); return false; }
    if (am) {   // arte própria da tela de login (convés ao pôr do sol); se faltar, a tela de login usa a arte normal do menu
        if (AAsset* a = AAssetManager_open(am, "scene_deck.e0i", AASSET_MODE_BUFFER)) {
            const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a); size_t len = (size_t)AAsset_getLength(a);
            int sw = 0, sh = 0;
            texLogin_ = uploadE0I(b, len, &sw, &sh, true);   // E0I1 ou E0I2 (ETC2 com mipmaps)
            if (texLogin_ && (sw != 1280 || sh != 720)) { glDeleteTextures(1, &texLogin_); texLogin_ = 0; }
            AAsset_close(a);
        }
    }
    t_ = 0; pageT_ = 1.0f; page_ = PageMain; leaving_ = quitNow_ = false; sel_ = HitPlay; pressed_ = HitNone; soonT_ = badT_ = 0; leaveT_ = 0; inputOpen_ = false;
    startProbe();
    return true;
}

void MainMenu::shutdown() {
    stopProbe();
    if (texImg_) glDeleteTextures(1, &texImg_);
    if (texAtlas_) glDeleteTextures(1, &texAtlas_);
    if (texLogin_) glDeleteTextures(1, &texLogin_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    texImg_ = texAtlas_ = texLogin_ = vao_ = prog_ = 0;
}

void MainMenu::pollInput() {
    if (!inputOpen_) return;
    std::string r;
    if (!jniTakeInput(act_, r)) return;
    inputOpen_ = false;
    if (r == "\x01") return;   // cancelado
    if (r.empty()) { opts.server.clear(); }
    else {
        std::string ip; int port;
        if (!parseServer(r, ip, port)) { badT_ = 2.4f; return; }
        opts.server = port == kDefaultPort ? ip : ip + ":" + std::to_string(port);
    }
    optsDirty = true;
    retarget();
}

void MainMenu::update(float dt) {
    t_ += dt;
    pageT_ = std::min(1.0f, pageT_ + dt / 0.32f);
    if (soonT_ > 0) soonT_ = std::max(0.0f, soonT_ - dt);
    if (badT_ > 0) badT_ = std::max(0.0f, badT_ - dt);
    if (leaving_) leaveT_ = std::min(1.0f, leaveT_ + dt / 0.6f);
    if (loginMsgT_ > 0) { loginMsgT_ -= dt; if (loginMsgT_ <= 0) loginMsg_ = 0; }
    pollInput();
}

MainMenu::Action MainMenu::poll() {
    if (quitNow_) { quitNow_ = false; return Action::Quit; }
    if (charNow_) { charNow_ = false; return Action::Character; }
    if (teamNow_) { teamNow_ = false; return Action::Team; }
    if (loginGoogleNow_) { loginGoogleNow_ = false; return Action::LoginGoogle; }
    if (loginGuestNow_) { loginGuestNow_ = false; return Action::LoginGuest; }
    if (logoutNow_) { logoutNow_ = false; return Action::Logout; }
    if (leaving_ && leaveT_ >= 1.0f) return Action::Play;
    return Action::None;
}

void MainMenu::quad(float x, float y, float qw, float qh) { glUniform4f(uRect_, x, y, qw, qh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
void MainMenu::button(float x, float y, float bw, float bh, float r, float g, float b, float a, float br, float bg, float bb, float ba, float radius, float border) {
    if (a <= 0.003f && ba <= 0.003f) return;
    glUniform1i(uMode_, 6); glUniform4f(uColor_, r, g, b, a); glUniform4f(uColor2_, br, bg, bb, ba); glUniform4f(uBox_, bw, bh, radius, border);
    quad(x, y, bw, bh);
}

void MainMenu::sprite(int id, float x, float y, float scale, float r, float g, float b, float a) {
    if (a <= 0.003f) return;
    const MenuSprite& s = kMenuSprites[id];
    const float u0 = (float)s.x / kMenuAtlasW, v0 = (float)s.y / kMenuAtlasH, u1 = (float)(s.x + s.w) / kMenuAtlasW, v1 = (float)(s.y + s.h) / kMenuAtlasH;
    glUniform1i(uMode_, 2); glUniform4f(uUv_, u0, v0, u1, v1); glUniform4f(uColor_, r, g, b, a);
    quad(x, y, s.w * scale, s.h * scale);
}
void MainMenu::spriteC(int id, float cx, float cy, float scale, float r, float g, float b, float a) {
    const MenuSprite& s = kMenuSprites[id];
    sprite(id, cx - s.w * scale * 0.5f, cy - s.h * scale * 0.5f, scale, r, g, b, a);
}

static int digitSprite(char c) { return c == '.' ? (int)MS_DDOT : c == ':' ? (int)MS_DCOLON : (c >= '0' && c <= '9') ? (int)MS_D0 + (c - '0') : -1; }
float MainMenu::textWidth(const std::string& s, float scale) const {
    float w = 0;
    for (char c : s) { const int id = digitSprite(c); if (id >= 0) w += (kMenuSprites[id].w - 6) * scale; }
    return w + 6 * scale;
}
void MainMenu::digits(const std::string& s, float x, float y, float scale, float r, float g, float b, float a) {
    float cx = x;
    for (char c : s) {
        const int id = digitSprite(c);
        if (id < 0) continue;
        sprite(id, cx - 3 * scale, y, scale, r, g, b, a);
        cx += (kMenuSprites[id].w - 6) * scale;
    }
}

void MainMenu::goPage(int page, int sel) { page_ = page; pageT_ = 0; sel_ = sel; }

void MainMenu::draw(int w, int h) {
    if (!prog_ || w <= 0 || h <= 0) return;
    w_ = w; h_ = h;
    const float u = (float)h / 1080.0f;
    const float fadeIn = smooth01(t_ / 0.9f);
    const float leaveE = smooth01(leaveT_);
    const float uiA = fadeIn * (1.0f - leaveE);

    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, w, h);
    glUseProgram(prog_); glBindVertexArray(vao_);
    glUniform2f(uScreen_, (float)w, (float)h); glUniform1f(uTime_, t_);
    glUniform1i(glGetUniformLocation(prog_, "uImg"), 0); glUniform1i(glGetUniformLocation(prog_, "uAtl"), 1);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texAtlas_);
    const bool loginArt = page_ == PageLogin && texLogin_ != 0;   // tela de login: arte do convés ao pôr do sol
    const int artW = loginArt ? 1280 : imgW_, artH = loginArt ? 720 : imgH_;
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, loginArt ? texLogin_ : texImg_);
    std::memset(hitOn_, 0, sizeof hitOn_);

    // 1) arte de fundo "cover" com um zoom/deriva lentíssimo (volta a 1,0 ao sair, para casar com a tela de loading que vem depois)
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    {
        float sa = (float)w / (float)h, ia = (float)artW / (float)artH;
        if (sa > ia) { float f = ia / sa; v0 = (1 - f) * 0.5f; v1 = 1 - v0; } else { float f = sa / ia; u0 = (1 - f) * 0.5f; u1 = 1 - u0; }
        const float z = 1.0f + 0.035f * (0.5f - 0.5f * std::cos(t_ * 0.18f)) * (1.0f - leaveE);
        const float cu = 0.5f * (u0 + u1) + 0.004f * std::sin(t_ * 0.11f) * (1.0f - leaveE), cv = 0.5f * (v0 + v1);
        const float hu = 0.5f * (u1 - u0) / z, hv = 0.5f * (v1 - v0) / z;
        u0 = cu - hu; u1 = cu + hu; v0 = cv - hv; v1 = cv + hv;
        glUniform1i(uMode_, 0); glUniform4f(uColor_, 1, 1, 1, 1); glUniform4f(uUv_, u0, v0, u1, v1);
        quad(0, 0, (float)w, (float)h);
    }
    // 2) brilho do sol (posição do sol na arte: ~ (0.70, 0.64)) pulsando devagar
    {
        const float sunX = loginArt ? 0.676f : 0.700f, sunY = loginArt ? 0.520f : 0.640f;   // posição do sol em cada arte
        const float sx = (sunX - u0) / (u1 - u0) * w, sy = (sunY - v0) / (v1 - v0) * h;
        const float pulse = 0.85f + 0.15f * std::sin(t_ * 0.9f);
        glUniform1i(uMode_, 4); glUniform4f(uColor_, 1.0f, 0.86f, 0.55f, 0.36f * pulse * (1.0f - leaveE));
        quad(sx - 380 * u, sy - 380 * u, 760 * u, 760 * u);
    }
    // 3) degradê escuro (esquerda/baixo) que dá leitura ao menu; some ao sair
    glUniform1i(uMode_, 1); glUniform4f(uColor_, 1, 1, 1, 1.0f - leaveE * 0.9f);
    quad(0, 0, (float)w, (float)h);
    // 4) poeira dourada à deriva (aditiva)
    glUniform1i(uMode_, 4);
    for (int i = 0; i < 26; ++i) {
        const float fi = (float)i;
        const float px = fract(fract(fi * 0.381966f) + t_ * (0.006f + 0.010f * fract(fi * 0.7f))) * w;
        const float py = fract(fract(fi * 0.754877f) - t_ * (0.012f + 0.018f * fract(fi * 0.31f))) * h + 14.0f * u * std::sin(t_ * 0.7f + fi);
        const float sz = (7.0f + 13.0f * fract(fi * 0.913f)) * u;
        const float tw = 0.45f + 0.55f * std::sin(t_ * (0.9f + 0.5f * fract(fi * 0.17f)) + fi * 1.7f);
        glUniform4f(uColor_, 1.0f, 0.84f, 0.50f, 0.34f * std::max(0.0f, tw) * fadeIn * (1.0f - leaveE));
        quad(px - sz, py - sz, sz * 2, sz * 2);
    }

    // 5) interface
    const float gold[3] = {1.00f, 0.80f, 0.34f}, cream[3] = {0.94f, 0.90f, 0.79f}, shadow[3] = {0, 0, 0};
    const float left = 160.0f * u;
    const float pg = smooth01(pageT_);   // transição entre páginas (fade + deslize)
    auto slide = [&](float k) { return (1.0f - k) * 46.0f * u; };

    if (page_ == PageMain) {
        const float tScale = 0.80f * u;
        const MenuSprite& ts = kMenuSprites[MS_TITLE];
        const float titleX = 96.0f * u, titleY = (36.0f - 26.0f * (1.0f - smooth01(t_ / 1.1f))) * u;
        const float titleW = ts.w * tScale, titleH = ts.h * tScale;
        sprite(MS_TITLE, titleX, titleY, tScale, 1, 1, 1, uiA * pg);
        const float cx = titleX + titleW * 0.5f;
        spriteC(MS_SUBTITLE, cx, titleY + titleH + 4.0f * u, 0.62f * u, 0.95f, 0.86f, 0.62f, uiA * pg * smooth01((t_ - 0.5f) / 0.6f));
        spriteC(MS_ORNAMENT, cx, titleY + titleH + 52.0f * u, 0.85f * u, 0.95f, 0.74f, 0.32f, uiA * pg * 0.9f * smooth01((t_ - 0.6f) / 0.6f));

        const int ids[7] = {MS_PLAY, MS_CHARACTER, MS_CREW, MS_INVITE, MS_SETTINGS, MS_CREDITS, MS_QUIT};
        const int hits[7] = {HitPlay, HitCharacter, HitTeam, HitInvite, HitSettings, HitCredits, HitQuit};
        const float y0 = 372.0f * u, step = 76.0f * u, isc = 0.60f * u;
        for (int i = 0; i < 7; ++i) {
            const float k = smooth01((t_ - 0.45f - 0.09f * i) / 0.55f);
            const float ap = k * uiA * pg;
            const MenuSprite& s = kMenuSprites[ids[i]];
            const float x = left + (1.0f - k) * -50.0f * u + slide(pg) * -0.6f;
            const float y = y0 + step * i;
            const bool isSel = sel_ == hits[i], isPress = pressed_ == hits[i];
            const float* col = (isPress || isSel) ? gold : cream;
            const float sc = isPress ? 1.03f : 1.0f;
            sprite(ids[i], x + 3.0f * u, y + 4.0f * u, isc * sc, shadow[0], shadow[1], shadow[2], 0.62f * ap);
            sprite(ids[i], x, y, isc * sc, col[0], col[1], col[2], (isSel || isPress ? 1.0f : 0.88f) * ap);
            if (isSel) {
                const float pulse = 0.7f + 0.3f * std::sin(t_ * 3.2f);
                const float cyy = y + s.h * isc * 0.5f;
                spriteC(MS_STAR, left - 52.0f * u, cyy, 0.54f * u * (0.9f + 0.1f * pulse), gold[0], gold[1], gold[2], ap);
                glUniform1i(uMode_, 3); glUniform4f(uColor_, gold[0], gold[1], gold[2], 0.85f * ap);
                quad(x, y + s.h * isc - 5.0f * u, s.w * isc + 70.0f * u, 3.0f * u);
            }
            hit_[hits[i]] = {96.0f * u, y - 4.0f * u, 800.0f * u, step - 2.0f * u}; hitOn_[hits[i]] = ap > 0.4f;
        }
        // ícones das comunidades (canto inferior esquerdo)
        const int icons[4] = {MS_ICON_DISCORD, MS_ICON_YOUTUBE, MS_ICON_INSTAGRAM, MS_ICON_TIKTOK};
        const float isz = 0.5f * u, ipx = 128.0f * isz, gap = 34.0f * u, iy = h - 128.0f * u;
        for (int i = 0; i < 4; ++i) {
            const float ap = smooth01((t_ - 1.0f - 0.07f * i) / 0.5f) * uiA * pg;
            const float x = left + i * (ipx + gap), y = iy;
            const bool press = pressed_ == HitSocial0 + i;
            sprite(icons[i], x + 2.0f * u, y + 3.0f * u, isz, 0, 0, 0, 0.55f * ap);
            sprite(icons[i], x, y, isz * (press ? 1.08f : 1.0f), press ? gold[0] : cream[0], press ? gold[1] : cream[1], press ? gold[2] : cream[2], 0.82f * ap);
            hit_[HitSocial0 + i] = {x - 16.0f * u, y - 16.0f * u, ipx + 32.0f * u, ipx + 32.0f * u}; hitOn_[HitSocial0 + i] = ap > 0.4f;
        }
        if (soonT_ > 0) {   // aviso logo acima do ícone tocado: sobe um pouco e some
            const float k = std::min(1.0f, soonT_ * 3.0f), rise = (1.0f - std::min(1.0f, (1.8f - soonT_) * 6.0f)) * 10.0f * u;
            const float cx2 = left + soonIdx_ * (ipx + gap) + ipx * 0.5f, cy2 = iy - 22.0f * u + rise;
            spriteC(MS_SOON, cx2 + 2.0f * u, cy2 + 3.0f * u, 0.46f * u, 0, 0, 0, 0.7f * k * uiA);
            spriteC(MS_SOON, cx2, cy2, 0.46f * u, gold[0], gold[1], gold[2], k * uiA);
        }
        const MenuSprite& vs = kMenuSprites[MS_VERSION];
        sprite(MS_VERSION, w - 44.0f * u - vs.w * 0.8f * u, h - 74.0f * u, 0.8f * u, cream[0], cream[1], cream[2], 0.55f * uiA * pg);
        if (account) {   // conta em uso, no canto inferior direito acima da versão
            const int accId = account == 2 ? (int)MS_ACC_GOOGLE : (int)MS_ACC_GUEST;
            const float as = 0.62f * u, aw = kMenuSprites[accId].w * as + (account == 2 ? 50.0f * u : 0.0f);
            const float ax = w - 44.0f * u - aw, ay = h - 128.0f * u, ca = 0.75f * uiA * pg * smooth01((t_ - 0.9f) / 0.6f);
            if (account == 2) sprite(MS_G_LOGO, ax, ay + 2.0f * u, 0.30f * u, 1, 1, 1, ca);
            sprite(accId, ax + (account == 2 ? 50.0f * u : 0.0f), ay, as, cream[0], cream[1], cream[2], ca);
        }

        // cartão de STATUS DO SERVIDOR (canto superior direito): bolinha + estado + ping, endereço embaixo
        {
            const int st = probe_ ? probe_->state.load() : 0, ms = probe_ ? probe_->ms.load() : 0;
            const int wordId = st == 1 ? MS_SRV_ONLINE : st == 2 ? MS_SRV_OFFLINE : st == 3 ? MS_SRV_LOCAL : MS_SRV_CHECK;
            static const float kGreen[3] = {0.45f, 0.92f, 0.52f}, kRed[3] = {0.96f, 0.42f, 0.36f}, kGray[3] = {0.80f, 0.80f, 0.76f};
            const float* sc3 = st == 1 ? kGreen : st == 2 ? kRed : st == 3 ? kGray : gold;
            const float fs = 0.62f * u, ds = 0.54f * u;
            const std::string addr = st == 3 ? std::string() : displayServer();
            const std::string ping = st == 1 ? std::to_string(ms) : std::string();
            const float dotW = 34.0f * u;
            float line1 = dotW + kMenuSprites[wordId].w * fs;
            if (st == 1) line1 += 14.0f * u + textWidth(ping, fs * 1.0f) + 6.0f * u + kMenuSprites[MS_MS].w * fs;
            const float line2 = addr.empty() ? 0.0f : textWidth(addr, ds);
            const float contentW = std::max(line1, line2), a = uiA * pg * smooth01((t_ - 0.8f) / 0.6f);
            const float right = w - 56.0f * u, px = right - contentW, py = 46.0f * u;
            glUniform1i(uMode_, 5); glUniform4f(uColor_, 0.02f, 0.05f, 0.10f, 0.74f * a);
            quad(px - 46.0f * u, py - 26.0f * u, contentW + 92.0f * u, (addr.empty() ? 70.0f : 112.0f) * u);
            const float pulse = st == 0 ? 0.55f + 0.45f * std::sin(t_ * 5.0f) : 1.0f;
            glUniform1i(uMode_, 4); glUniform4f(uColor_, sc3[0], sc3[1], sc3[2], 0.9f * a * pulse); quad(px - 6.0f * u, py + 2.0f * u, 30.0f * u, 30.0f * u);
            glUniform4f(uColor_, sc3[0], sc3[1], sc3[2], 1.0f * a * pulse); quad(px + 3.0f * u, py + 11.0f * u, 12.0f * u, 12.0f * u);
            float x1 = px + dotW;
            sprite(wordId, x1 + 2.0f * u, py + 2.0f * u, fs, 0, 0, 0, 0.6f * a);
            sprite(wordId, x1, py, fs, sc3[0], sc3[1], sc3[2], a);
            x1 += kMenuSprites[wordId].w * fs + 14.0f * u;
            if (st == 1) {
                digits(ping, x1, py, fs, cream[0], cream[1], cream[2], a);
                x1 += textWidth(ping, fs) + 6.0f * u;
                sprite(MS_MS, x1, py, fs, cream[0], cream[1], cream[2], 0.8f * a);
            }
            if (!addr.empty()) digits(addr, right - line2, py + 48.0f * u, ds, cream[0], cream[1], cream[2], 0.7f * a);
        }
    } else if (page_ == PageLogin) {   // ---- TELA DE LOGIN (antes do menu): Google ou visitante ----
        const float a = uiA * pg;
        const float tScale = 0.80f * u;
        const MenuSprite& ts = kMenuSprites[MS_TITLE];
        const float titleX = 96.0f * u, titleY = (36.0f - 26.0f * (1.0f - smooth01(t_ / 1.1f))) * u;
        const float titleW = ts.w * tScale, titleH = ts.h * tScale;
        sprite(MS_TITLE, titleX, titleY, tScale, 1, 1, 1, a);
        const float cx = titleX + titleW * 0.5f;
        spriteC(MS_SUBTITLE, cx, titleY + titleH + 4.0f * u, 0.62f * u, 0.95f, 0.86f, 0.62f, a * smooth01((t_ - 0.5f) / 0.6f));
        spriteC(MS_ORNAMENT, cx, titleY + titleH + 52.0f * u, 0.85f * u, 0.95f, 0.74f, 0.32f, a * 0.9f * smooth01((t_ - 0.6f) / 0.6f));
        const float k0 = smooth01((t_ - 0.45f) / 0.55f), k1 = smooth01((t_ - 0.6f) / 0.55f), k2 = smooth01((t_ - 0.75f) / 0.55f);
        const float bx = left, bw = 760.0f * u, bh = 108.0f * u, rad = 22.0f * u;
        // cabeçalho
        sprite(MS_LOGIN_HEAD, bx + 2.0f * u, 404.0f * u + 3.0f * u, 0.62f * u, 0, 0, 0, 0.6f * a * k0);
        sprite(MS_LOGIN_HEAD, bx, 404.0f * u, 0.62f * u, cream[0], cream[1], cream[2], 0.9f * a * k0);
        // ENTRAR COM GOOGLE: botão branco com o "G" colorido (padrão visual do Google)
        {
            const float y = 470.0f * u + (1.0f - k1) * 30.0f * u, aa = a * k1;
            const bool press = pressed_ == HitLoginGoogle, dim = loginBusy_;
            const float sc = press ? 0.985f : 1.0f, ox = bw * (1.0f - sc) * 0.5f, oy = bh * (1.0f - sc) * 0.5f;
            button(bx + 4.0f * u, y + 7.0f * u, bw, bh, 0, 0, 0, 0.40f * aa, 0, 0, 0, 0, rad, 0);
            button(bx + ox, y + oy, bw * sc, bh * sc, dim ? 0.78f : 0.98f, dim ? 0.78f : 0.98f, dim ? 0.78f : 0.97f, aa, 0.85f, 0.85f, 0.85f, aa, rad, 2.0f * u);
            const float gs = 0.50f * u;
            sprite(MS_G_LOGO, bx + 40.0f * u, y + (bh - 128.0f * gs) * 0.5f, gs, 1, 1, 1, aa);
            const MenuSprite& s = kMenuSprites[MS_LOGIN_GOOGLE];
            const float ls = 0.70f * u;
            sprite(MS_LOGIN_GOOGLE, bx + 40.0f * u + 128.0f * gs + 34.0f * u, y + (bh - s.h * ls) * 0.5f, ls, 0.13f, 0.13f, 0.14f, aa);
            hit_[HitLoginGoogle] = {bx, y, bw, bh}; hitOn_[HitLoginGoogle] = aa > 0.4f && !loginBusy_;
        }
        // JOGAR COMO VISITANTE: botão escuro com borda dourada
        {
            const float y = 606.0f * u + (1.0f - k2) * 30.0f * u, aa = a * k2;
            const bool press = pressed_ == HitLoginGuest;
            const float sc = press ? 0.985f : 1.0f, ox = bw * (1.0f - sc) * 0.5f, oy = bh * (1.0f - sc) * 0.5f;
            button(bx + 4.0f * u, y + 7.0f * u, bw, bh, 0, 0, 0, 0.40f * aa, 0, 0, 0, 0, rad, 0);
            button(bx + ox, y + oy, bw * sc, bh * sc, 0.05f, 0.04f, 0.03f, 0.82f * aa, gold[0], gold[1], gold[2], (press ? 1.0f : 0.85f) * aa, rad, 3.0f * u);
            const MenuSprite& s = kMenuSprites[MS_LOGIN_GUEST];
            const float ls = 0.70f * u;
            sprite(MS_LOGIN_GUEST, bx + (bw - s.w * ls) * 0.5f, y + (bh - s.h * ls) * 0.5f, ls, gold[0], gold[1], gold[2], aa);
            hit_[HitLoginGuest] = {bx, y, bw, bh}; hitOn_[HitLoginGuest] = aa > 0.4f && !loginBusy_;
        }
        sprite(MS_LOGIN_NOTE, bx + 2.0f * u, 738.0f * u + 2.0f * u, 0.62f * u, 0, 0, 0, 0.5f * a * k2);
        sprite(MS_LOGIN_NOTE, bx, 738.0f * u, 0.62f * u, cream[0], cream[1], cream[2], 0.62f * a * k2);
        // estado: conectando (pulsa) ou o erro
        int msgId = -1; float mr = 1.0f, mg = 0.48f, mb = 0.40f, ma = 1.0f;
        if (loginBusy_) { msgId = MS_LOGIN_BUSY; mr = gold[0]; mg = gold[1]; mb = gold[2]; ma = 0.6f + 0.4f * std::sin(t_ * 5.0f); }
        else if (loginMsg_ == 1) msgId = MS_LOGIN_ERR_CFG;
        else if (loginMsg_ == 2) msgId = MS_LOGIN_ERR;
        else if (loginMsg_ == 3) { msgId = MS_LOGIN_CANCEL; mr = cream[0]; mg = cream[1]; mb = cream[2]; }
        else if (loginMsg_ == 4) msgId = MS_LOGIN_NET;
        if (msgId >= 0) {
            const float fa = loginBusy_ ? 1.0f : std::min(1.0f, loginMsgT_ * 2.0f);
            sprite(msgId, bx + 2.0f * u, 800.0f * u + 2.0f * u, 0.70f * u, 0, 0, 0, 0.6f * a * fa);
            sprite(msgId, bx, 800.0f * u, 0.70f * u, mr, mg, mb, ma * a * fa);
        }
        const MenuSprite& vs = kMenuSprites[MS_VERSION];
        sprite(MS_VERSION, w - 44.0f * u - vs.w * 0.8f * u, h - 74.0f * u, 0.8f * u, cream[0], cream[1], cream[2], 0.55f * a);
    } else if (page_ == PageSettings) {   // ---- CONFIGURAÇÕES ----
        const float a = uiA * pg;
        sprite(MS_SET_HEADER, left - 6.0f * u + slide(pg) * -0.6f, 90.0f * u, 0.95f * u, 1.0f, 0.86f, 0.52f, a);
        spriteC(MS_ORNAMENT, left + 330.0f * u, 242.0f * u, 0.85f * u, 0.95f, 0.74f, 0.32f, a * 0.9f);
        const int labels[5] = {MS_SET_QUALITY, MS_SET_FILTER, MS_SET_SOUND, MS_SET_SERVER, MS_SET_ACCOUNT};
        const int qv[4] = {MS_V_AUTO, MS_V_LOW, MS_V_MED, MS_V_HIGH}, gv[3] = {MS_V_OFF, MS_V_SOFT, MS_V_STRONG};
        const int vals[5] = {qv[std::max(0, std::min(3, opts.quality + 1))], gv[std::max(0, std::min(2, opts.grade))], opts.sound ? (int)MS_V_ON : (int)MS_V_OFF, (int)MS_V_DEFAULT, (int)MS_V_LOGOUT};
        const float ry0 = 290.0f * u, rstep = 108.0f * u, ls = 0.78f * u;
        const int nRows = account ? 5 : 4;
        for (int i = 0; i < nRows; ++i) {
            const float y = ry0 + i * rstep + slide(pg) * (0.2f + 0.15f * i);
            const float rowH = kMenuSprites[labels[i]].h * ls;
            const int hitId = i == 4 ? (int)HitSetAccount : (int)HitSetQuality + i;
            const bool press = pressed_ == hitId;
            if (press) { glUniform1i(uMode_, 3); glUniform4f(uColor_, gold[0], gold[1], gold[2], 0.16f * a); quad(left - 50.0f * u, y - 4.0f * u, 1260.0f * u, rowH + 8.0f * u); }
            sprite(labels[i], left + 3.0f * u, y + 4.0f * u, ls, 0, 0, 0, 0.62f * a);
            sprite(labels[i], left, y, ls, cream[0], cream[1], cream[2], 0.92f * a);
            const float vx = left + 640.0f * u;
            if (i == 3 && !opts.server.empty()) {   // endereço digitado
                digits(opts.server, vx + 3.0f * u, y + 12.0f * u, 1.05f * ls, 0, 0, 0, 0.62f * a);
                digits(opts.server, vx, y + 8.0f * u, 1.05f * ls, gold[0], gold[1], gold[2], a);
            } else {
                sprite(vals[i], vx + 3.0f * u, y + 4.0f * u, ls, 0, 0, 0, 0.62f * a);
                sprite(vals[i], vx, y, ls, gold[0], gold[1], gold[2], a);
            }
            glUniform1i(uMode_, 3); glUniform4f(uColor_, 0.95f, 0.74f, 0.32f, 0.35f * a);
            quad(left, y + rowH - 2.0f * u, 1100.0f * u, 2.0f * u);
            if (i == 4) {   // CONTA: tipo da conta ao lado do SAIR DA CONTA
                const int accId = account == 2 ? (int)MS_ACC_GOOGLE : (int)MS_ACC_GUEST;
                const float ax = vx + kMenuSprites[MS_V_LOGOUT].w * ls + 40.0f * u;
                if (account == 2) sprite(MS_G_LOGO, ax, y + 6.0f * u, 0.30f * u, 1, 1, 1, a);
                sprite(accId, ax + (account == 2 ? 48.0f * u : 0.0f) + 2.0f * u, y + 13.0f * u, 0.62f * u, 0, 0, 0, 0.7f * a);
                sprite(accId, ax + (account == 2 ? 48.0f * u : 0.0f), y + 10.0f * u, 0.62f * u, cream[0], cream[1], cream[2], 0.95f * a);
            }
            hit_[hitId] = {left - 50.0f * u, y - 6.0f * u, 1260.0f * u, rowH + 12.0f * u}; hitOn_[hitId] = a > 0.4f;
            if (i == 3 && badT_ > 0) spriteC(MS_BAD_ADDR, vx + 190.0f * u, y + rowH + 20.0f * u, 0.5f * u, 1.0f, 0.45f, 0.40f, std::min(1.0f, badT_ * 3.0f) * a);
        }
        const float by = ry0 + nRows * rstep + 20.0f * u + slide(pg) * 0.8f;
        const bool bp = pressed_ == HitSetBack;
        const MenuSprite& bs = kMenuSprites[MS_SET_BACK];
        sprite(MS_SET_BACK, left + 3.0f * u, by + 4.0f * u, 0.72f * u, 0, 0, 0, 0.62f * a);
        sprite(MS_SET_BACK, left, by, 0.72f * u * (bp ? 1.03f : 1.0f), gold[0], gold[1], gold[2], a);
        spriteC(MS_STAR, left - 52.0f * u, by + bs.h * 0.72f * u * 0.5f, 0.54f * u, gold[0], gold[1], gold[2], a);
        hit_[HitSetBack] = {96.0f * u, by - 6.0f * u, 700.0f * u, bs.h * 0.72f * u + 12.0f * u}; hitOn_[HitSetBack] = a > 0.4f;
    } else {   // ---- CRÉDITOS ----
        const float a = uiA * pg;
        sprite(MS_CRED_HEADER, left - 6.0f * u + slide(pg) * -0.6f, 90.0f * u, 0.95f * u, 1.0f, 0.86f, 0.52f, a);
        spriteC(MS_ORNAMENT, left + 330.0f * u, 242.0f * u, 0.85f * u, 0.95f, 0.74f, 0.32f, a * 0.9f);
        const int heads[5] = {MS_CRED_H0, MS_CRED_H1, MS_CRED_H2, MS_CRED_H3, MS_CRED_H4}, bodies[5] = {MS_CRED_B0, MS_CRED_B1, MS_CRED_B2, MS_CRED_B3, MS_CRED_B4};
        for (int i = 0; i < 5; ++i) {
            const float k = smooth01((t_ * 0 + pageT_ * 1.6f - 0.12f * i)), y = 290.0f * u + i * 108.0f * u + (1.0f - k) * 24.0f * u;
            sprite(heads[i], left, y + 2.0f * u, 0.56f * u, 0, 0, 0, 0.55f * a * k);
            sprite(heads[i], left - 2.0f * u, y, 0.56f * u, gold[0], gold[1], gold[2], 0.95f * a * k);
            sprite(bodies[i], left + 2.0f * u, y + 40.0f * u, 0.68f * u, 0, 0, 0, 0.6f * a * k);
            sprite(bodies[i], left, y + 38.0f * u, 0.68f * u, cream[0], cream[1], cream[2], 0.92f * a * k);
        }
        const float by = 290.0f * u + 5 * 108.0f * u + 6.0f * u;
        const bool bp = pressed_ == HitSetBack;
        const MenuSprite& bs = kMenuSprites[MS_SET_BACK];
        sprite(MS_SET_BACK, left + 3.0f * u, by + 4.0f * u, 0.72f * u, 0, 0, 0, 0.62f * a);
        sprite(MS_SET_BACK, left, by, 0.72f * u * (bp ? 1.03f : 1.0f), gold[0], gold[1], gold[2], a);
        spriteC(MS_STAR, left - 52.0f * u, by + bs.h * 0.72f * u * 0.5f, 0.54f * u, gold[0], gold[1], gold[2], a);
        hit_[HitSetBack] = {96.0f * u, by - 6.0f * u, 700.0f * u, bs.h * 0.72f * u + 12.0f * u}; hitOn_[HitSetBack] = a > 0.4f;
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void MainMenu::activate(int hit) {
    switch (hit) {
        case HitPlay: leaving_ = true; leaveT_ = 0; break;
        case HitCharacter: charNow_ = true; break;
        case HitTeam: teamNow_ = true; break;
        case HitInvite: {
            const std::string srv = displayServer();
            jniShare(act_, std::string("Vem jogar Isolated comigo!") + (srv.empty() ? "" : " Servidor: " + srv));
            break;
        }
        case HitSettings: goPage(PageSettings, HitSetBack); break;
        case HitCredits: goPage(PageCredits, HitSetBack); break;
        case HitQuit: quitNow_ = true; break;
        case HitSocial0: case HitSocial1: case HitSocial2: case HitSocial3: {
            const char* url = kMenuLinks[hit - HitSocial0];
            if (!url[0] || !openUrl(act_, url)) { soonT_ = 1.8f; soonIdx_ = hit - HitSocial0; }
            break;
        }
        case HitSetQuality: opts.quality = opts.quality >= 2 ? -1 : opts.quality + 1; optsDirty = true; break;   // AUTO -> BAIXA -> MEDIA -> ALTA -> AUTO
        case HitSetFilter: opts.grade = (opts.grade + 1) % 3; optsDirty = true; break;
        case HitSetSound: opts.sound = !opts.sound; optsDirty = true; break;
        case HitSetServer:
            if (!inputOpen_ && jniShowInput(act_, "Endereço do servidor", opts.server, "Ex.: 192.168.0.10 (vazio = padrão)")) inputOpen_ = true;
            break;
        case HitSetBack: goPage(PageMain, page_ == PageCredits ? HitCredits : HitSettings); break;
        case HitLoginGoogle: if (!loginBusy_) loginGoogleNow_ = true; break;
        case HitLoginGuest: if (!loginBusy_) loginGuestNow_ = true; break;
        case HitSetAccount: logoutNow_ = true; break;
        default: break;
    }
}

void MainMenu::touch(int type, float x, float y) {
    if (leaving_ || t_ < 0.6f || inputOpen_) return;   // ainda entrando, saindo ou com a caixa de texto aberta
    int found = HitNone;
    for (int i = 0; i < HitCount; ++i) if (hitOn_[i] && hit_[i].has(x, y)) { found = i; break; }
    if (type == 0) {
        pressed_ = found;
        if ((found >= HitPlay && found <= HitQuit) || found == HitCharacter || found == HitTeam) sel_ = found;
    } else if (type == 1) {
        if (found != pressed_) pressed_ = HitNone;   // arrastou para fora: cancela
    } else if (type == 2) {
        const int p = pressed_; pressed_ = HitNone;
        if (p != HitNone && p == found) activate(p);
    } else {
        pressed_ = HitNone;
    }
}

}  // namespace e0
