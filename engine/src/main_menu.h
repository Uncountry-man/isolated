// Isolated - MENU PRINCIPAL (estilo AAA, tema pirata): arte de fundo + título dourado + itens no canto esquerdo (JOGAR, CONVIDAR AMIGOS,
// CONFIGURAÇÕES, CRÉDITOS, SAIR) + ícones das comunidades + cartão de STATUS DO SERVIDOR (ping real) no canto superior direito.
// Autônomo como a LoadingScreen (programa GL próprio; sprites de texto vêm do atlas assets/menu.e0i gerado por tools/make_menu.py, que tem
// acentos). Adota a textura da arte da LoadingScreen (sem duplicar na GPU). Ver CLAUDE.md, seção "Menu principal".
#pragma once
#include <android/asset_manager.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
struct ANativeActivity;

namespace e0 {

class MainMenu {
public:
    // Character = abrir a tela PERSONAGEM (o motor desenha por cima; o menu fica parado). LoginGoogle/LoginGuest = botões da TELA DE LOGIN;
    // Logout = SAIR DA CONTA nas configurações (o motor cuida da conta em Auth e chama showLogin/loginDone/loginMessage).
    enum class Action { None, Play, Quit, Character, LoginGoogle, LoginGuest, Logout, Team };   // Team = tela TRIPULAÇÃO (o motor desenha por cima)
    struct Options {
        int quality = -1;      // -1 AUTO, 0..2 fixa
        int grade = 1;         // 0 OFF, 1 SUAVE, 2 FORTE
        bool sound = true;
        std::string server;    // IPv4 ou IPv4:porta digitado pelo jogador; vazio = servidor padrão do app
    };

    MainMenu();
    ~MainMenu();
    // Contexto GL atual. imgTex/imgW/imgH = arte de fundo já enviada à GPU (o menu passa a ser dono dela).
    // defaultServer = servidor embutido no app ("ip" ou "ip:porta"; vazio = build offline).
    bool init(AAssetManager* am, ANativeActivity* act, unsigned imgTex, int imgW, int imgH, const std::string& defaultServer);
    void shutdown();
    bool ready() const { return prog_ != 0; }

    void update(float dt);
    void draw(int w, int h);
    // type: 0 = toque começou, 1 = moveu, 2 = soltou, 3 = cancelado (coordenadas em pixels)
    void touch(int type, float x, float y);
    // Ação pronta (JOGAR só depois do fade-out; SAIR na hora). Devolve None enquanto não há.
    Action poll();
    // Botão VOLTAR do Android: dentro de CONFIGURAÇÕES/CRÉDITOS volta ao menu principal (canGoBack); no menu principal o app fecha normalmente.
    bool canGoBack() const { return page_ != PageMain && page_ != PageLogin && !inputOpen_ && !leaving_; }
    void goBack() { if (canGoBack()) goPage(PageMain, page_ == PageCredits ? HitCredits : HitSettings); }

    // "ip[:porta]" -> ip e porta (porta padrão se faltar). false se o IPv4 for inválido.
    static bool parseServer(const std::string& s, std::string& ip, int& port);

