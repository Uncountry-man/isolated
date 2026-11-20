// Isolated - verificação do idToken do FIREBASE no servidor (JWT RS256), sem dependências: SHA-256, base64url, JSON mínimo e
// RSA (exponenciação de Montgomery) próprios. As chaves públicas do Google vêm de um arquivo JWK que um timer do systemd baixa
// (https://www.googleapis.com/service_accounts/v1/jwk/securetoken@system.gserviceaccount.com); o servidor relê quando o arquivo muda.
// Checa: alg RS256, kid conhecido, assinatura PKCS#1 v1.5 + SHA-256, iss = https://securetoken.google.com/<projeto>, aud = <projeto>,
// exp no futuro, iat/auth_time não no futuro (com folga), sub (uid) não vazio e <= 128 caracteres.
#pragma once
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jwt {

// ---------------- SHA-256 ----------------
struct Sha256 {
    uint32_t h[8]; uint8_t buf[64]; uint64_t len = 0; size_t bl = 0;
    Sha256() { const uint32_t i[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; std::memcpy(h, i, sizeof h); }
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d; len += n;
        while (n) { size_t k = std::min(n, 64 - bl); std::memcpy(buf + bl, p, k); bl += k; p += k; n -= k; if (bl == 64) { block(buf); bl = 0; } }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = len * 8; uint8_t pad = 0x80; update(&pad, 1); pad = 0;
        while (bl != 56) update(&pad, 1);
        uint8_t L[8]; for (int i = 0; i < 8; ++i) L[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(L, 8);
        for (int i = 0; i < 8; ++i) { out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16); out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i]; }
    }
};
inline void sha256(const void* d, size_t n, uint8_t out[32]) { Sha256 s; s.update(d, n); s.final(out); }
inline std::string sha256Hex(const std::string& s) {
    uint8_t o[32]; sha256(s.data(), s.size(), o);
    static const char* hx = "0123456789abcdef"; std::string r;
    for (uint8_t b : o) { r += hx[b >> 4]; r += hx[b & 15]; }
    return r;
}

// ---------------- base64url ----------------
inline bool b64url(const std::string& in, std::vector<uint8_t>& out) {
    out.clear(); uint32_t acc = 0; int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A'; else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26; else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-' || ch == '+') v = 62; else if (ch == '_' || ch == '/') v = 63; else if (ch == '=') break; else return false;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return true;
}

// ---------------- JSON mínimo ----------------
struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double num = 0; std::string str;
    std::vector<Json> arr; std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* k) const { if (t != Obj) return nullptr; for (const auto& kv : obj) if (kv.first == k) return &kv.second; return nullptr; }
    std::string s(const char* k) const { const Json* j = get(k); return j && j->t == Str ? j->str : std::string(); }
    double n(const char* k, double def = -1) const { const Json* j = get(k); return j && j->t == Num ? j->num : def; }
};
struct JsonParser {
    const char* p; const char* e; int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool str(std::string& o) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            if (*p == '\\') {
                if (++p >= e) return false;
                char c = *p++;
                if (c == 'u') { if (e - p < 4) return false; unsigned v = (unsigned)std::strtoul(std::string(p, 4).c_str(), nullptr, 16); p += 4; if (v < 0x80) o += (char)v; else if (v < 0x800) { o += (char)(0xC0 | (v >> 6)); o += (char)(0x80 | (v & 63)); } else { o += (char)(0xE0 | (v >> 12)); o += (char)(0x80 | ((v >> 6) & 63)); o += (char)(0x80 | (v & 63)); } }
                else o += c == 'n' ? '\n' : (c == 't' ? '\t' : (c == 'r' ? '\r' : (c == 'b' ? '\b' : (c == 'f' ? '\f' : c))));
            } else o += *p++;
        }
        if (p >= e) return false;
        ++p; return true;
    }
    bool val(Json& j) {
        if (++depth > 32) return false;
        ws(); if (p >= e) return false;
        bool ok = true;
        if (*p == '{') {
            j.t = Json::Obj; ++p; ws();
            if (p < e && *p == '}') { ++p; --depth; return true; }
            for (;;) {
                ws(); std::string k; if (!str(k)) return false;
                ws(); if (p >= e || *p != ':') return false; ++p;
                Json v; if (!val(v)) return false;
                j.obj.emplace_back(std::move(k), std::move(v));
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                return false;
            }
        } else if (*p == '[') {
            j.t = Json::Arr; ++p; ws();
            if (p < e && *p == ']') { ++p; --depth; return true; }
            for (;;) {
                Json v; if (!val(v)) return false;
                j.arr.push_back(std::move(v));
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; break; }
                return false;
            }
        } else if (*p == '"') { j.t = Json::Str; ok = str(j.str); }
        else if (e - p >= 4 && std::strncmp(p, "true", 4) == 0) { j.t = Json::Bool; j.b = true; p += 4; }
        else if (e - p >= 5 && std::strncmp(p, "false", 5) == 0) { j.t = Json::Bool; p += 5; }
        else if (e - p >= 4 && std::strncmp(p, "null", 4) == 0) { j.t = Json::Null; p += 4; }
        else {
            const char* s = p;
            while (p < e && (std::strchr("+-0123456789.eE", *p))) ++p;
            if (p == s) return false;
            j.t = Json::Num; j.num = std::strtod(std::string(s, p).c_str(), nullptr);
        }
        --depth; return ok;
    }
};
inline bool parseJson(const std::string& s, Json& out) { JsonParser P{s.data(), s.data() + s.size()}; if (!P.val(out)) return false; P.ws(); return P.p == P.e; }

