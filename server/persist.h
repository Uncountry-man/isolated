// Isolated - PERSISTÊNCIA do servidor (contas dos jogadores + estado do mundo), sem dependências externas.
// Formato: 1 arquivo binário por conta (E0P1, CRC32 no fim) em <data>/accounts/<aa>/<sha256(chave)[0..40]>.e0p, gravado de forma ATÔMICA
// (.tmp + fsync + rename) por uma thread de E/S própria: o tick de 30 Hz nunca espera o disco. Recibos da Play já creditados ficam também
// num log global só de acréscimo (<data>/receipts.log, sha256(token) -> conta) para nunca creditar o mesmo token 2x (nem em contas diferentes).
// Estado do mundo (itens soltos, construções, baús): <data>/world_state.e0x (E0X1, CRC32), gravado a cada 60 s se mudou e ao desligar.
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../shared/inventory.h"
#include "../shared/world.h"   // e0net::crc32
#include "jwt.h"

namespace persist {

using e0net::InvSlot;
using e0net::kInvSlots;

// ---------------- buffer binário ----------------
struct Out {
    std::vector<uint8_t> b;
    template <class T> void put(T v) { const uint8_t* p = (const uint8_t*)&v; b.insert(b.end(), p, p + sizeof(T)); }
    void str(const std::string& s) { put<uint16_t>((uint16_t)std::min<size_t>(s.size(), 65535)); b.insert(b.end(), s.begin(), s.begin() + (long)std::min<size_t>(s.size(), 65535)); }
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void crc() { put<uint32_t>(e0net::crc32(b.data(), b.size())); }
};
struct In {
    const uint8_t* p; size_t n, i = 0; bool ok = true;
    In(const uint8_t* d, size_t len) : p(d), n(len) {}
    template <class T> T get() { T v{}; if (i + sizeof(T) > n) { ok = false; return v; } std::memcpy(&v, p + i, sizeof(T)); i += sizeof(T); return v; }
    std::string str() { uint16_t l = get<uint16_t>(); if (!ok || i + l > n) { ok = false; return {}; } std::string s((const char*)p + i, l); i += l; return s; }
    void raw(void* d, size_t k) { if (i + k > n) { ok = false; return; } std::memcpy(d, p + i, k); i += k; }
};
// confere o CRC32 dos últimos 4 bytes
inline bool crcOk(const std::vector<uint8_t>& d) {
    if (d.size() < 8) return false;
    uint32_t st; std::memcpy(&st, d.data() + d.size() - 4, 4);
    return e0net::crc32(d.data(), d.size() - 4) == st;
}

// ---------------- CONTA ----------------
struct Purchase { std::string sku, token; int32_t coins = 0; int64_t at = 0; uint8_t verified = 0; };
struct Account {
    static constexpr uint32_t kMagic = 0x31503045u;   // "E0P1"
    static constexpr uint16_t kVersion = 4;   // v3: licença + tripulação; v4: token FCM (push)
    std::string key;            // "fb:<uid do Firebase>" (verificado) ou "anon:<uid local>" (NÃO verificado)
    bool verified = false;
    int64_t created = 0, updated = 0;
    int32_t coins = 0;
    InvSlot inv[kInvSlots];
    uint8_t health = 100, hunger = 100;
    uint8_t skin = 0;
    bool hasFlag = false; uint8_t flag[e0net::kFlagBytes] = {};
    bool hasPos = false; float px = 0, py = 0, pz = 0;
    uint8_t ships = 0;          // bit k = a conta é dona do navio do tipo k (começa 0 = nenhum)
    uint8_t teamLicense = 0;    // 1 = comprou a LICENÇA DE TRIPULAÇÃO e ainda não criou (gasta ao criar)
    std::string pushToken;      // token do Firebase Cloud Messaging do último aparelho (notificações da tripulação com o jogo fechado)
    int32_t adDay = 0; uint16_t adCount = 0; int64_t adLast = 0;   // limite de anúncios premiados (dia UTC)
    std::vector<Purchase> purchases;
    std::string migratedTo, migratedFrom;   // migração 1x de um visitante local para a conta do Firebase
    // --- só em memória ---
    int session = -1;          // slot do jogador que usa a conta agora (-1 = ninguém)
    bool persistent = true;    // false = sessão sem login (efêmera: nunca vai ao disco)
    bool dirty = false, critical = false; double dirtySince = 0, lastUsed = 0;
    bool fresh = false;        // criada nesta execução e ainda sem nada (pode receber a migração)

