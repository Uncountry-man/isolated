#include "world_sync.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace e0net {

static bool sendAll(int fd, const uint8_t* d, size_t n) {
    while (n > 0) {
        ssize_t k = send(fd, d, n, MSG_NOSIGNAL);
        if (k <= 0) return false;
        d += k; n -= (size_t)k;
    }
    return true;
}
static bool recvAll(int fd, uint8_t* d, size_t n) {
    while (n > 0) {
        ssize_t k = recv(fd, d, n, 0);
        if (k <= 0) return false;
        d += k; n -= (size_t)k;
    }
    return true;
}
static void setTimeouts(int fd, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void serveWorldConnection(int fd, const std::vector<uint8_t>& bytes, uint32_t hash) {
    setTimeouts(fd, 5000);
    uint8_t req[4];
    if (recvAll(fd, req, 4) && std::memcmp(req, "E0WG", 4) == 0) serveWorldAfterMagic(fd, bytes, hash);
    else close(fd);
}

void serveWorldAfterMagic(int fd, const std::vector<uint8_t>& bytes, uint32_t hash) {
    setTimeouts(fd, 5000);
    uint8_t req[4];
    if (recvAll(fd, req, 4)) {
        uint32_t cached; std::memcpy(&cached, req, 4);
        uint8_t head[12];
        std::memcpy(head, "E0WR", 4);
        uint32_t size = cached == hash ? 0u : (uint32_t)bytes.size();
        std::memcpy(head + 4, &hash, 4); std::memcpy(head + 8, &size, 4);
        if (sendAll(fd, head, 12) && size) sendAll(fd, bytes.data(), bytes.size());
    }
    close(fd);
}

bool fetchWorld(const char* ip, uint16_t port, uint32_t cachedHash, std::vector<uint8_t>& payload, uint32_t& serverHash, int timeoutMs) {
    payload.clear();
    in_addr ia{};
    if (!ip || inet_pton(AF_INET, ip, &ia) != 1) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    // connect com timeout (não-bloqueante + poll)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr = ia; a.sin_port = htons(port);
    int rc = connect(fd, (sockaddr*)&a, sizeof a);
    if (rc < 0) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeoutMs) <= 0) { close(fd); return false; }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return false; }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    setTimeouts(fd, timeoutMs);
    uint8_t req[8];
    std::memcpy(req, "E0WG", 4); std::memcpy(req + 4, &cachedHash, 4);
    uint8_t head[12];
    bool ok = sendAll(fd, req, 8) && recvAll(fd, head, 12) && std::memcmp(head, "E0WR", 4) == 0;
    if (ok) {
        uint32_t size;
        std::memcpy(&serverHash, head + 4, 4); std::memcpy(&size, head + 8, 4);
        if (size > (64u << 20)) ok = false;                  // limite de sanidade: 64 MB
        else if (size) { payload.resize(size); ok = recvAll(fd, payload.data(), size); }
    }
    close(fd);
    return ok;
}

}  // namespace e0net