// ---------------- RSA (só verificação: m = s^e mod n, Montgomery com limbs de 32 bits) ----------------
struct Rsa {
    std::vector<uint32_t> n;   // little-endian
    std::vector<uint8_t> nBytes; uint64_t e = 0;
    uint32_t n0inv = 0; std::vector<uint32_t> r2;   // -n^-1 mod 2^32, R^2 mod n
    static std::vector<uint32_t> fromBE(const uint8_t* p, size_t len, size_t limbs) {
        std::vector<uint32_t> v(limbs, 0);
        for (size_t i = 0; i < len; ++i) { size_t bi = len - 1 - i; v[bi / 4] |= (uint32_t)p[i] << (8 * (bi % 4)); }
        return v;
    }
    static bool geq(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) { for (size_t i = a.size(); i-- > 0;) { if (a[i] != b[i]) return a[i] > b[i]; } return true; }
    static void subIn(std::vector<uint32_t>& a, const std::vector<uint32_t>& b) { uint64_t br = 0; for (size_t i = 0; i < a.size(); ++i) { uint64_t d = (uint64_t)a[i] - b[i] - br; a[i] = (uint32_t)d; br = (d >> 63) & 1; } }
    bool init(const std::vector<uint8_t>& mod, const std::vector<uint8_t>& ex) {
        size_t off = 0; while (off < mod.size() && mod[off] == 0) ++off;
        nBytes.assign(mod.begin() + (long)off, mod.end());
        if (nBytes.size() < 128 || nBytes.size() > 1024 || ex.empty() || ex.size() > 8) return false;
        e = 0; for (uint8_t b : ex) e = (e << 8) | b;
        if (e < 3 || !(e & 1)) return false;
        const size_t L = (nBytes.size() + 3) / 4;
        n = fromBE(nBytes.data(), nBytes.size(), L);
        if (!(n[0] & 1)) return false;
        uint32_t inv = n[0]; for (int i = 0; i < 5; ++i) inv *= 2 - n[0] * inv;
        n0inv = (uint32_t)(0u - inv);
        // R^2 mod n: 1 dobrado 2*32*L vezes (com redução)
        std::vector<uint32_t> x(L, 0); x[0] = 1;
        for (size_t k = 0; k < 64 * L; ++k) {
            uint32_t carry = 0;
            for (size_t i = 0; i < L; ++i) { uint32_t nv = (x[i] << 1) | carry; carry = x[i] >> 31; x[i] = nv; }
            if (carry || geq(x, n)) subIn(x, n);
        }
        r2 = x; return true;
    }
    void mont(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, std::vector<uint32_t>& out) const {
        const size_t L = n.size(); std::vector<uint32_t> t(L + 2, 0);
        for (size_t i = 0; i < L; ++i) {
            uint64_t C = 0;
            for (size_t j = 0; j < L; ++j) { uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + C; t[j] = (uint32_t)s; C = s >> 32; }
            uint64_t s = (uint64_t)t[L] + C; t[L] = (uint32_t)s; t[L + 1] = (uint32_t)(s >> 32);
            const uint32_t m = t[0] * n0inv;
            s = (uint64_t)t[0] + (uint64_t)m * n[0]; C = s >> 32;
            for (size_t j = 1; j < L; ++j) { s = (uint64_t)t[j] + (uint64_t)m * n[j] + C; t[j - 1] = (uint32_t)s; C = s >> 32; }
            s = (uint64_t)t[L] + C; t[L - 1] = (uint32_t)s; C = s >> 32;
            t[L] = t[L + 1] + (uint32_t)C;
        }
        out.assign(t.begin(), t.begin() + (long)L);
        if (t[L] || geq(out, n)) subIn(out, n);
    }
    // s^e mod n em big-endian com o tamanho do módulo (vazio = assinatura inválida)
    std::vector<uint8_t> pub(const std::vector<uint8_t>& sig) const {
        if (sig.size() != nBytes.size()) return {};
        const size_t L = n.size();
        std::vector<uint32_t> s = fromBE(sig.data(), sig.size(), L);
        if (geq(s, n)) return {};
        std::vector<uint32_t> sm, acc, one(L, 0); one[0] = 1;
        mont(s, r2, sm);                 // s em forma de Montgomery
        mont(one, r2, acc);              // 1 em forma de Montgomery
        int top = 63; while (top > 0 && !((e >> top) & 1)) --top;
        for (int bit = top; bit >= 0; --bit) { mont(acc, acc, acc); if ((e >> bit) & 1) mont(acc, sm, acc); }
        std::vector<uint32_t> r; mont(acc, one, r);
        std::vector<uint8_t> o(nBytes.size());
        for (size_t i = 0; i < o.size(); ++i) { size_t bi = o.size() - 1 - i; o[i] = (uint8_t)(r[bi / 4] >> (8 * (bi % 4))); }
        return o;
    }
    // PKCS#1 v1.5 + SHA-256
    bool verifySha256(const std::string& msg, const std::vector<uint8_t>& sig) const {
        std::vector<uint8_t> em = pub(sig);
        if (em.size() < 11 + 19 + 32) return false;
        uint8_t h[32]; sha256(msg.data(), msg.size(), h);
        static const uint8_t di[19] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
        const size_t k = em.size(), psEnd = k - 32 - 19 - 1;
        if (em[0] != 0x00 || em[1] != 0x01 || em[psEnd] != 0x00) return false;
        for (size_t i = 2; i < psEnd; ++i) if (em[i] != 0xFF) return false;
        return std::memcmp(&em[psEnd + 1], di, 19) == 0 && std::memcmp(&em[psEnd + 20], h, 32) == 0;
    }
};

