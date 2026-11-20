// Isolated - cliente da TRIPULAÇÃO: faz os pedidos TCP (shared/team.h) numa thread própria, um de cada vez, sem travar a tela.
// O motor enfileira (send) e lê as respostas (poll) a cada quadro. Ver engine_team.inc.
#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>

#include "../../shared/team.h"

namespace e0 {

class TeamClient {
public:
    struct Reply { uint8_t op = 0; bool net = false; uint8_t result = 0; e0net::TeamView view; e0net::TeamRequest req; };
    ~TeamClient() { stop(); }
    void setServer(const std::string& ip, int port) { std::lock_guard<std::mutex> l(m_); ip_ = ip; port_ = port; }
    bool hasServer() { std::lock_guard<std::mutex> l(m_); return !ip_.empty(); }
    void send(const e0net::TeamRequest& r) {
        std::lock_guard<std::mutex> l(m_);
        if (r.op == e0net::TeamGet) for (const auto& q : q_) if (q.op == e0net::TeamGet) return;   // já tem uma atualização na fila
        if (q_.size() > 16) return;
        q_.push_back(r);
        if (!th_.joinable()) { stop_ = false; th_ = std::thread([this] { run(); }); }
        cv_.notify_one();
    }
    bool poll(Reply& out) { std::lock_guard<std::mutex> l(m_); if (done_.empty()) return false; out = std::move(done_.front()); done_.pop_front(); return true; }
    bool busy() { std::lock_guard<std::mutex> l(m_); return !q_.empty() || working_; }
    void stop() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; q_.clear(); }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
private:
    void run() {
        for (;;) {
            e0net::TeamRequest r; std::string ip; int port;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                r = q_.front(); q_.pop_front(); ip = ip_; port = port_; working_ = true;
            }
            Reply rep; rep.op = r.op; rep.req = r;
            rep.net = !ip.empty() && e0net::teamCall(ip.c_str(), (uint16_t)port, r, rep.result, rep.view, 6000);
            std::lock_guard<std::mutex> l(m_);
            working_ = false;
            if (done_.size() < 32) done_.push_back(std::move(rep));
        }
    }
    std::mutex m_; std::condition_variable cv_;
    std::thread th_;
    std::deque<e0net::TeamRequest> q_;
    std::deque<Reply> done_;
    std::string ip_; int port_ = 0;
    bool stop_ = false, working_ = false;
};

}  // namespace e0
