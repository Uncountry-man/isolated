// Isolated - CONTA DO JOGADOR (tela de login: Google ou visitante). Ver CLAUDE.md, seção "Login (Google / visitante)".
//
// O C++ guarda a sessão em <dados internos>/account.cfg e fala com o Java (android/java/com/rungo/isolatedMT/Auth.java) por JNI, lendo
// EVENTOS numa fila (mesmo esquema da monetização). O Java usa o FIREBASE AUTH quando o app foi compilado com android/google-services.json
// (build.sh liga o Firebase sozinho: android/java_firebase/AuthFirebase.java + bibliotecas); sem ele:
//   - Google  -> falha "NAO_CONFIGURADO" (a tela mostra "LOGIN COM GOOGLE AINDA NÃO ESTÁ ATIVO");
//   - visitante -> conta LOCAL (uid "local-<16 hex>"), só neste aparelho.
// E-mail e senha (Firebase): signInWithEmailAndPassword; visitante do Firebase tenta ligar o e-mail à conta anônima antes.
// Com Firebase: Google = Credential Manager + FirebaseAuth.signInWithCredential; visitante = FirebaseAuth.signInAnonymously
// (o progresso pode ir para a nuvem e, se depois entrar com Google, a conta anônima é LIGADA à do Google, sem perder nada).
// idToken() = token JWT do Firebase (validade ~1 h; refreshToken() pede outro) para o SERVIDOR do jogo validar quem é o jogador no futuro.
#pragma once
#include <cstdint>
#include <string>
struct ANativeActivity;

namespace e0 {

class Auth {
public:
    enum Provider : uint8_t { None = 0, Guest = 1, Google = 2, Email = 3 };   // Email = Firebase e-mail/senha (conta fb:<uid>, como o Google)
    struct Account {
        Provider provider = None;
        bool cloud = false;            // true = conta do Firebase (uid do Firebase); false = visitante local
        std::string uid, name, email;
    };
    // Resultado de um pedido de login (poll)
    enum Result { Nothing = 0, SignedIn, Failed };
    enum Fail { FailNone = 0, FailNotConfigured, FailError, FailCanceled, FailNetwork, FailCredential, FailEmailInvalid, FailTooMany };

    void init(ANativeActivity* act, const std::string& dataDir);
    void shutdown();
    bool signedIn() const { return acc_.provider != None; }
    const Account& account() const { return acc_; }
    bool cloudAvailable() const { return cloud_; }   // o app tem Firebase configurado

    void signInGoogle();
    void signInGuest();
    // LOGIN POR E-MAIL E SENHA (Firebase). A senha só é repassada ao Java; não é guardada nem registrada.
    void signInEmail(const std::string& email, const std::string& pass);
    void signOut();
    void refreshToken();                       // pede um idToken novo ao Firebase (chega por poll como SignedIn)
    const std::string& idToken() const { return token_; }
    // uid do visitante LOCAL que existia antes de entrar numa conta do Firebase (o servidor migra os dados dele 1 vez para a conta nova)
    const std::string& prevLocal() const { return prevLocal_; }

    // Lê a fila do Java. SignedIn: account() já foi atualizada e salva. Failed: fail = motivo.
    Result poll(Fail& fail);

private:
    ANativeActivity* act_ = nullptr;
    void* cls_ = nullptr;   // jclass global de com.rungo.isolatedMT.Auth
    bool cloud_ = false;
    bool pendingGuest_ = false;
    Fail pendingFail_ = FailNone;   // falha imediata (sem Firebase) entregue no próximo poll
    std::string path_, token_, prevLocal_;
    Account acc_;
    void load();
    void save() const;
    void makeLocalGuest();
    void call(const char* name);
};

}  // namespace e0
