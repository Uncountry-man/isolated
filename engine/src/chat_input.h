// Isolated - caixa de texto do CHAT (Java: GameActivity.showChatInput/takeInputResult, por JNI). Abre o teclado do sistema.
#pragma once
#include <string>
struct ANativeActivity;

namespace e0 {
bool chatInputOpen(ANativeActivity* a, const char* hint);          // abre a caixa (e força o teclado); false se o JNI falhou
bool chatInputPoll(ANativeActivity* a, std::string& out);          // true quando acabou; out = texto ("" = vazio, "\x01" = cancelado)
bool shareText(ANativeActivity* a, const std::string& utf8);     // folha de compartilhamento do Android (texto em UTF-8)
std::string pushToken(ANativeActivity* a);                       // token do Firebase Cloud Messaging ("" = ainda não / sem Firebase)
bool pushPoll(ANativeActivity* a, std::string& out);               // próxima mensagem push para mostrar dentro do jogo ("título\x1fcorpo\x1fação", UTF-8); false = nenhuma
}  // namespace e0
