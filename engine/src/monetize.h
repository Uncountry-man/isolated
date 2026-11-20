// Isolated - AdMob (anúncios) e Google Play Billing (compras) pelo Java (android/java/com/rungo/isolatedMT/Monetize.java).
// O C++ só chama métodos estáticos por JNI e lê EVENTOS de texto ("TIPO|a|b") numa fila; nada bloqueia o jogo.
// Ver CLAUDE.md, seção "Monetização".
#pragma once
#include <string>
#include <vector>
struct ANativeActivity;

namespace e0 {

class Monetize {
public:
    enum Status { RewardedReady = 1, InterstitialReady = 2, BillingReady = 4, AdShowing = 8, TestMode = 16 };
    // skus = "id:c,id:c" (c = 1 consumível, 0 permanente). test = IDs de anúncio de TESTE + compra simulada se o produto não existir na Play.
    bool init(ANativeActivity* a, bool test, const std::string& skus);
    void shutdown();
    bool active() const { return cls_ != nullptr; }
    // Lê a fila de eventos e o status (chamar ~4x/s). Devolve false se não está ativo.
    bool poll(std::vector<std::string>& events);
    int status() const { return status_; }
    void showRewarded(int tag);
    void showInterstitial();
    void buy(const std::string& sku);

private:
    ANativeActivity* act_ = nullptr;
    void* cls_ = nullptr;   // jclass global
    int status_ = 0;
};

}  // namespace e0