// ---------------- chaves (JWK) ----------------
class KeySet {
public:
    // relê o arquivo se ele mudou (mtime/tamanho). Devolve o número de chaves válidas.
    int refresh(const std::string& path) {
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) { keys_.clear(); mtime_ = 0; return 0; }
        if (st.st_mtime == mtime_ && st.st_size == size_ && !keys_.empty()) return (int)keys_.size();
        mtime_ = st.st_mtime; size_ = st.st_size;
        std::string txt;
        if (FILE* f = std::fopen(path.c_str(), "rb")) { char b[4096]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0 && txt.size() < 200000) txt.append(b, r); std::fclose(f); }
        std::map<std::string, std::shared_ptr<Rsa>> next;
        Json j;
        if (parseJson(txt, j)) {
            const Json* arr = j.get("keys");
            if (arr && arr->t == Json::Arr) for (const Json& k : arr->arr) {
                if (k.s("kty") != "RSA" || (k.get("alg") && k.s("alg") != "RS256")) continue;
                std::vector<uint8_t> nb, eb;
                if (!b64url(k.s("n"), nb) || !b64url(k.s("e"), eb)) continue;
                auto r = std::make_shared<Rsa>();
                if (r->init(nb, eb) && !k.s("kid").empty()) next[k.s("kid")] = r;
            }
        }
        if (!next.empty() || txt.empty()) keys_ = std::move(next);   // arquivo inválido no meio de uma troca: mantém as chaves antigas
        return (int)keys_.size();
    }
    const Rsa* find(const std::string& kid) const { auto it = keys_.find(kid); return it == keys_.end() ? nullptr : it->second.get(); }
    size_t size() const { return keys_.size(); }
