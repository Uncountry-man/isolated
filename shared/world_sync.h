// Isolated - distribuição do mundo por TCP (servidor -> clientes), com cache no cliente.
//
//  Protocolo (porta TCP = a mesma porta UDP do jogo):
//    cliente -> servidor : "E0WG" + hashEmCache (u32; 0 = não tenho)
//    servidor -> cliente : "E0WR" + hash (u32) + tamanho (u32) [+ arquivo .e0w inteiro se hash != hashEmCache]
//  O cliente valida o CRC32 do arquivo (World::fromBytes). Se o hash do servidor == hash do cache, não baixa nada.
#pragma once
#include <cstdint>
#include <vector>

namespace e0net {

// Servidor: atende UMA conexão já aceita (bloqueante; chamar em thread) e fecha o socket.
void serveWorldConnection(int fd, const std::vector<uint8_t>& worldBytes, uint32_t worldHash);
// Igual, mas os 4 bytes "E0WG" já foram lidos (o servidor lê o magic para escolher entre mundo e TRIPULAÇÃO "E0TQ").
void serveWorldAfterMagic(int fd, const std::vector<uint8_t>& worldBytes, uint32_t worldHash);

// Cliente: busca o mundo. Retorna true se o servidor respondeu; `payload` vazio => "seu cache está atual".
bool fetchWorld(const char* ipv4, uint16_t port, uint32_t cachedHash, std::vector<uint8_t>& payload, uint32_t& serverHash, int timeoutMs);

}  // namespace e0net