    bool charNow_ = false;         // PERSONAGEM tocado: poll() devolve Action::Character uma vez
    bool teamNow_ = false;         // TRIPULAÇÃO tocado: poll() devolve Action::Team uma vez
    // ---- TELA DE LOGIN (página antes do menu principal) ----
    void showLogin() { goPage(PageLogin, HitLoginGoogle); loginBusy_ = false; loginMsg_ = 0; }
    bool onLogin() const { return page_ == PageLogin; }
    void loginBusy(bool b) { loginBusy_ = b; if (b) loginMsg_ = 0; }
    void loginMessage(int m) { loginMsg_ = m; loginMsgT_ = 5.0f; loginBusy_ = false; }   // 1 Google não configurado, 2 erro, 3 cancelado, 4 sem internet
    void loginDone() { loginBusy_ = false; loginMsg_ = 0; goPage(PageMain, HitPlay); }
    int account = 0;               // conta em uso (o motor preenche): 0 nenhuma, 1 visitante, 2 Google
    Options opts;                  // o motor preenche antes de abrir e lê quando optsDirty
    bool optsDirty = false;

private:
    struct Rect { float x = 0, y = 0, w = 0, h = 0; bool has(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; } };
    enum Page { PageMain, PageSettings, PageCredits, PageLogin };
    enum Hit { HitNone = -1, HitPlay = 0, HitInvite, HitSettings, HitCredits, HitQuit, HitSocial0 = 5, HitSocial1, HitSocial2, HitSocial3, HitCharacter = 9,
               HitSetQuality = 10, HitSetFilter, HitSetSound, HitSetServer, HitSetBack, HitLoginGoogle = 15, HitLoginGuest, HitSetAccount, HitTeam = 18, HitCount = 20 };

    // Sondagem do servidor: conecta no TCP (o mesmo que baixa o mundo) a cada ~4 s numa thread e mede o tempo. Não bloqueia o desenho.
    struct Probe {
        std::thread th;
        std::atomic<bool> stop{false};
        std::mutex mu;
        std::string ip; int port = 0;          // alvo atual (ip vazio = sem servidor, build local)
        std::atomic<int> version{0};           // muda quando o alvo muda (acorda a espera)
        std::atomic<int> state{0};             // 0 verificando, 1 online, 2 offline, 3 modo local
        std::atomic<int> ms{0};
    };
    std::unique_ptr<Probe> probe_;
    void startProbe();
    void stopProbe();
    void retarget();                          // aponta a sondagem para o servidor efetivo
    std::string effectiveServer() const;      // override ou padrão
    std::string displayServer() const;        // sem ":7777"

    unsigned prog_ = 0, vao_ = 0, texImg_ = 0, texAtlas_ = 0, texLogin_ = 0;   // texLogin_ = arte da TELA DE LOGIN (assets/scene_deck.e0i, 1280x720)
    int imgW_ = 0, imgH_ = 0;
    int uRect_ = -1, uScreen_ = -1, uMode_ = -1, uColor_ = -1, uTime_ = -1, uUv_ = -1, uBox_ = -1, uColor2_ = -1;
    bool loginBusy_ = false, loginGoogleNow_ = false, loginGuestNow_ = false, logoutNow_ = false;
    int loginMsg_ = 0; float loginMsgT_ = 0;
    void button(float x, float y, float bw, float bh, float r, float g, float b, float a, float br, float bg, float bb, float ba, float radius, float border);   // botão arredondado nítido (modo 6)
    ANativeActivity* act_ = nullptr;
    std::string defaultServer_;

    float t_ = 0, pageT_ = 1.0f, soonT_ = 0, badT_ = 0, leaveT_ = 0;
    int soonIdx_ = 0;              // ícone que mostrou o aviso "EM BREVE"
    bool inputOpen_ = false;       // caixa de texto do endereço do servidor aberta (Java)
    int page_ = PageMain;
    bool leaving_ = false, quitNow_ = false;
    int sel_ = HitPlay, pressed_ = HitNone;
    int w_ = 0, h_ = 0;
    Rect hit_[HitCount];           // retângulos de toque do último quadro (índice = Hit)
    bool hitOn_[HitCount] = {};

    void activate(int hit);
    void goPage(int page, int sel);
    void pollInput();
    void sprite(int id, float x, float y, float scale, float r, float g, float b, float a);   // canto superior esquerdo
    void spriteC(int id, float cx, float cy, float scale, float r, float g, float b, float a);
    float textWidth(const std::string& digits, float scale) const;
    void digits(const std::string& s, float x, float y, float scale, float r, float g, float b, float a);   // só 0-9 . :
    void quad(float x, float y, float qw, float qh);
};

}  // namespace e0