private:
    std::map<std::string, std::shared_ptr<Rsa>> keys_;
    time_t mtime_ = 0; off_t size_ = -1;
};

enum class Result { Ok, Malformed, BadAlg, UnknownKid, BadSig, BadIss, BadAud, Expired, NotYet, BadSub, NoKeys };
inline const char* resultName(Result r) {
    switch (r) { case Result::Ok: return "ok"; case Result::Malformed: return "malformado"; case Result::BadAlg: return "alg"; case Result::UnknownKid: return "kid desconhecido";
        case Result::BadSig: return "assinatura invalida"; case Result::BadIss: return "iss"; case Result::BadAud: return "aud"; case Result::Expired: return "expirado";
        case Result::NotYet: return "iat no futuro"; case Result::BadSub: return "sub"; case Result::NoKeys: return "sem chaves"; }
    return "?";
}
struct Claims { std::string sub, provider; double exp = 0, iat = 0; };
// Verifica um idToken do Firebase. now = segundos Unix.
inline Result verifyFirebase(const std::string& tok, const std::string& project, const KeySet& keys, double now, Claims& out) {
    if (tok.size() > 4096) return Result::Malformed;
    const size_t d1 = tok.find('.'), d2 = d1 == std::string::npos ? d1 : tok.find('.', d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos || tok.find('.', d2 + 1) != std::string::npos) return Result::Malformed;
    std::vector<uint8_t> hb, pb, sig;
    if (!b64url(tok.substr(0, d1), hb) || !b64url(tok.substr(d1 + 1, d2 - d1 - 1), pb) || !b64url(tok.substr(d2 + 1), sig)) return Result::Malformed;
    Json h, p;
    if (!parseJson(std::string(hb.begin(), hb.end()), h) || !parseJson(std::string(pb.begin(), pb.end()), p) || h.t != Json::Obj || p.t != Json::Obj) return Result::Malformed;
    if (h.s("alg") != "RS256") return Result::BadAlg;
    if (keys.size() == 0) return Result::NoKeys;
    const Rsa* k = keys.find(h.s("kid"));
    if (!k) return Result::UnknownKid;
    if (!k->verifySha256(tok.substr(0, d2), sig)) return Result::BadSig;
    if (p.s("iss") != "https://securetoken.google.com/" + project) return Result::BadIss;
    if (p.s("aud") != project) return Result::BadAud;
    const double exp = p.n("exp"), iat = p.n("iat"), at = p.n("auth_time", iat);
    if (exp <= 0 || now > exp + 30.0) return Result::Expired;                 // 30 s de folga de relógio
    if (iat <= 0 || iat > now + 300.0 || at > now + 300.0) return Result::NotYet;
    const std::string sub = p.s("sub");
    if (sub.empty() || sub.size() > 128) return Result::BadSub;
    for (char c : sub) if ((unsigned char)c < 33 || (unsigned char)c > 126) return Result::BadSub;
    out.sub = sub; out.exp = exp; out.iat = iat;
    if (const Json* fb = p.get("firebase")) out.provider = fb->s("sign_in_provider");
    return Result::Ok;
}

}  // namespace jwt
