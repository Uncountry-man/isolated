// Isolated - áudio (AAudio): efeitos pontuais (passos, nado) + ambientes em loop (oceano, selva). Sem Java.
//  * Os sons ficam em PCM 16 bit na memória (assets/sfx/*.wav, gerados por tools/export_audio.py).
//  * O callback do AAudio mistura vozes (com tom e pan) e dois ambientes cujo volume é suavizado; o jogo só manda comandos.
#pragma once
#include <android/asset_manager.h>
#include <aaudio/AAudio.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

namespace e0 {

class Audio {
public:
    enum Surface { SurfHard = 0, SurfGrass, SurfSand, SurfCount };
    ~Audio() { musStop_ = true; if (musThread_.joinable()) musThread_.join(); shutdown(); }
    bool init(AAssetManager* am);        // carrega os WAVs e abre o stream
    void shutdown();
    void pause();                        // app em segundo plano
    void resume();

    // vol 0..1+, pan -1 (esq) .. +1 (dir)
    void playStep(int surface, float vol, float pan);
    void playSwim(float vol, float pan);
    void playChop(float vol, float pan);   // machadinha na madeira
    void playMine(float vol, float pan);   // picareta na pedra
    // BACAMARTE (sons SINTETIZADOS no init, sem asset): estrondo do tiro (far = 0 perto .. 1 longe: mais grave e abafado), estalos metálicos
    // (cão, caçoleta, gatilho em seco; pitch muda o timbre) e a vareta socando a carga (recarga)
    void playGun(float vol, float pan, float far);
    void playClick(float vol, float pitch, float pan);
    void playRamrod(float vol, float pan);
    void setAmbience(float ocean, float jungle);   // alvos 0..1 (suavizados)
    // TRILHA SONORA: assets/music/*.ogg (Opus, ~0,9 MB cada) decodificadas em STREAMING por uma thread (AMediaCodec); nada fica em PCM na memória
    void startMusic(AAssetManager* am);
    void setMusicVolume(float v) { musVol_.store(v); }
    void setMaster(float v) { master_.store(v); }
    bool ready() const { return stream_ != nullptr; }

private:
    struct Clip { std::vector<int16_t> pcm; int rate = 32000; };
    struct Voice { const Clip* c; double pos; float step, gl, gr; };
    struct Loop { const Clip* c = nullptr; double pos = 0; float cur = 0; std::atomic<float> target{0}; };
    bool loadWav(AAssetManager* am, const char* name, Clip& out);
    void play(const Clip* c, float vol, float pitch, float pan);
    static aaudio_data_callback_result_t onData(AAudioStream*, void* self, void* data, int32_t frames);
    static void onError(AAudioStream*, void* self, aaudio_result_t err);
    void mix(float* out, int frames);
    bool openStream();

    // música: anel estéreo int16 (2 s) escrito pela thread decodificadora e lido no callback
    static constexpr uint32_t kMusRing = 1u << 17;   // frames (potência de 2)
    std::vector<int16_t> musRing_;
    std::atomic<uint64_t> musW_{0}, musR_{0};
    std::atomic<int> musRate_{48000};
    std::atomic<float> musVol_{0.4f};
    std::atomic<bool> musStop_{false};
    std::thread musThread_;
    double musFrac_ = 0;
    float musCur_ = 0;
    void musicLoop(AAssetManager* am);
    bool musicDecode(AAssetManager* am, const char* name, uint32_t seed);

    AAudioStream* stream_ = nullptr;
    int outRate_ = 48000;
    std::vector<Clip> steps_[SurfCount];
    Clip swim_, ambOcean_, ambJungle_, gun_, gunFar_, click_, ramrod_;
    void synthGunSounds();
    std::vector<Clip> chops_, picks_;
    int lastChop_ = -1, lastPick_ = -1;
    Loop loops_[2];                       // 0 = oceano, 1 = selva
    std::vector<Voice> active_;           // só mexe no callback
    std::vector<Voice> pending_;          // game -> callback
    std::mutex mu_;
    std::atomic<float> master_{0.9f};
    std::atomic<bool> paused_{false}, restart_{false};
    int lastStep_[SurfCount] = {-1, -1, -1};
    uint32_t rng_ = 12345u;
    float rnd() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) * (1.0f / 16777216.0f); }
};

}  // namespace e0
