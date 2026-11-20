// Isolated - TRIPULAÇÕES no servidor: dados + arquivo <data>/teams.e0t (E0T1, CRC32, gravação atômica pela thread de E/S).
// Quem é de qual tripulação fica SÓ aqui (lista de membros por chave de conta "fb:..."/"anon:..."); a conta guarda apenas a licença.
// Regras e o serviço TCP: shared/team.h. Processamento dos pedidos: server_main.cpp ("TRIPULAÇÃO").
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../shared/team.h"
#include "../shared/conquest.h"
#include "persist.h"

namespace teams {

struct Member { std::string key, name; int64_t joined = 0; std::string pushToken; };   // pushToken: FCM do aparelho (notificação com o jogo fechado)
struct Msg { uint32_t id = 0; int64_t at = 0; std::string authorKey, author, text; };
struct Team {
    uint32_t id = 0;
    std::string name, code, ownerKey;
    uint8_t flag[e0net::kFlagBytes] = {};
    int64_t created = 0;
    std::vector<Member> members;   // members[0] não é necessariamente o capitão: ownerKey manda
    std::deque<Msg> msgs;
    uint32_t nextMsg = 1;
    int64_t treasury = 0;      // COFRE (moedas)
    uint8_t slotLevel = 0;     // expansões de vagas compradas
    uint8_t ships = 0;         // navios comprados pela tripulação (bit = ShipKind)
    int capacity() const { return e0net::teamCapacity(slotLevel); }
    int memberOf(const std::string& key) const { for (size_t i = 0; i < members.size(); ++i) if (members[i].key == key) return (int)i; return -1; }
};

class Registry {
public:
    static constexpr uint32_t kMagic = 0x31543045u;   // "E0T1"
    static constexpr uint16_t kVersion = 4;   // v2: token FCM de cada membro; v3: cofre, vagas e navios; v4: ilhas dominadas
    std::unordered_map<uint32_t, std::shared_ptr<Team>> byId;
    std::unordered_map<std::string, uint32_t> byKey;    // conta -> tripulação
    std::unordered_map<std::string, uint32_t> byCode;   // código -> tripulação
    uint32_t nextId = 1;
    bool dirty = false;
    // DOMINAÇÃO DE ILHAS (shared/conquest.h): dona de cada ilha (0 = livre), desde quando (unix s) e o último pagamento da renda
    uint32_t isleOwner[e0net::kIslandSlots] = {};
    int64_t isleSince[e0net::kIslandSlots] = {};
    int64_t lastPayout = 0;

    Team* ofKey(const std::string& key) { auto it = byKey.find(key); if (it == byKey.end()) return nullptr; auto t = byId.find(it->second); return t == byId.end() ? nullptr : t->second.get(); }
    Team* ofCode(const std::string& code) { auto it = byCode.find(code); if (it == byCode.end()) return nullptr; auto t = byId.find(it->second); return t == byId.end() ? nullptr : t->second.get(); }
    static std::string lower(const std::string& s) { std::string o = s; for (char& c : o) { if (c >= 'A' && c <= 'Z') c = (char)(c + 32); else if ((unsigned char)c >= 0xC0 && (unsigned char)c <= 0xDE && (unsigned char)c != 0xD7) c = (char)((unsigned char)c + 32); } return o; }
    bool nameTaken(const std::string& name, uint32_t except) const {
        const std::string n = lower(name);
        for (const auto& kv : byId) if (kv.first != except && lower(kv.second->name) == n) return true;
        return false;
    }
    void add(std::shared_ptr<Team> t) {
        byId[t->id] = t; byCode[t->code] = t->id;
        for (const Member& m : t->members) byKey[m.key] = t->id;
        if (t->id >= nextId) nextId = t->id + 1;
    }
    void remove(uint32_t id) {
        auto it = byId.find(id); if (it == byId.end()) return;
        for (const Member& m : it->second->members) { auto k = byKey.find(m.key); if (k != byKey.end() && k->second == id) byKey.erase(k); }
        byCode.erase(it->second->code);
        byId.erase(it);
        for (int s = 0; s < e0net::kIslandSlots; ++s) if (isleOwner[s] == id) { isleOwner[s] = 0; isleSince[s] = 0; }   // ilhas da tripulação desfeita ficam livres
    }
    // apaga as mensagens com mais de 24 h (e o excesso). true se mudou algo.
    static bool prune(Team& t, int64_t now) {
        bool ch = false;
        while (!t.msgs.empty() && (now - t.msgs.front().at > e0net::kTeamMsgTtl || (int)t.msgs.size() > e0net::kTeamMsgKeep)) { t.msgs.pop_front(); ch = true; }
        return ch;
    }