    std::vector<uint8_t> serialize() const {
        Out o; o.put<uint32_t>(kMagic); o.put<uint16_t>(kVersion);
        o.str(key); o.put<uint8_t>(verified ? 1 : 0); o.put<int64_t>(created); o.put<int64_t>(updated);
        o.put<int32_t>(coins);
        for (const InvSlot& s : inv) { o.put<uint8_t>(s.id); o.put<uint16_t>(s.n); }
        o.put<uint8_t>(health); o.put<uint8_t>(hunger); o.put<uint8_t>(skin);
        o.put<uint8_t>(hasFlag ? 1 : 0); o.raw(flag, sizeof flag);
        o.put<uint8_t>(hasPos ? 1 : 0); o.put<float>(px); o.put<float>(py); o.put<float>(pz);
        o.put<int32_t>(adDay); o.put<uint16_t>(adCount); o.put<int64_t>(adLast);
        o.put<uint16_t>((uint16_t)std::min<size_t>(purchases.size(), 4000));
        for (size_t i = 0; i < purchases.size() && i < 4000; ++i) { const Purchase& p = purchases[i]; o.str(p.sku); o.str(p.token); o.put<int32_t>(p.coins); o.put<int64_t>(p.at); o.put<uint8_t>(p.verified); }
        o.str(migratedTo); o.str(migratedFrom);
        o.put<uint8_t>(ships);
        o.put<uint8_t>(teamLicense);   // a tripulação de cada conta fica só em teams.e0t (lista de membros)
        o.str(pushToken);
        o.crc(); return o.b;
    }
    bool deserialize(const std::vector<uint8_t>& d) {
        if (!crcOk(d)) return false;
        In r(d.data(), d.size() - 4);
        if (r.get<uint32_t>() != kMagic) return false;
        const uint16_t ver = r.get<uint16_t>();
        if (ver < 1 || ver > kVersion) return false;   // versão futura: não mexe (não sobrescreve)
        key = r.str(); verified = r.get<uint8_t>() != 0; created = r.get<int64_t>(); updated = r.get<int64_t>();
        coins = r.get<int32_t>();
        for (InvSlot& s : inv) { s.id = r.get<uint8_t>(); s.n = r.get<uint16_t>(); }
        e0net::invSanitize(inv);
        health = r.get<uint8_t>(); hunger = r.get<uint8_t>(); skin = r.get<uint8_t>();
        hasFlag = r.get<uint8_t>() != 0; r.raw(flag, sizeof flag);
        hasPos = r.get<uint8_t>() != 0; px = r.get<float>(); py = r.get<float>(); pz = r.get<float>();
        adDay = r.get<int32_t>(); adCount = r.get<uint16_t>(); adLast = r.get<int64_t>();
        const uint16_t np = r.get<uint16_t>();
        purchases.clear();
        for (int i = 0; i < np && r.ok; ++i) { Purchase p; p.sku = r.str(); p.token = r.str(); p.coins = r.get<int32_t>(); p.at = r.get<int64_t>(); p.verified = r.get<uint8_t>(); purchases.push_back(p); }
        migratedTo = r.str(); migratedFrom = r.str();
        ships = ver >= 2 ? (uint8_t)(r.get<uint8_t>() & 15u) : 0;
        if (ver >= 3) teamLicense = r.get<uint8_t>() ? 1 : 0;
        if (ver >= 4) pushToken = r.str();
        if (coins < 0) coins = 0;
        if (health > 100) health = 100;
        if (hunger > 100) hunger = 100;
        return r.ok;
    }
};

// ---------------- arquivos ----------------
inline bool mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) mkdir(cur.c_str(), 0750);
    }
    struct stat st{}; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
