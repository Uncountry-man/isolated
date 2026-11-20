// Isolated - Wi-Fi Low Latency Lock (Android): desliga a economia de energia do Wi-Fi enquanto o jogo
// está em primeiro plano. É a maior fonte de jitter em jogos por Wi-Fi. Requer API 29+ e a permissão
// WAKE_LOCK; em aparelhos sem suporte o lock simplesmente não tem efeito.
#pragma once
struct ANativeActivity;

namespace e0 {
bool wifiLowLatencyAcquire(ANativeActivity* activity);  // true se o lock ficou ativo
void wifiLowLatencyRelease(ANativeActivity* activity);
// Modo imersivo: esconde barra de status e de navegação (reaparecem só com gesto, depois somem) e usa a área do notch.
void hideSystemBars(ANativeActivity* a);

}  // namespace e0
