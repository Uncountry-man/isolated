#include "audio.h"
#include "adpcm.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated.audio", __VA_ARGS__)

namespace e0 {

bool Audio::loadWav(AAssetManager* am, const char* name, Clip& out) {
    AAsset* a = AAssetManager_open(am, name, AASSET_MODE_BUFFER);
    if (!a) { LOG("asset %s não encontrado", name); return false; }
    const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a);
    size_t len = (size_t)AAsset_getLength(a);
    bool ok = false;
    if (len > 44 && !std::memcmp(b, "RIFF", 4) && !std::memcmp(b + 8, "WAVE", 4)) {
        size_t o = 12; int rate = 0, ch = 0, bits = 0, fmtTag = 0, align = 0; uint32_t factN = 0;
        while (o + 8 <= len) {
            uint32_t sz; std::memcpy(&sz, b + o + 4, 4);
            if (!std::memcmp(b + o, "fmt ", 4) && o + 8 + 16 <= len) {
                uint16_t tg, c16, al, bt; uint32_t sr; std::memcpy(&tg, b + o + 8, 2); std::memcpy(&c16, b + o + 10, 2); std::memcpy(&sr, b + o + 12, 4);
                std::memcpy(&al, b + o + 20, 2); std::memcpy(&bt, b + o + 22, 2);
                fmtTag = tg; ch = c16; rate = (int)sr; bits = bt; align = al;
            } else if (!std::memcmp(b + o, "fact", 4) && o + 12 <= len) {
                std::memcpy(&factN, b + o + 8, 4);
            } else if (!std::memcmp(b + o, "data", 4) && ch == 1 && fmtTag == 1 && bits == 16) {
                size_t n = std::min((size_t)sz, len - o - 8) / 2;
                out.pcm.resize(n); std::memcpy(out.pcm.data(), b + o + 8, n * 2); out.rate = rate; ok = n > 0; break;
            } else if (!std::memcmp(b + o, "data", 4) && ch == 1 && fmtTag == 0x11 && bits == 4) {   // IMA ADPCM (tools/compress_wav.py)
                decodeImaAdpcm(b + o + 8, std::min((size_t)sz, len - o - 8), align, factN, out.pcm); out.rate = rate; ok = !out.pcm.empty(); break;
            }
            o += 8 + sz + (sz & 1);
        }
    }
    AAsset_close(a);
    if (!ok) LOG("%s: WAV inválido (precisa ser mono PCM 16 bit ou IMA ADPCM)", name);
    return ok;
}

bool Audio::init(AAssetManager* am) {
    if (!am || stream_) return stream_ != nullptr;
    static const char* kSurf[SurfCount] = {"hard", "grass", "sand"};
    int total = 0;
    for (int s = 0; s < SurfCount; ++s)
        for (int i = 0; i < 16; ++i) {
            char n[64]; std::snprintf(n, sizeof n, "sfx/step_%s_%d.wav", kSurf[s], i);
            Clip c; if (!loadWav(am, n, c)) break;
            steps_[s].push_back(std::move(c)); ++total;
        }
    for (int i = 0; i < 16; ++i) { char n[64]; std::snprintf(n, sizeof n, "sfx/chop_%d.wav", i); Clip c; if (!loadWav(am, n, c)) break; chops_.push_back(std::move(c)); }
    for (int i = 0; i < 16; ++i) { char n[64]; std::snprintf(n, sizeof n, "sfx/pick_%d.wav", i); Clip c; if (!loadWav(am, n, c)) break; picks_.push_back(std::move(c)); }
    bool okSwim = loadWav(am, "sfx/swim.wav", swim_);
    bool okO = loadWav(am, "sfx/amb_ocean.wav", ambOcean_), okJ = loadWav(am, "sfx/amb_jungle.wav", ambJungle_);
    LOG("sons carregados: %d passos, nado %d, oceano %d, selva %d", total, (int)okSwim, (int)okO, (int)okJ);
    synthGunSounds();
    if (okO) loops_[0].c = &ambOcean_;
    if (okJ) loops_[1].c = &ambJungle_;
    return openStream();
}

