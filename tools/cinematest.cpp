// Testes do roteiro do modo cinema (engine/src/cinema.h): build/cinematest [arquivo.cine ...] valida também os roteiros passados.
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../engine/src/cinema.h"
using namespace e0::cine;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FALHOU: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    Script S; std::string err;
    const char* txt = "nome teste\nvideo 1280 720 30 20 # comentario\nplano \"Um plano\" 4\n camera navio 1\n alvo mundo\n chave 0 0 0 0 1 1 1 60\n chave 2 10 0 0 1 1 1 70 0.2\n chave 4 10 10 0 1 1 1 80\n"
                      "fade_in 1\nfade_out 1\nhora 18:00\nnavio 2 10 20 90 3 0.1\nnavio 0 some\ntitulo 1 2 \"A B\"\ntiro 1 1 -1 4 60\ntremor 1 2 0.5\n"
                      "plano B 2\n orbita 0 0 2 10 20 5 6 0 90 60 50\n suave\n";
    CHECK(parse(txt, S, err), "roteiro valido: %s", err.c_str());
    CHECK(S.name == "teste" && S.w == 1280 && S.fps == 30 && S.shots.size() == 2 && S.total() == 6.0f, "cabecalho");
    const Shot& A = S.shots[0];
    CHECK(A.name == "Um plano" && A.camSpace == 1 && A.lookSpace == -1 && A.keys.size() == 3, "plano");
    CHECK(std::fabs(A.hour - 900.0f) < 0.01f, "18:00 = por do sol (%.1f)", A.hour);
    CHECK(std::fabs(hourToCycle("06:00")) < 0.01f && std::fabs(hourToCycle("12:00") - 450.0f) < 0.01f && std::fabs(hourToCycle("00:00") - 1350.0f) < 0.01f, "hora");
    CHECK(A.ships[2].set && std::fabs(A.ships[2].yaw - 1.5708f) < 0.01f && A.ships[0].hide, "navios");
    CHECK(A.titles[0].text == "A B", "titulo com espaco");
    Eval e0 = evalShot(A, 0), e2 = evalShot(A, 2), e4 = evalShot(A, 4), m = evalShot(A, 1);
    CHECK(e0.p[0] == 0 && std::fabs(e2.p[0] - 10) < 1e-3f && std::fabs(e4.p[1] - 10) < 1e-3f, "passa pelas chaves");
    CHECK(m.p[0] > 2 && m.p[0] < 8 && std::fabs(evalShot(A, 2).roll - 0.2f) < 1e-3f, "interpola");
    float last = -1, maxStep = 0; for (int i = 0; i <= 400; ++i) { float x = evalShot(A, i * 0.01f).p[0]; if (last >= 0) maxStep = std::max(maxStep, std::fabs(x - last)); last = x; }
    CHECK(maxStep < 0.2f, "sem saltos (%.3f)", maxStep);
    const Shot& B = S.shots[1];
    Eval b0 = evalShot(B, 0), b1 = evalShot(B, 2);
    CHECK(std::fabs(b0.p[0] - 10) < 1e-3f && std::fabs(b1.p[1] - 20) < 1e-2f && std::fabs(b1.p[2] - 8) < 1e-3f && std::fabs(b1.fov - 50) < 1e-3f, "orbita (%.2f %.2f %.2f)", b0.p[0], b1.p[1], b1.p[2]);
    CHECK(fadeAt(A, 0) == 1 && fadeAt(A, 2) == 0 && fadeAt(A, 4) == 1 && fadeAt(A, 3.5f) > 0, "fade");
    CHECK(shakeAmp(A, 1.0f) > 0.4f && shakeAmp(A, 3.5f) == 0, "tremor");
    { Script X; CHECK(!parse("plano A 2\n voar\n", X, err) && err.find("linha 2") != std::string::npos, "erro aponta a linha: %s", err.c_str()); }
    { Script X; CHECK(!parse("plano A 2\n hora 12:00\n", X, err), "plano sem camera e recusado"); }
    { Script X; CHECK(!parse("plano A 2\n chave 1 0 0 0 0 0 0 60\n chave 1 0 0 0 0 0 0 60\n", X, err), "chaves fora de ordem"); }
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i]); std::stringstream ss; ss << f.rdbuf(); Script X;
        CHECK(f.good() && parse(ss.str(), X, err), "%s: %s", argv[i], err.c_str());
        std::printf("%s: %zu planos, %.1f s\n", argv[i], X.shots.size(), X.total());
    }
    std::printf(fails ? "%d FALHAS\n" : "TODOS OS TESTES DO CINEMA PASSARAM\n", fails);
    return fails ? 1 : 0;
}
