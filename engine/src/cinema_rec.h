// Isolated - MODO CINEMA: gravador. A engine desenha direto na superfície de entrada do encoder H.264 do aparelho (AMediaCodec), com tempo fixo:
// o vídeo sai com FPS constante mesmo que cada quadro leve mais que 1/60 s, sem HUD, sem barra do sistema e sem nada que esteja por cima do app.
// Um encoder, dois muxers: o arquivo completo e um clipe por plano (cada plano começa num quadro-chave).
#pragma once
#include <EGL/egl.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct AMediaCodec; struct AMediaMuxer; struct AMediaFormat; struct ANativeWindow;

namespace e0 {

class CinemaRecorder {
public:
    // Deixa a superfície do encoder como alvo de render atual. cfg precisa ter EGL_RECORDABLE_ANDROID.
    bool start(EGLDisplay dpy, EGLConfig cfg, EGLContext ctx, int w, int h, int fps, float mbps, const std::string& fullPath);
    void nextClip(const std::string& path, long long ptsNs);   // o próximo quadro-chave a partir de ptsNs abre este clipe
    void present(long long ptsNs);                             // entrega o quadro desenhado
    void finish();                                             // fecha os arquivos e devolve a superfície (a engine reativa a da janela)
    bool active() const { return codec_ != nullptr; }
    long long samples() const { return samples_.load(); }
private:
    void drainLoop();
    void openClip(const std::string& path);
    void closeClip();
    EGLDisplay dpy_ = EGL_NO_DISPLAY; EGLSurface surf_ = EGL_NO_SURFACE;
    AMediaCodec* codec_ = nullptr; ANativeWindow* win_ = nullptr; AMediaFormat* outFmt_ = nullptr;
    AMediaMuxer* full_ = nullptr; AMediaMuxer* clip_ = nullptr; int fullFd_ = -1, clipFd_ = -1, fullTrack_ = -1, clipTrack_ = -1;
    long long clipBase_ = 0, clipFrom_ = -1; std::string fullPath_, nextPath_;
    std::mutex m_; std::thread th_; std::atomic<bool> eos_{false}; std::atomic<long long> samples_{0};
};

}  // namespace e0