bool Audio::openStream() {
    AAudioStreamBuilder* bld = nullptr;
    if (AAudio_createStreamBuilder(&bld) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(bld, 2);
    AAudioStreamBuilder_setDataCallback(bld, &Audio::onData, this);
    AAudioStreamBuilder_setErrorCallback(bld, &Audio::onError, this);
    AAudioStream* st = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &st);
    AAudioStreamBuilder_delete(bld);
    if (r != AAUDIO_OK) { LOG("openStream falhou: %s", AAudio_convertResultToText(r)); return false; }
    outRate_ = AAudioStream_getSampleRate(st);
    AAudioStream_setBufferSizeInFrames(st, AAudioStream_getFramesPerBurst(st) * 2);
    stream_ = st;
    r = AAudioStream_requestStart(st);
    LOG("stream aberto: %d Hz, burst %d, início: %s", outRate_, AAudioStream_getFramesPerBurst(st), AAudio_convertResultToText(r));
    return r == AAUDIO_OK;
}

void Audio::shutdown() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
}

void Audio::pause() { paused_ = true; if (stream_) AAudioStream_requestPause(stream_); }
void Audio::resume() {
    paused_ = false;
    if (restart_.exchange(false)) { shutdown(); openStream(); return; }
    if (stream_) AAudioStream_requestStart(stream_);
}

void Audio::onError(AAudioStream*, void* self, aaudio_result_t err) {
    auto* a = (Audio*)self;
    if (err != AAUDIO_ERROR_DISCONNECTED) return;   // fones removidos / rota mudou: reabre numa thread (não dá para fechar dentro do callback)
    LOG("stream desconectado: reabrindo");
    std::thread([a] { a->shutdown(); if (!a->paused_) a->openStream(); else a->restart_ = true; }).detach();
}