    std::vector<uint8_t> serialize() const {
        persist::Out o; o.put<uint32_t>(kMagic); o.put<uint16_t>(kVersion); o.put<uint32_t>(nextId);
        o.put<uint32_t>((uint32_t)byId.size());
        for (const auto& kv : byId) {
            const Team& t = *kv.second;
            o.put<uint32_t>(t.id); o.str(t.name); o.str(t.code); o.str(t.ownerKey); o.raw(t.flag, sizeof t.flag); o.put<int64_t>(t.created); o.put<uint32_t>(t.nextMsg); o.put<int64_t>(t.treasury); o.put<uint8_t>(t.slotLevel); o.put<uint8_t>(t.ships);
            o.put<uint16_t>((uint16_t)t.members.size());
            for (const Member& m : t.members) { o.str(m.key); o.str(m.name); o.put<int64_t>(m.joined); o.str(m.pushToken); }
            o.put<uint16_t>((uint16_t)t.msgs.size());
            for (const Msg& m : t.msgs) { o.put<uint32_t>(m.id); o.put<int64_t>(m.at); o.str(m.authorKey); o.str(m.author); o.str(m.text); }
        }
        int ni = 0; for (int s = 0; s < e0net::kIslandSlots; ++s) if (isleOwner[s]) ++ni;
        o.put<uint16_t>((uint16_t)ni);
        for (int s = 0; s < e0net::kIslandSlots; ++s) if (isleOwner[s]) { o.put<uint8_t>((uint8_t)s); o.put<uint32_t>(isleOwner[s]); o.put<int64_t>(isleSince[s]); }
        o.put<int64_t>(lastPayout);
        o.crc(); return o.b;
    }
    bool deserialize(const std::vector<uint8_t>& d) {
        if (!persist::crcOk(d)) return false;
        persist::In r(d.data(), d.size() - 4);
        if (r.get<uint32_t>() != kMagic) return false;
        const uint16_t ver = r.get<uint16_t>(); if (ver < 1 || ver > kVersion) return false;
        Registry tmp; tmp.nextId = r.get<uint32_t>();
        const uint32_t n = r.get<uint32_t>();
        for (uint32_t i = 0; i < n && r.ok; ++i) {
            auto t = std::make_shared<Team>();
            t->id = r.get<uint32_t>(); t->name = r.str(); t->code = r.str(); t->ownerKey = r.str(); r.raw(t->flag, sizeof t->flag); t->created = r.get<int64_t>(); t->nextMsg = r.get<uint32_t>();
            if (ver >= 3) { t->treasury = r.get<int64_t>(); t->slotLevel = std::min<uint8_t>(r.get<uint8_t>(), (uint8_t)e0net::kTeamSlotLevels); t->ships = r.get<uint8_t>() & 15u; }
            const uint16_t nm = r.get<uint16_t>();
            for (int k = 0; k < nm && r.ok; ++k) { Member m; m.key = r.str(); m.name = r.str(); m.joined = r.get<int64_t>(); if (ver >= 2) m.pushToken = r.str(); t->members.push_back(m); }
            const uint16_t nn = r.get<uint16_t>();
            for (int k = 0; k < nn && r.ok; ++k) { Msg m; m.id = r.get<uint32_t>(); m.at = r.get<int64_t>(); m.authorKey = r.str(); m.author = r.str(); m.text = r.str(); t->msgs.push_back(m); }
            if (r.ok && t->id && !t->members.empty()) tmp.add(t);
        }
        if (ver >= 4) {
            const uint16_t ni = r.get<uint16_t>();
            for (int k = 0; k < ni && r.ok; ++k) {
                const uint8_t sl = r.get<uint8_t>(); const uint32_t ow = r.get<uint32_t>(); const int64_t since = r.get<int64_t>();
                if (e0net::islandConquerable(sl) && tmp.byId.count(ow)) { tmp.isleOwner[sl] = ow; tmp.isleSince[sl] = since; }
            }
            tmp.lastPayout = r.get<int64_t>();
        }
        if (!r.ok) return false;
        std::copy(tmp.isleOwner, tmp.isleOwner + e0net::kIslandSlots, isleOwner); std::copy(tmp.isleSince, tmp.isleSince + e0net::kIslandSlots, isleSince); lastPayout = tmp.lastPayout;
        byId.swap(tmp.byId); byKey.swap(tmp.byKey); byCode.swap(tmp.byCode); nextId = std::max(tmp.nextId, nextId);
        return true;
    }
};

}  // namespace teams