inline bool readFile(const std::string& p, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    uint8_t b[65536]; size_t r;
    while ((r = std::fread(b, 1, sizeof b, f)) > 0) { out.insert(out.end(), b, b + r); if (out.size() > (64u << 20)) break; }
    std::fclose(f); return true;
}
// grava atomicamente: tmp + fsync + rename (+ fsync do diretório)
inline bool writeAtomic(const std::string& p, const std::vector<uint8_t>& d, bool sync = true) {
    const std::string tmp = p + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0) return false;
    size_t w = 0;
    while (w < d.size()) { ssize_t k = ::write(fd, d.data() + w, d.size() - w); if (k <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; } w += (size_t)k; }
    if (sync && ::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
    ::close(fd);
    if (std::rename(tmp.c_str(), p.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    if (sync) { const size_t sl = p.rfind('/'); if (sl != std::string::npos) { int dfd = ::open(p.substr(0, sl).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dfd >= 0) { ::fsync(dfd); ::close(dfd); } } }
    return true;
}

// ---------------- LOJA DE DADOS (thread de E/S) ----------------
class Store {
public:
    struct Loaded { std::string key; bool found = false, corrupt = false; std::shared_ptr<Account> acc; };
    bool open(const std::string& dir) {
        dir_ = dir;
        if (!mkdirs(dir_) || !mkdirs(dir_ + "/accounts")) return false;
        // índice global de recibos: sha256(token) -> chave da conta
        std::vector<uint8_t> log;
        if (readFile(dir_ + "/receipts.log", log)) {
            std::string s(log.begin(), log.end()); size_t a = 0;
            while (a < s.size()) {
                size_t e = s.find('\n', a); if (e == std::string::npos) break;   // linha incompleta no fim (queda no meio): ignorada
                std::string line = s.substr(a, e - a); a = e + 1;
                size_t sp = line.find(' ');
                if (sp == 64) { size_t sp2 = line.find(' ', sp + 1); receipts_[line.substr(0, 64)] = line.substr(sp + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1); }
            }
        }
        th_ = std::thread([this] { run(); });
        return true;
    }
    ~Store() { close(); }
    void close() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    const std::string& dir() const { return dir_; }
    std::string pathOf(const std::string& key) const {
        const std::string h = jwt::sha256Hex(key);
        return dir_ + "/accounts/" + h.substr(0, 2) + "/" + h.substr(0, 40) + ".e0p";
    }
    // pedidos (thread principal)
    void requestLoad(const std::string& key) { push({JobLoad, key, {}, "", true}); }
    void save(const Account& a, bool sync) { push({JobSave, a.key, a.serialize(), "", sync}); }
    void saveFile(const std::string& name, std::vector<uint8_t> bytes) { push({JobFile, "", std::move(bytes), dir_ + "/" + name, true}); }
    void appendReceipt(const std::string& tokenHash, const std::string& key, const std::string& line) {
        receipts_[tokenHash] = key;
        std::string l = tokenHash + " " + key + " " + line + "\n";
        push({JobAppend, "", std::vector<uint8_t>(l.begin(), l.end()), dir_ + "/receipts.log", true});
    }
    const std::string* receiptOwner(const std::string& tokenHash) const { auto it = receipts_.find(tokenHash); return it == receipts_.end() ? nullptr : &it->second; }
    // resultados de leitura (thread principal)
    void poll(const std::function<void(Loaded&)>& fn) {
        std::deque<Loaded> got;
        { std::lock_guard<std::mutex> l(m_); got.swap(done_); }
        for (Loaded& x : got) fn(x);
    }
    void flush() {   // espera a fila esvaziar (desligamento)
        std::unique_lock<std::mutex> l(m_);
        cvIdle_.wait(l, [this] { return jobs_.empty() && !busy_; });
    }
    size_t queued() { std::lock_guard<std::mutex> l(m_); return jobs_.size(); }
    long writes() const { return writes_; }
    long writeErrors() const { return writeErr_; }
    // leitura síncrona (ferramentas/testes)
    bool loadNow(const std::string& key, Account& out, bool* corrupt = nullptr) {
        std::vector<uint8_t> d;
        if (!readFile(pathOf(key), d)) return false;
        bool ok = out.deserialize(d) && out.key == key;
        if (corrupt) *corrupt = !ok;
        return ok;
    }
private:
    enum JobKind { JobLoad, JobSave, JobFile, JobAppend };
    struct Job { JobKind k; std::string key; std::vector<uint8_t> data; std::string path; bool sync; };
    void push(Job j) { { std::lock_guard<std::mutex> l(m_); jobs_.push_back(std::move(j)); } cv_.notify_one(); }
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !jobs_.empty(); });
                if (jobs_.empty()) { if (stop_) return; continue; }
                j = std::move(jobs_.front()); jobs_.pop_front(); busy_ = true;
            }
            if (j.k == JobLoad) {
                Loaded r; r.key = j.key;
                std::vector<uint8_t> d;
                const std::string p = pathOf(j.key);
                if (readFile(p, d)) {
                    auto a = std::make_shared<Account>();
                    if (a->deserialize(d) && a->key == j.key) { r.found = true; r.acc = a; }
                    else {   // arquivo corrompido: guarda uma cópia e NÃO sobrescreve em silêncio (o servidor recusa o login dessa conta)
                        r.corrupt = true;
                        const std::string bak = p + ".corrompido-" + std::to_string((long)std::time(nullptr));
                        writeAtomic(bak, d, true);
                        std::fprintf(stderr, "[dados] ERRO: conta '%s' com arquivo corrompido (copia em %s)\n", j.key.c_str(), bak.c_str());
                    }
                }
                std::lock_guard<std::mutex> l(m_); done_.push_back(std::move(r));
            } else if (j.k == JobSave) {
                const std::string p = pathOf(j.key);
                mkdirs(p.substr(0, p.rfind('/')));
                if (writeAtomic(p, j.data, j.sync)) ++writes_; else { ++writeErr_; std::fprintf(stderr, "[dados] ERRO ao gravar a conta '%s' (%s)\n", j.key.c_str(), std::strerror(errno)); }
            } else if (j.k == JobFile) {
                if (writeAtomic(j.path, j.data, j.sync)) ++writes_; else { ++writeErr_; std::fprintf(stderr, "[dados] ERRO ao gravar %s\n", j.path.c_str()); }
            } else if (j.k == JobAppend) {
                int fd = ::open(j.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
                bool ok = fd >= 0 && ::write(fd, j.data.data(), j.data.size()) == (ssize_t)j.data.size() && ::fsync(fd) == 0;
                if (fd >= 0) ::close(fd);
                if (!ok) { ++writeErr_; std::fprintf(stderr, "[dados] ERRO ao gravar o log de recibos\n"); }
            }
            { std::lock_guard<std::mutex> l(m_); busy_ = false; }
            cvIdle_.notify_all();
        }
    }
    std::string dir_;
    std::thread th_;
    std::mutex m_; std::condition_variable cv_, cvIdle_;
    std::deque<Job> jobs_; std::deque<Loaded> done_;
    bool stop_ = false, busy_ = false;
    std::atomic<long> writes_{0}, writeErr_{0};
    std::unordered_map<std::string, std::string> receipts_;   // só a thread principal mexe
};

}  // namespace persist