aaudio_data_callback_result_t Audio::onData(AAudioStream*, void* self, void* data, int32_t frames) {
    ((Audio*)self)->mix((float*)data, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void Audio::play(const Clip* c, float vol, float pitch, float pan) {
    if (!c || c->pcm.empty() || !stream_) return;
    pan = std::max(-1.0f, std::min(1.0f, pan));
    float ang = (pan + 1.0f) * 0.78539816f;                 // potência constante
    Voice v{c, 0.0, (float)c->rate / (float)outRate_ * pitch, vol * std::cos(ang) * 1.4142f, vol * std::sin(ang) * 1.4142f};
    std::lock_guard<std::mutex> l(mu_);
    if (pending_.size() < 32) pending_.push_back(v);
}

void Audio::playStep(int surface, float vol, float pan) {
    if (surface < 0 || surface >= SurfCount || steps_[surface].empty()) return;
    auto& v = steps_[surface];
    int i = (int)(rnd() * v.size()); if (i >= (int)v.size()) i = (int)v.size() - 1;
    if (v.size() > 1 && i == lastStep_[surface]) i = (i + 1) % (int)v.size();   // nunca repete o mesmo passo seguido
    lastStep_[surface] = i;
    play(&v[(size_t)i], vol * (0.82f + 0.18f * rnd()), 0.93f + 0.14f * rnd(), pan);
}

void Audio::playChop(float vol, float pan) {
    if (chops_.empty()) return;
    int i = (int)(rnd() * chops_.size()); if (i >= (int)chops_.size()) i = (int)chops_.size() - 1;
    if (chops_.size() > 1 && i == lastChop_) i = (i + 1) % (int)chops_.size();
    lastChop_ = i;
    play(&chops_[(size_t)i], vol * (0.9f + 0.1f * rnd()), 0.94f + 0.12f * rnd(), pan);
}

void Audio::playMine(float vol, float pan) {
    if (picks_.empty()) return;
    int i = (int)(rnd() * picks_.size()); if (i >= (int)picks_.size()) i = (int)picks_.size() - 1;
    if (picks_.size() > 1 && i == lastPick_) i = (i + 1) % (int)picks_.size();
    lastPick_ = i;
    play(&picks_[(size_t)i], vol * (0.9f + 0.1f * rnd()), 0.96f + 0.08f * rnd(), pan);
}

// ---- BACAMARTE: sons gerados (ruído filtrado + senóides com envelopes) ----
void Audio::synthGunSounds() {
    const int R = 32000;
    uint32_t seed = 0xC0FFEEu;
    auto noise = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(int32_t)seed / 2147483648.0f; };
    auto make = [&](Clip& c, float dur, auto fn) {
        const int n = (int)(dur * R);
        std::vector<float> f((size_t)n);
        float peak = 1e-6f;
        for (int i = 0; i < n; ++i) { f[(size_t)i] = fn((float)i / (float)R); peak = std::max(peak, std::fabs(f[(size_t)i])); }
        c.rate = R; c.pcm.resize((size_t)n);
        for (int i = 0; i < n; ++i) c.pcm[(size_t)i] = (int16_t)std::lround(f[(size_t)i] / peak * 0.95f * 32767.0f);
    };
    auto shot = [&](Clip& c, float lpBody, float lpTail, float crackK, float dur) {
        float lb = 0.0f, lt = 0.0f, lt2 = 0.0f, ph = 0.0f;
        make(c, dur, [&](float t) {
            const float nz = noise();
            lb += (nz - lb) * lpBody; lt += (nz - lt) * lpTail; lt2 += (lt - lt2) * lpTail;
            const float crack = crackK * nz * std::exp(-t / 0.006f);                     // estalo inicial
            const float body = 1.6f * lb * std::exp(-t / 0.10f) * std::min(1.0f, t / 0.0015f);   // explosão (ruído grave)
            const float f0 = 38.0f + 45.0f * std::exp(-t / 0.05f); ph += 6.2831853f * f0 / (float)R;
            const float boom = 0.9f * std::sin(ph) * std::exp(-t / 0.20f) * std::min(1.0f, t / 0.003f);   // "bum" do sino do bacamarte
            const float tail = 2.2f * lt2 * std::exp(-t / 0.55f);                          // eco/ressonância
            return crack + body + boom + tail;
        });
    };
    shot(gun_, 0.35f, 0.06f, 1.2f, 1.6f);
    shot(gunFar_, 0.08f, 0.03f, 0.0f, 1.8f);
    {   // estalo metálico curto (cão / caçoleta / gatilho)
        float hp = 0.0f, prev = 0.0f;
        make(click_, 0.09f, [&](float t) {
            const float nz = noise(); hp = 0.85f * (hp + nz - prev); prev = nz;
            return 0.7f * hp * std::exp(-t / 0.004f) + 0.5f * std::sin(6.2831853f * 2900.0f * t) * std::exp(-t / 0.018f) + 0.3f * std::sin(6.2831853f * 4700.0f * t) * std::exp(-t / 0.010f);
        });
    }
    {   // vareta: raspa no cano e soca (2 batidas surdas)
        float bp = 0.0f, bp2 = 0.0f;
        make(ramrod_, 0.62f, [&](float t) {
            const float nz = noise(); bp += (nz - bp) * 0.25f; bp2 += (bp - bp2) * 0.25f;
            const float scrape = (bp - bp2) * (t < 0.30f ? std::sin(3.14159f * t / 0.30f) : 0.0f) * 1.2f;
            auto thud = [&](float t0) { float u = t - t0; return u < 0.0f ? 0.0f : std::sin(6.2831853f * 190.0f * u) * std::exp(-u / 0.03f) + 0.6f * nz * std::exp(-u / 0.008f); };
            return scrape + 0.9f * thud(0.34f) + 0.7f * thud(0.48f);
        });
    }
}

void Audio::playGun(float vol, float pan, float far) {
    far = std::max(0.0f, std::min(1.0f, far));
    if (far < 0.6f) play(&gun_, vol * (1.0f - 0.5f * far), 0.96f + 0.08f * rnd(), pan);
    if (far > 0.25f) play(&gunFar_, vol * std::min(1.0f, far * 1.3f) * 0.9f, 0.85f + 0.06f * rnd(), pan);
}
void Audio::playClick(float vol, float pitch, float pan) { play(&click_, vol, pitch * (0.97f + 0.06f * rnd()), pan); }
void Audio::playRamrod(float vol, float pan) { play(&ramrod_, vol, 0.95f + 0.1f * rnd(), pan); }

void Audio::playSwim(float vol, float pan) { play(&swim_, vol, 0.95f + 0.1f * rnd(), pan); }

void Audio::setAmbience(float ocean, float jungle) {
    loops_[0].target.store(std::max(0.0f, std::min(1.0f, ocean)));
    loops_[1].target.store(std::max(0.0f, std::min(1.0f, jungle)));
}

void Audio::mix(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    if (paused_) return;
    if (mu_.try_lock()) { for (const Voice& v : pending_) if (active_.size() < 40) active_.push_back(v); pending_.clear(); mu_.unlock(); }
    const float master = master_.load();
    // ambientes: ganho suavizado (~0,6 s), leitura em loop com interpolação linear
    for (Loop& L : loops_) {
        if (!L.c || L.c->pcm.empty()) continue;
        const float tgt = L.target.load();
        if (L.cur < 0.0005f && tgt < 0.0005f) continue;
        const double step = (double)L.c->rate / outRate_;
        const double n = (double)L.c->pcm.size();
        const float k = 1.0f - std::exp(-1.0f / (0.6f * outRate_));
        for (int i = 0; i < frames; ++i) {
            L.cur += (tgt - L.cur) * k;
            int i0 = (int)L.pos, i1 = i0 + 1 >= (int)n ? 0 : i0 + 1;
            float fr = (float)(L.pos - i0);
            float s = (L.c->pcm[(size_t)i0] * (1.0f - fr) + L.c->pcm[(size_t)i1] * fr) * (1.0f / 32768.0f) * L.cur * master;
            out[2 * i] += s; out[2 * i + 1] += s;
            L.pos += step; if (L.pos >= n) L.pos -= n;
        }
    }
    for (size_t vi = 0; vi < active_.size();) {
        Voice& v = active_[vi];
        const int n = (int)v.c->pcm.size();
        int i = 0;
        for (; i < frames; ++i) {
            int i0 = (int)v.pos;
            if (i0 + 1 >= n) break;
            float fr = (float)(v.pos - i0);
            float s = (v.c->pcm[(size_t)i0] * (1.0f - fr) + v.c->pcm[(size_t)i0 + 1] * fr) * (1.0f / 32768.0f) * master;
            out[2 * i] += s * v.gl; out[2 * i + 1] += s * v.gr;
            v.pos += v.step;
        }
        if (i < frames) active_[vi] = active_.back(), active_.pop_back(); else ++vi;
    }
    // trilha sonora: lê do anel (reamostra linear se o stream não for 48 kHz), ganho suavizado
    if (!musRing_.empty()) {
        const double step = (double)musRate_.load() / outRate_;
        const float tgt = musVol_.load() * master;
        const float k = 1.0f - std::exp(-1.0f / (0.5f * outRate_));
        uint64_t r = musR_.load(std::memory_order_relaxed);
        const uint64_t w = musW_.load(std::memory_order_acquire);
        for (int i = 0; i < frames; ++i) {
            if (r + 2 >= w) break;   // anel vazio (decodificando): silêncio
            musCur_ += (tgt - musCur_) * k;
            const float f = (float)musFrac_;
            const int16_t* a = &musRing_[(r & (kMusRing - 1)) * 2];
            const int16_t* b = &musRing_[((r + 1) & (kMusRing - 1)) * 2];
            out[2 * i] += (a[0] + (b[0] - a[0]) * f) * (1.0f / 32768.0f) * musCur_;
            out[2 * i + 1] += (a[1] + (b[1] - a[1]) * f) * (1.0f / 32768.0f) * musCur_;
            musFrac_ += step;
            const uint64_t adv = (uint64_t)musFrac_; musFrac_ -= (double)adv; r += adv;
        }
        musR_.store(r, std::memory_order_release);
    }
    for (int i = 0; i < frames * 2; ++i) {   // limitador suave
        float x = out[i];
        out[i] = x > 0.9f ? 0.9f + 0.1f * std::tanh((x - 0.9f) * 10.0f) : (x < -0.9f ? -0.9f - 0.1f * std::tanh((-x - 0.9f) * 10.0f) : x);
    }
}

// ---- TRILHA SONORA (streaming) ----
void Audio::startMusic(AAssetManager* am) {
    if (!am || musThread_.joinable()) return;
    musRing_.assign((size_t)kMusRing * 2, 0);
    musStop_ = false;
    musThread_ = std::thread([this, am] { musicLoop(am); });
}

void Audio::musicLoop(AAssetManager* am) {
    static const char* kTracks[] = {"music/ocean.ogg", "music/captains.ogg", "music/treasure.ogg", "music/ghost.ogg"};
    const int n = (int)(sizeof kTracks / sizeof kTracks[0]);
    int order[8]; for (int i = 0; i < n; ++i) order[i] = i;
    uint32_t seed = 0x9E3779B9u ^ (uint32_t)(uintptr_t)this;
    int fails = 0, idx = 0;
    while (!musStop_) {
        if (idx == 0) {   // embaralha a cada volta 
            for (int i = n - 1; i > 0; --i) { seed = seed * 1664525u + 1013904223u; int j = (int)((seed >> 8) % (uint32_t)(i + 1)); std::swap(order[i], order[j]); }
        }
        if (musicDecode(am, kTracks[order[idx]], seed)) fails = 0; else if (++fails >= n) break;   // nada toca: desiste
        idx = (idx + 1) % n;
    }
}

bool Audio::musicDecode(AAssetManager* am, const char* name, uint32_t) {
    AAsset* a = AAssetManager_open(am, name, AASSET_MODE_STREAMING);
    if (!a) { LOG("música %s: não achei", name); return false; }
    off64_t off = 0, len = 0;
    int fd = AAsset_openFileDescriptor64(a, &off, &len);
    if (fd < 0) { LOG("música %s: asset comprimido (use aapt2 -0 ogg)", name); AAsset_close(a); return false; }
    AMediaExtractor* ex = AMediaExtractor_new();
    AMediaCodec* codec = nullptr;
    bool ok = false;
    if (AMediaExtractor_setDataSourceFd(ex, fd, off, len) == AMEDIA_OK && AMediaExtractor_getTrackCount(ex) > 0) {
        AMediaFormat* fmt = AMediaExtractor_getTrackFormat(ex, 0);
        const char* mime = nullptr;
        AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime);
        if (mime && !std::strncmp(mime, "audio/", 6)) {
            AMediaExtractor_selectTrack(ex, 0);
            codec = AMediaCodec_createDecoderByType(mime);
            if (codec && AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) == AMEDIA_OK && AMediaCodec_start(codec) == AMEDIA_OK) ok = true;
        }
        AMediaFormat_delete(fmt);
    }
    if (!ok) LOG("música %s: decodificador indisponível", name);
    else {
        int ch = 2, rate = 48000;
        bool inEos = false, outEos = false;
        uint64_t frameNo = 0;                    // para o fade-in
        const uint64_t fadeIn = 96000;           // ~2 s
        while (!musStop_ && !outEos) {
            if (!inEos) {
                ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 5000);
                if (ib >= 0) {
                    size_t cap = 0; uint8_t* buf = AMediaCodec_getInputBuffer(codec, (size_t)ib, &cap);
                    ssize_t sz = AMediaExtractor_readSampleData(ex, buf, cap);
                    if (sz < 0) { AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); inEos = true; }
                    else { AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, (size_t)sz, (uint64_t)AMediaExtractor_getSampleTime(ex), 0); AMediaExtractor_advance(ex); }
                }
            }
            AMediaCodecBufferInfo info;
            ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
            if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
                AMediaFormat_delete(of);
                if (ch < 1) ch = 2;
                musRate_.store(rate > 8000 ? rate : 48000);
            } else if (ob >= 0) {
                size_t cap = 0; const uint8_t* buf = AMediaCodec_getOutputBuffer(codec, (size_t)ob, &cap);
                if (buf && info.size > 0) {
                    const int16_t* pcm = (const int16_t*)(buf + info.offset);
                    const size_t frames = (size_t)info.size / (sizeof(int16_t) * (size_t)ch);
                    for (size_t f = 0; f < frames && !musStop_;) {
                        const uint64_t w = musW_.load(std::memory_order_relaxed), r = musR_.load(std::memory_order_acquire);
                        if (w - r >= kMusRing - 1) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }   // anel cheio (ou pausado)
                        float g = frameNo < fadeIn ? (float)frameNo / (float)fadeIn : 1.0f;
                        const int16_t* s = pcm + f * (size_t)ch;
                        int16_t* d = &musRing_[(w & (kMusRing - 1)) * 2];
                        d[0] = (int16_t)(s[0] * g); d[1] = (int16_t)(s[ch > 1 ? 1 : 0] * g);
                        musW_.store(w + 1, std::memory_order_release);
                        ++f; ++frameNo;
                    }
                }
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outEos = true;
                AMediaCodec_releaseOutputBuffer(codec, (size_t)ob, false);
            }
        }
        // ~1 s de silêncio entre as faixas
        for (int i = 0; i < 48000 && !musStop_;) {
            const uint64_t w = musW_.load(std::memory_order_relaxed), r = musR_.load(std::memory_order_acquire);
            if (w - r >= kMusRing - 1) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            int16_t* d = &musRing_[(w & (kMusRing - 1)) * 2]; d[0] = d[1] = 0;
            musW_.store(w + 1, std::memory_order_release); ++i;
        }
    }
    if (codec) { AMediaCodec_stop(codec); AMediaCodec_delete(codec); }
    AMediaExtractor_delete(ex);
    ::close(fd);
    AAsset_close(a);
    return ok;
}

}  // namespace e0
