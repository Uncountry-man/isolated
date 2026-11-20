#include "cinema_rec.h"

#include <EGL/eglext.h>
#include <android/log.h>
#include <android/native_window.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated", __VA_ARGS__)

namespace e0 {

static constexpr int kColorFormatSurface = 0x7F000789;

bool CinemaRecorder::start(EGLDisplay dpy, EGLConfig cfg, EGLContext ctx, int w, int h, int fps, float mbps, const std::string& fullPath) {
    dpy_ = dpy; fullPath_ = fullPath; eos_ = false; samples_ = 0;
    codec_ = AMediaCodec_createEncoderByType("video/avc");
    if (!codec_) { LOG("cinema: sem encoder H.264"); return false; }
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatSurface);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, (int)(mbps * 1000000.0f));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);   // quadro-chave a cada 1 s: corta e navega bem no editor
    AMediaFormat_setInt32(f, "bitrate-mode", 1);                      // VBR
    AMediaFormat_setInt32(f, "profile", 8);                           // High
    AMediaFormat_setInt32(f, "level", 0x2000);                        // 4.2... o encoder ajusta para o que suporta
    media_status_t st = AMediaCodec_configure(codec_, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (st != AMEDIA_OK) {   // sem perfil/nível explícitos
        AMediaFormat_delete(f); f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatSurface);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, (int)(mbps * 1000000.0f));
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, fps); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        st = AMediaCodec_configure(codec_, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    }
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) { LOG("cinema: configure falhou (%d)", (int)st); AMediaCodec_delete(codec_); codec_ = nullptr; return false; }
    if (AMediaCodec_createInputSurface(codec_, &win_) != AMEDIA_OK || !win_) { LOG("cinema: sem superficie de entrada"); AMediaCodec_delete(codec_); codec_ = nullptr; return false; }
    surf_ = eglCreateWindowSurface(dpy_, cfg, win_, nullptr);
    if (surf_ == EGL_NO_SURFACE) { LOG("cinema: eglCreateWindowSurface falhou 0x%x", eglGetError()); AMediaCodec_delete(codec_); codec_ = nullptr; return false; }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) { LOG("cinema: start falhou"); AMediaCodec_delete(codec_); codec_ = nullptr; return false; }
    if (!eglMakeCurrent(dpy_, surf_, surf_, ctx)) { LOG("cinema: eglMakeCurrent falhou 0x%x", eglGetError()); return false; }
    eglSwapInterval(dpy_, 1);   // bloqueante: com 0 a superfície fica assíncrona e DESCARTA quadros quando o render é mais rápido que o encoder
    fullFd_ = open(fullPath_.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fullFd_ < 0) { LOG("cinema: nao abri %s", fullPath_.c_str()); return false; }
    full_ = AMediaMuxer_new(fullFd_, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    th_ = std::thread([this] { drainLoop(); });
    LOG("cinema: gravando %dx%d %d fps %.0f Mbps -> %s", w, h, fps, mbps, fullPath_.c_str());
    return true;
}

void CinemaRecorder::nextClip(const std::string& path, long long ptsNs) {
    { std::lock_guard<std::mutex> g(m_); nextPath_ = path; clipFrom_ = ptsNs / 1000; }
    AMediaFormat* p = AMediaFormat_new();
    AMediaFormat_setInt32(p, "request-sync", 0);
    AMediaCodec_setParameters(codec_, p);
    AMediaFormat_delete(p);
}

void CinemaRecorder::present(long long ptsNs) {
    using Fn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLnsecsANDROID);
    static Fn setPts = (Fn)eglGetProcAddress("eglPresentationTimeANDROID");
    if (setPts) setPts(dpy_, surf_, ptsNs);
    eglSwapBuffers(dpy_, surf_);
}

void CinemaRecorder::openClip(const std::string& path) {
    closeClip();
    clipFd_ = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (clipFd_ < 0 || !outFmt_) return;
    clip_ = AMediaMuxer_new(clipFd_, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    clipTrack_ = (int)AMediaMuxer_addTrack(clip_, outFmt_);
    AMediaMuxer_start(clip_);
}

void CinemaRecorder::closeClip() {
    if (clip_) { AMediaMuxer_stop(clip_); AMediaMuxer_delete(clip_); clip_ = nullptr; }
    if (clipFd_ >= 0) { close(clipFd_); clipFd_ = -1; }
}

void CinemaRecorder::drainLoop() {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            outFmt_ = AMediaCodec_getOutputFormat(codec_);
            fullTrack_ = (int)AMediaMuxer_addTrack(full_, outFmt_);
            AMediaMuxer_start(full_);
            continue;
        }
        if (i < 0) continue;
        size_t sz = 0; uint8_t* b = AMediaCodec_getOutputBuffer(codec_, (size_t)i, &sz);
        const bool config = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
        const bool end = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        if (b && info.size > 0 && !config && fullTrack_ >= 0) {
            AMediaMuxer_writeSampleData(full_, (size_t)fullTrack_, b, &info);
            const bool key = (info.flags & 1) != 0;   // BUFFER_FLAG_KEY_FRAME
            std::string open_;
            { std::lock_guard<std::mutex> g(m_); if (key && !nextPath_.empty() && info.presentationTimeUs >= clipFrom_) { open_ = nextPath_; nextPath_.clear(); } }
            if (!open_.empty()) { openClip(open_); clipBase_ = info.presentationTimeUs; }
            if (clip_) {
                AMediaCodecBufferInfo ci = info; ci.presentationTimeUs -= clipBase_;
                AMediaMuxer_writeSampleData(clip_, (size_t)clipTrack_, b, &ci);
            }
            samples_.fetch_add(1);
        }
        AMediaCodec_releaseOutputBuffer(codec_, (size_t)i, false);
        if (end) break;
    }
}

void CinemaRecorder::finish() {
    if (!codec_) return;
    AMediaCodec_signalEndOfInputStream(codec_);
    if (th_.joinable()) th_.join();
    closeClip();
    if (full_) { AMediaMuxer_stop(full_); AMediaMuxer_delete(full_); full_ = nullptr; }
    if (fullFd_ >= 0) { close(fullFd_); fullFd_ = -1; }
    AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr;
    if (outFmt_) { AMediaFormat_delete(outFmt_); outFmt_ = nullptr; }
    if (surf_ != EGL_NO_SURFACE) { eglDestroySurface(dpy_, surf_); surf_ = EGL_NO_SURFACE; }
    if (win_) { ANativeWindow_release(win_); win_ = nullptr; }
    fullTrack_ = clipTrack_ = -1;
    LOG("cinema: gravacao fechada, %lld quadros", samples_.load());
}

}  // namespace e0
