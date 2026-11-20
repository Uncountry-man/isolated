// Testes da verificação do idToken do Firebase (server/jwt.h): SHA-256, JSON, RSA e regras do JWT.
// Uso: build/authtest DIR_DO_make_test_jwt [jwk_real_do_google]
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include "../server/jwt.h"
static int fails = 0;
static void check(bool c, const char* what) { std::printf("%s %s\n", c ? "[ok]  " : "[FALHA]", what); if (!c) ++fails; }
static std::string rd(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
int main(int argc, char** argv) {
    check(jwt::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 'abc'");
    check(jwt::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 vazio");
    std::string big(1000000, 'a'); check(jwt::sha256Hex(big) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 1M 'a'");
    jwt::Json j; check(jwt::parseJson("{\"a\":[1,2,{\"b\":\"x\\u00e9\"}],\"c\":true}", j) && j.get("a") && j.get("a")->arr.size() == 3, "JSON");
    if (argc < 2) { std::printf("%s\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM"); return fails ? 1 : 0; }
    const std::string d = argv[1], proj = "isolated-multiplayer";
    jwt::KeySet ks; check(ks.refresh(d + "/test.jwk") == 1, "carrega JWK de teste");
    const double now = (double)std::time(nullptr); jwt::Claims c;
    auto t0 = std::clock();
    jwt::Result r = jwt::verifyFirebase(rd(d + "/token_alice.txt"), proj, ks, now, c);
    double ms = (double)(std::clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    check(r == jwt::Result::Ok && c.sub == "alice" && c.provider == "anonymous", "token valido -> sub alice");
    std::printf("      (verificacao RSA-2048 em %.2f ms)\n", ms);
    check(jwt::verifyFirebase(rd(d + "/token_alice_expirado.txt"), proj, ks, now, c) == jwt::Result::Expired, "token expirado recusado");
    check(jwt::verifyFirebase(rd(d + "/token_alice_outroaud.txt"), proj, ks, now, c) == jwt::Result::BadAud, "aud de outro projeto recusado");
    std::string t = rd(d + "/token_alice.txt");
    { std::string x = t; size_t p = x.find('.') + 5; x[p] = x[p] == 'A' ? 'B' : 'A'; check(jwt::verifyFirebase(x, proj, ks, now, c) != jwt::Result::Ok, "payload adulterado recusado"); }
    { std::string x = t; x[x.size() - 3] = x[x.size() - 3] == 'A' ? 'B' : 'A'; check(jwt::verifyFirebase(x, proj, ks, now, c) == jwt::Result::BadSig, "assinatura adulterada recusada"); }
    check(jwt::verifyFirebase("abc.def", proj, ks, now, c) == jwt::Result::Malformed, "token sem 3 partes");
    check(jwt::verifyFirebase(t, "outro", ks, now, c) == jwt::Result::BadIss, "iss de outro projeto");
    jwt::KeySet vazio; check(jwt::verifyFirebase(t, proj, vazio, now, c) == jwt::Result::NoKeys, "sem chaves -> NoKeys");
    if (argc > 2) { jwt::KeySet g; int n = g.refresh(argv[2]); std::printf("      JWK do Google: %d chaves\n", n); check(n >= 1, "JWK real do Google carrega"); check(jwt::verifyFirebase(t, proj, g, now, c) == jwt::Result::UnknownKid, "token de teste com chaves do Google: kid desconhecido"); }
    std::printf("%s\n", fails ? "FALHOU" : "TODOS OS TESTES PASSARAM");
    return fails ? 1 : 0;
}
