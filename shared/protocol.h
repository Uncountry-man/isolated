// Isolated - protocolo de rede e simulação COMPARTILHADOS entre cliente e servidor.
// Qualquer mudança aqui afeta os dois lados: incremente kProtocolMagic se quebrar compatibilidade.
// Serialização: tipos primitivos em little-endian nativo (todos os alvos são LE).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "world.h"
#include "terrain.h"
#include "ship.h"
#include "chest.h"
#include "inventory.h"
#include "isle_build.h"

namespace e0net {

constexpr uint32_t kProtocolMagic = 0xE0E0002Du;  // versão do protocolo (v45: 4 SKINS (kSkinCount 2 -> 4: pirata gordo e corsária de casaco); v44: MELHORIAS DA ILHA no lugar do sistema de construção antigo (MsgBuildPlace/Remove/Sync 9..11 saíram; MsgStructReq/MsgStructRes/MsgStructSync 58..60; colisão das estruturas em stepPlayer; portas/pisos/escadas não existem mais); v43: NAVIOS SAO DA CONTA (a conta comeca sem navio; ActBuyShip compra com moedas; MsgInvSync ganha o byte `ships`; ShipState.owner no snapshot; so o dono assume o leme e abre o bau; SumNotOwned); v42: PERSISTÊNCIA NO SERVIDOR (MsgLogin/MsgLoginResult, MsgInvSync/MsgInvAck, MsgVitals, MsgProfile, MsgAccountResult, MsgDebugCoins/MsgDebugItems; ActCraft/ActUse/ActInvMove/ActInvSplit/ActInvSort/ActAdReward; slot nas ações Drop/Give/ChestPut/Potion; rev u32 no fim do MsgActAck): o servidor é a autoridade de moedas e inventário; v41: HANDSHAKE COM COOKIE (MsgJoin [skin u8][cookie u32] preenchido até kJoinPacketSize; MsgChallenge = 36) contra amplificação/IP falso, até 40 comandos por Input (120 Hz), stateRel sem saturar (valor fora da faixa vai como float após um marcador); v40: NAVIOS SÓ POR CHAMADO (nenhum nasce ao subir o servidor nem renasce sozinho) + LIMPEZA a cada kShipCleanupSec s dos navios sem ninguém; u8 com os segundos até a limpeza no fim do snapshot (255 = sem limpeza); v39: só 2 skins (pirata homem e mulher, kSkinCount 8 -> 2); v38: PIRATAS no lugar do Y Bot (kSkinCount 16 -> 8: 2 modelos x 4 materiais); v37: CHAMAR NAVIO (MsgSummonShip/MsgSummonResult, tela NAVEGAÇÃO); v36: SKINS (MsgJoin [skin u8], PlayerState.skin no snapshot); v35: CHAT DE TEXTO (MsgChat/MsgChatMsg, balão sobre a cabeça); v34: QUATRO NAVIOS (o bergantim entra no snapshot); v33: MIL JOGADORES (ids u16 em todas as mensagens, snapshot por ÁREA DE INTERESSE com os outros em formato compacto relativo a mim); v32: POÇÕES (ActPotion, StSpeed, kSpeedPotionMult); v31: BACAMARTE (HeldGun/CarryGun, InputCmd/PlayerState.aim + aimPitch, MsgShoot/MsgShotFx, dano de tiro com tiro na cabeça); v30: VIDA DOS NAVIOS (ShipState.hp/sinkT no snapshot, MsgCannonHit com navio atingido, MsgDebugShipHp); v29: MINÉRIO DE FERRO (item 15, ore 3); v28: BAÚ DOS NAVIOS GUARDA ITENS (MsgChestSync, ActChestPut/ActChestTake); v27: TRÊS NAVIOS (a chalupa entra no snapshot); v26: vários navios (PlayerState.shipIdx, ShipState de cada um no snapshot, MsgDebugShip com o tipo); v25: CANHÕES do navio (MsgCannon/MsgCannonFire/MsgCannonHit); v24: OCEANO ABERTO (ilhas por setor, gid de objeto, MsgTakenSync por ilha); v23: navio pirata (ShipState no snapshot, PlayerState.ship/sa/sb, InputCmd.shipEv, MsgDebugShip); v22: dano entre jogadores (MsgDamage/HitConfirm); v21: morto/salvar/carregar (status+link, ActDie/Revive/Carry); v20: MsgRespawn; v19: adaga de pedra na mão/cintura direita; v18: ferramentas guardadas no corpo (carried); v17: tocha na mão; v16: minérios ouro/diamante; v15: cortar/minerar com loot; v14: itens soltos no chão + entregar; v13: coleta de gravetos/pedrinhas; v12 = ferramenta na mão; v11 = construção e portas)
constexpr uint16_t kDefaultPort = 7777;           // UDP
constexpr int kTickHz = 30;                       // taxa de snapshots e de envio de inputs
constexpr int kFirstPublicId = 300;              // ids 0..299 sao RESERVADOS (vendidos na LOJA, aba IDS); quem entra comum recebe de 300 em diante
constexpr int kMaxPlayers = 1000;                 // jogadores simultâneos na sala (ids 0..999, u16 no protocolo)
constexpr int kMaxPacket = 8192;                  // maior pacote (bitset de objetos pegos, sincronia de construções)
// ÁREA DE INTERESSE: cada snapshot leva EU (completo) + os outros mais perto de mim, até kSnapMaxOthers num raio de kInterestRadius,
// em formato compacto relativo à minha posição (stateRel, ~25 B). Assim o pacote fica < ~1,2 KB (sem fragmentar no Wi-Fi) com qualquer
// número de jogadores na sala, e o celular nunca desenha/anima mais que isso.
constexpr float kInterestRadius = 180.0f;          // m
constexpr int kSnapMaxOthers = 40;
constexpr float kEventRadius = 400.0f;             // tiros/canhões só vão a quem está até essa distância (e aos envolvidos)
constexpr uint16_t kNoPlayer = 0xFFFF;             // "ninguém" nos campos de id (u16)
// Redundância de inputs: o cliente reenvia os comandos não confirmados. A 120 Hz, 40 comandos = 333 ms (a 60 Hz, 666 ms) de perda/atraso
// absorvidos; 40 x 29 B + cabeçalho ~ 1,17 KB (< MTU). Era 16 (só 133 ms a 120 Hz).
constexpr int kMaxCmdsPerPacket = 40;
// Handshake: o MsgJoin precisa ter pelo menos kJoinPacketSize bytes (preenchido com zeros) e só vira jogador com o cookie certo
// (MsgChallenge). Assim um Join com IP falso nunca ocupa vaga e a resposta (9 B) é MENOR que o pedido (sem amplificação).
constexpr int kJoinPacketSize = 32;
enum AccountKind : uint8_t { AccIdToken = 1, AccPurchase = 2 };
constexpr int kAccountMaxLen = 1300;   // idToken JWT do Firebase ~0,9-1,2 KB: cabe num pacote
// ---- CONTA / PERSISTÊNCIA (v42) ----
// MsgLogin: kind LoginLocal = visitante local (uid "local-<hex>", NÃO verificado: dados num espaço separado "anon:"), LoginFirebase = idToken JWT
// do Firebase (verificado pelo servidor: RS256 + chaves públicas do Google; uid = sub, espaço "fb:"). prevLocal = uid local antigo (migração 1x).
enum LoginKind : uint8_t { LoginLocal = 1, LoginFirebase = 2 };
enum LoginStatus : uint8_t { LoginOk = 1, LoginGuest = 2, LoginExpired = 3, LoginInvalid = 4, LoginNoKeys = 5, LoginBusy = 6, LoginBadUid = 7 };
enum LoginFlags : uint8_t { LoginHasFlag = 1, LoginRestoredPos = 2, LoginNewAccount = 4, LoginMigrated = 8 };
constexpr int kFlagBytes = 11;            // FlagDesign (bandeira pirata) = 11 bytes de índices
constexpr int kMaxUidLen = 128;
// Recibos (MsgAccount AccPurchase "sku|token") -> MsgAccountResult.status
enum PurchaseStatus : uint8_t { PurchCredited = 1, PurchDuplicate = 2, PurchRejected = 3, PurchNotLogged = 4 };
// Anúncio premiado: o servidor credita (ActAdReward) com limite de taxa (não há como provar que o anúncio passou sem o SSV do AdMob)
constexpr int kAdRewardCoins = 50, kAdRewardPerDay = 30;
constexpr double kAdRewardMinGap = 25.0;   // s entre dois créditos da mesma conta
// moedas de um pacote da Play pelo sku ("coins_500" -> 500); 0 = sku desconhecido
inline int coinsForSku(const char* sku) {
    static const char* k[4] = {"coins_500", "coins_2500", "coins_10000", "coins_50000"}; static const int v[4] = {500, 2500, 10000, 50000};
    for (int i = 0; i < 4; ++i) if (std::strcmp(sku, k[i]) == 0) return v[i];
    return 0;
}
inline uint32_t fnv1a32(const char* s, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)s[i]; h *= 16777619u; } return h; }
constexpr double kTimeoutSec = 5.0;

// Ciclo dia/noite (o servidor é o relógio do mundo)
constexpr float kDayLengthSec = 900.0f;    // 15 min de dia
constexpr float kNightLengthSec = 900.0f;  // 15 min de noite
constexpr float kCycleSec = kDayLengthSec + kNightLengthSec;
constexpr float kStartTimeSec = 120.0f;    // hora inicial (manhã) quando não há outra

// Parâmetros de simulação (devem ser idênticos no cliente e no servidor)
constexpr float kWalkSpeed = 3.0f;       // m/s (com o joystick no máximo)
constexpr float kRunSpeed = 6.0f;        // m/s
constexpr float kSwimSpeed = 2.6f;       // m/s nadando normal
constexpr float kFastSwimSpeed = 4.4f;   // m/s nadando rápido (botão de correr segurado)
constexpr float kTurnRate = 12.0f;       // rad/s: giro do corpo em direção ao movimento
constexpr float kJumpVel = 6.5f;         // m/s (altura ~1.17 m)
constexpr float kGravity = 18.0f;        // m/s²
constexpr float kWorldHalf = 500.0f;     // limite do mundo plano (sem terreno); com terreno o limite é World::kHalf-4
constexpr float kPlayerRadius = 0.35f;   // raio de colisão do jogador (árvores, pedras, construções)
constexpr uint32_t kDefaultWorldSeed = 67;  // ilha padrão (todos os recursos) usada ao criar o primeiro mundo / offline
constexpr float kPi = 3.14159265358979f;

enum InputFlags : uint8_t {
    InputRun = 1,
    InputJump = 2,      // Jump = evento (borda), só vale no chão (ou subir vertical no voo)
    InputActShift = 2,  // bits 2..4 = PlayerAction (evento de animação/ação)
    InputActMask = 7 << InputActShift,
    InputFly = 1 << 5,      // Modo voar ativo (god mode)
    InputFlyFast = 1 << 6,  // Voo rápido
    InputFlyDown = 1 << 7,  // Descer vertical no voo
};

// Ferramenta na mão direita (só visual/cosmético: o cliente informa, o servidor repassa a todos). Modelos em assets/tool_*.e0m.
enum HeldTool : uint8_t { HeldNone = 0, HeldHatchet = 1, HeldPickaxe = 2, HeldTorch = 3, HeldDagger = 4, HeldGun = 5, HeldCount = 6 };
// Itens que o jogador CARREGA no corpo (máscara em InputCmd/PlayerState::carried; cosmético): os que não estão na mão aparecem guardados
enum CarriedBits : uint8_t { CarryHatchet = 1, CarryPickaxe = 2, CarryTorch = 4, CarryKnife = 8, CarryGun = 16, CarryMask = 31 };

enum PlayerAction : uint8_t {
    ActNone = 0,
    ActPunch = 1,   // BATER 1 (soco esquerdo)
    ActChop = 2,    // CORTAR
    ActMine = 3,    // MINERAR
    ActPickup = 4,  // COLETAR
    ActPunch2 = 5,  // BATER 2 (gancho direito)
    ActPunch3 = 6,  // BATER 3 (direto direito)
};

enum Msg : uint8_t {
    MsgJoin = 1,          // C->S  [header][skin u8][cookie u32][zeros até kJoinPacketSize]  (skin da tela PERSONAGEM; cookie 0/errado => o servidor responde MsgChallenge)
    MsgChallenge = 36,    // S->C  [header][cookie u32]  repita o MsgJoin com este cookie (prova que o endereço é seu; vale ~10-20 s)
    MsgAccount = 37,      // C->S  [header][kind u8 = AccountKind][len u16][len bytes]  idToken do Firebase ou recibo da Play ("sku|purchaseToken").
                          //       PONTO DE EXTENSÃO: o servidor hoje só registra (validar exige backend: chaves do Firebase / Google Play Developer API).
    MsgWelcome = 2,       // S->C  [header][id u16][x y z f32][worldHash u32]
    MsgReject = 3,        // S->C  [header][reason u8] 1=servidor cheio
    MsgInput = 4,         // C->S  [header][clientMs u32][count u8][count x InputCmd]
    MsgSnapshot = 5,      // S->C  [header][tick u32][cycleSec f32][echoMs u32][holdMs u16][nOthers u16][EU: state + ackSeq u32][nOthers x stateRel][navios] (ÁREA DE INTERESSE)
    MsgLeave = 6,         // C->S  [header]
    MsgDebugTeleport = 8, // C->S  [header][x f32][z f32]  teleporta o jogador (SÓ com o servidor em --debug)
    MsgDebugShip = 24,    // C->S  [header][x f32][z f32][tipo u8]  (SÓ com --debug) faz nascer o navio do tipo na água perto de (x, z); quem está a bordo vai junto
    MsgCannon = 25,       // C->S  [header][shotId u8][x f32][z f32]  jogador A BORDO manda disparar o canhão: a bala cai em (x, z) (o servidor valida alcance/recarga; reenviado 3x, shotId repetido é ignorado)
    MsgCannonFire = 26,   // S->C  [header][shooter u16][sx sy sz f32][tx ty tz f32][flight f32]  um canhão disparou (todos veem a bala voar por `flight` s)
    MsgChestSync = 28,    // S->C  [header][ship u8][ver u16][kChestSlots x (item u8, n u16)]  conteúdo do baú de um navio (a cada mudança e 1x/s)
    MsgCannonHit = 27,    // S->C  [header][x y z f32][kind u8][ship u8][hp i16][from u16]  a bala chegou: kind 0 = água, 1 = terra, 3 = no casco do navio `ship` (255 = nenhum), que ficou com `hp`; from = quem atirou
    MsgDebugShipHp = 29,  // C->S  [header][ship u8][delta i16]  (SÓ com --debug) soma delta à vida do navio (-50 = dano; <= -1000 = afundar já)
    MsgDebugTime = 7,     // C->S  [header][cycleSec f32]  muda a hora do mundo; SÓ aceito se o servidor foi iniciado com --debug
    MsgCollect = 12,      // C->S  [header][gid u32]  quer pegar o coletável (gid = (ilha << 20) | índice em world.objects; ilha 0 = inicial) (graveto/pedrinha/tecido)
    MsgCollectOk = 13,    // S->C  [header][objIdx u32][type u8]  coleta aceita: o item vai para o inventário de quem pediu
    MsgAct = 15,          // C->S  [header][seq u32][kind u8][a u16][b u16]  ação confiável (reenviada até o Ack): ver ActKind
    MsgActAck = 16,       // S->C  [header][seq u32][kind u8][result u8][item u8][n u16][other u16]
    MsgDropSync = 17,     // S->C  [header][count u16][count x (id u16, item u8, n u16, x f32, y f32, z f32)]  itens soltos no chão
    MsgGift = 18,         // S->C  [header][giftId u32][from u16][item u8][n u16]  item entregue por outro jogador (reenviado até MsgGiftAck)
    MsgDamage = 21,       // S->C  [header][id u32][amount u8][from u16]  dano de outro jogador (reenviado até MsgDamageAck; o cliente aplica cada id 1 vez)
    MsgDamageAck = 22,    // C->S  [header][id u32]
    MsgHitConfirm = 23,   // S->C  [header][target u16][amount u8]  seu golpe acertou o jogador `target` (1 vez, não confiável)
    MsgRespawn = 20,      // C->S  [header]  renascer: o servidor leva o jogador ao ponto de nascimento da ilha (sempre aceito)
    MsgGiftAck = 19,      // C->S  [header][giftId u32]
    MsgTakenSync = 14,    // S->C  [header][ilha u8][nObjs u32][ceil(nObjs/8) bytes]  bitset dos objetos já pegos/derrubados de UMA ilha (um por ilha com algo pego)
    MsgShoot = 30,        // C->S  [header][shotId u16][ox oy oz f32][dx dy dz f32][viewTick f32][flags u8]  disparo do BACAMARTE (origem = boca do cano, dir = para o alvo da mira;
                          //       viewTick = tick do servidor em que o cliente desenhava os outros (compensação de latência); flags bit0 = mirando). Reenviado 2x; shotId repetido é ignorado.
    MsgShotFx = 31,       // S->C  [header][shooter u16][shotId u16][ox oy oz f32][n u8][n x (ex ey ez f32, kind u8, target u16)]  um tiro aconteceu: fim de cada chumbo
                          //       (kind ShotKind; target = jogador atingido ou 255). Todos desenham rastro/impacto; quem atirou já desenhou o seu (ignora o visual, usa o acerto).
    MsgChat = 32,         // C->S  [header][chatId u8][len u8][len bytes Latin-1]  mensagem de texto (reenviada 3x; chatId repetido é ignorado; o servidor limpa o texto e limita a 1 por kChatInterval s)
    MsgSummonShip = 34,   // C->S  [header][id u8][kind u8]  CHAMAR o navio `kind` para o mar aberto à frente de quem pediu (tela NAVEGAÇÃO; reenviado 3x, id repetido recebe o mesmo resultado)
    MsgSummonResult = 35, // S->C  [header][id u8][kind u8][result u8 = SummonResult][dist u16 m]  resposta do servidor
    MsgLogin = 38,        // C->S  [header][reqId u8][kind u8 = LoginKind][uidLen u8][uid][tokLen u16][token][prevLen u8][prevLocal]  (reenviado a cada 1 s até o MsgLoginResult do mesmo reqId)
    MsgLoginResult = 39,  // S->C  [header][reqId u8][status u8 = LoginStatus][flags u8 = LoginFlags][health u8][hunger u8][skin u8][flag kFlagBytes]
    MsgInvSync = 40,      // S->C  [header][rev u32][coins i32][kInvSlots x (id u8, n u16)]  inventário + moedas AUTORITATIVOS (a cada mudança; repetido 1x/s até o MsgInvAck)
    MsgInvAck = 41,       // C->S  [header][rev u32]
    MsgVitals = 42,       // C->S  [header][health u8][hunger u8]  vida/fome (são do cliente; o servidor só guarda para a próxima sessão). A cada ~5 s.
    MsgProfile = 43,      // C->S  [header][flag kFlagBytes]  bandeira salva no editor (reenviado 3x)
    MsgAccountResult = 44,// S->C  [header][kind u8][status u8 = PurchaseStatus][coins i32 creditadas][hash u32 = fnv1a32("sku|token")]
    MsgDebugCoins = 45,   // C->S  [header][delta i32]  (SÓ com --debug) cheat de moedas
    MsgDebugItems = 46,   // C->S  [header][item u8][n u16]  (SÓ com --debug) cheat: dá n do item (item 0 = 1 de cada)
    // TRIPULAÇÃO no jogo (ver shared/team.h): bandeira ao lado do id na TAG e CONVITES pelo menu de segurar no jogador
    MsgTeamQuery = 47,    // C->S  [header][n u8][n x id u16]  quero saber a tripulação destes jogadores (n <= kTeamQueryMax)
    MsgTeamInfo = 48,     // S->C  [header][n u8][n x (id u16, teamId u32 (0 = nenhuma), flag kFlagBytes)]
    MsgTeamInvite = 49,   // C->S  [header][inviteId u32][target u16]  convidar o jogador para a MINHA tripulação (reenviado 3x; inviteId repetido recebe a mesma resposta)
    MsgTeamOffer = 50,    // S->C  [header][offerId u32][from u16][members u8][flag kFlagBytes][nameLen u8][nome]  você foi convidado (repetido 1x/s até a resposta)
    MsgTeamOfferReply = 51,// C->S [header][offerId u32][accept u8]  resposta ao convite (reenviada 3x)
    MsgTeamPos = 53,      // S->C  [header][n u8][n x (id u16, x f32, z f32)]  onde estão os OUTROS tripulantes online (1x/s; qualquer distância: minimapa/mapa)
    MsgTeamChatPush = 54, // S->C  [header][system u8][authorLen u8][autor][textLen u8][texto]  mensagem nova no chat da tripulação (tripulantes online no jogo)
    // DOMINAÇÃO DE ILHAS (shared/conquest.h). Mensagens novas: o servidor/app antigo só as ignora (kProtocolMagic igual).
    MsgIslandOwners = 55, // S->C  [header][n u8][n x (slot u8, teamId u32, flag kFlagBytes)]  ilhas dominadas (Welcome, a cada mudança e a cada 15 s)
    MsgIslandCap = 56,    // S->C  [header][slot u8][owner u32][pole u32][prog u8 0..255][state u8 = CapState][nAtt u8][nDef u8][shield u16 s][ownerFlag][poleFlag][nameLen u8][dona][nameLen u8][do mastro]
                          //       estado do MASTRO de uma ilha, 5x/s, a quem está a até kCapNearRadius dele
    MsgIslandEvent = 57,  // S->C  [header][kind u8 = IslandEventKind][slot u8][value i32][nameLen u8][nome da tripulação]
    // MELHORIAS DA ILHA (shared/isle_build.h): a tripulação dona ergue estruturas na ilha dominada (9..11 eram do sistema de construção antigo: não reusar)
    MsgStructReq = 58,    // C->S  [header][reqId u16][op u8 (1 erguer, 2 demolir)][slot u8][kind u8][rot u8][x f32][z f32][id u16]  (reenviado 3x; reqId repetido = mesma resposta)
    MsgStructRes = 59,    // S->C  [header][reqId u16][op u8][result u8 = StructRes][kind u8]
    MsgStructSync = 60,   // S->C  [header][slot u8][n u8][n x (id u16, kind u8, rot u8, x f32, y f32, z f32)]  estruturas de UMA ilha (Welcome, a cada mudança, e a cada 20 s)
    MsgTeamNotice = 52,   // S->C  [header][kind u8 = TeamNoticeKind][result u8 = TeamResult][other u16][nameLen u8][nome da tripulação]
    MsgChatMsg = 33,      // S->C  [header][from u16][len u8][len bytes Latin-1]  alguém disse algo: só para quem está a até kChatRadius m dele (inclui quem falou = confirmação de entrega)
};
// CHAMAR NAVIO: o servidor tira o navio de onde estiver (se ninguém estiver a bordo) e o põe no mar aberto à frente do jogador (shipFindAhead).
enum SummonResult : uint8_t { SumOk = 0, SumNoWater = 1, SumInUse = 2, SumCooldown = 3, SumSinking = 4, SumAboard = 5, SumBusy = 6, SumNoShip = 7, SumNotOwned = 8 };
// Preço (moedas) de cada navio, na ordem de ShipKind (navio, galeão, chalupa, bergantim). A conta começa SEM navio; comprou, é dela para sempre.
constexpr int kShipPrice[4] = {5000, 15000, 1500, 9000};
constexpr double kSummonCooldown = 25.0;   // s entre dois chamados bem-sucedidos do mesmo jogador
// CHAT: texto em Latin-1 de 1 byte por caractere: ASCII 32..126 + letras acentuadas 0xC0..0xFF (sem × e ÷); a fonte desenha o acento por cima da letra maiúscula. O balão fica sobre a cabeça por alguns segundos.
constexpr int kChatMaxLen = 80;
constexpr double kChatInterval = 1.0;      // s: intervalo mínimo entre mensagens de um jogador (anti-spam)
constexpr float kChatRadius = 180.0f;      // m: quem está mais longe não recebe (mesmo raio da área de interesse)
constexpr float kChatShowSec = 4.0f;       // s mínimos com o balão visível (+ 0,06 s por caractere, máx. kChatShowMax)
constexpr float kChatShowMax = 9.0f;
// Limpa o texto: só ASCII 32..126 e letras latinas 0xC0..0xFF, espaços repetidos viram um, sem espaço nas pontas, no máx. kChatMaxLen. Devolve o tamanho (0 = vazio/ inválido).
inline int chatSanitize(const char* in, int n, char* out) {
    int m = 0; bool sp = true;
    for (int i = 0; i < n && m < kChatMaxLen; ++i) {
        unsigned char ch = (unsigned char)in[i];
        if (ch < 32 || (ch > 126 && ch < 0xC0) || ch == 0xD7 || ch == 0xF7 || ch == 127) continue;
        if (ch == ' ') { if (sp) continue; sp = true; } else sp = false;
        out[m++] = (char)ch;
    }
    while (m > 0 && out[m - 1] == ' ') --m;
    out[m] = 0;
    return m;
}
// BACAMARTE (arma de fogo de chumbo, servidor autoritativo): cada tiro solta kGunPellets chumbos num cone (mais aberto sem mirar). O servidor
// volta os outros jogadores para onde o atirador os via (viewTick) e testa cada chumbo contra a CABEÇA (esfera) e o CORPO (cápsula); relevo bloqueia.
// TIRO NA CABEÇA = MORTE CERTA (dano kGunHeadDamage = 255: o cliente zera a vida). Corpo: kGunBodyDamage por chumbo, cai com a distância.
enum ShotKind : uint8_t { ShotNone = 0, ShotTerrain = 1, ShotWater = 2, ShotBody = 3, ShotHead = 4 };
constexpr int kGunPellets = 7;
constexpr float kGunRange = 70.0f;           // m: depois disso o chumbo some
constexpr float kGunSpreadAim = 0.022f;      // rad (meio-ângulo do cone) mirando
constexpr float kGunSpreadHip = 0.085f;      // rad sem mirar (disparo do quadril)
constexpr float kGunReload = 2.2f;           // s para recarregar (1 tiro por carga)
constexpr float kGunMinInterval = 1.6f;      // s: intervalo mínimo entre tiros aceitos pelo servidor (folga para a latência)
constexpr int kGunBodyDamage = 10;           // por chumbo no corpo (7 chumbos à queima-roupa = 70: 2 tiros bons no corpo matam)
constexpr uint8_t kGunHeadDamage = 255;      // tiro na cabeça: morte certa
constexpr float kGunFullDmgDist = 6.0f, kGunMinDmgK = 0.10f;    // corpo: até 6 m dano cheio; cai linearmente até 10% no alcance máximo
constexpr float kGunHeadKillDist = 20.0f;                       // cabeça: morte certa só até 20 m; depois o dano cai de 90 (20 m) até 30 (alcance máximo)
constexpr float kGunMaxOriginDist = 3.0f;    // a origem do tiro tem de estar a no máx. isso do peito do atirador (no servidor)
constexpr float kAimWalkSpeed = 2.2f;        // m/s andando mirando (não corre)
// Caixas de acerto (relativas aos pés): cabeça = esfera; corpo = cápsula vertical do quadril aos ombros
constexpr float kHeadY = 1.63f, kHeadR = 0.15f, kBodyY0 = 0.18f, kBodyY1 = 1.42f, kBodyR = 0.27f;
inline int gunBodyDamage(float dist) {
    float k = dist <= kGunFullDmgDist ? 1.0f : std::max(kGunMinDmgK, 1.0f - (1.0f - kGunMinDmgK) * (dist - kGunFullDmgDist) / (kGunRange - kGunFullDmgDist));
    return std::max(1, (int)(kGunBodyDamage * k + 0.5f));
}
inline uint8_t gunHeadDamage(float dist) {   // tiro na cabeça: morte certa de perto; de longe tira cada vez menos vida
    if (dist <= kGunHeadKillDist) return kGunHeadDamage;
    const float k = std::min(1.0f, (dist - kGunHeadKillDist) / (kGunRange - kGunHeadKillDist));
    return (uint8_t)(90.0f - 60.0f * k + 0.5f);
}
// Raio x esfera: menor t >= 0 (ou -1). d normalizado.
inline float rayHitSphere(float ox, float oy, float oz, float dx, float dy, float dz, float cx, float cy, float cz, float r) {
    float lx = ox - cx, ly = oy - cy, lz = oz - cz;
    float b = lx * dx + ly * dy + lz * dz, c = lx * lx + ly * ly + lz * lz - r * r, disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    float sq = std::sqrt(disc), t = -b - sq;
    if (t < 0.0f) t = -b + sq;
    return t >= 0.0f ? t : -1.0f;
}
// Raio x cápsula vertical (eixo em (x, z), de y0 a y1, raio r): cilindro aberto + as 2 esferas das pontas. Menor t >= 0 (ou -1). d normalizado.
inline float rayHitCapsule(float ox, float oy, float oz, float dx, float dy, float dz, float x, float y0, float y1, float z, float r) {
    float best = -1.0f;
    auto take = [&](float t) { if (t >= 0.0f && (best < 0.0f || t < best)) best = t; };
    const float lx = ox - x, lz = oz - z, A = dx * dx + dz * dz;
    if (A > 1e-8f) {
        const float B = 2.0f * (lx * dx + lz * dz), Cc = lx * lx + lz * lz - r * r, disc = B * B - 4.0f * A * Cc;
        if (disc >= 0.0f) {
            const float sq = std::sqrt(disc);
            for (float t : {(-B - sq) / (2.0f * A), (-B + sq) / (2.0f * A)}) {
                const float y = oy + dy * t;
                if (t >= 0.0f && y >= y0 && y <= y1) { take(t); break; }
            }
            if (Cc <= 0.0f && oy >= y0 && oy <= y1) take(0.0f);   // começa dentro
        }
    }
    take(rayHitSphere(ox, oy, oz, dx, dy, dz, x, y0, z, r));
    take(rayHitSphere(ox, oy, oz, dx, dy, dz, x, y1, z, r));
    return best;
}

// Ações confiáveis do inventário (MsgAct). DROP: a=item, b=quantidade -> solta à frente do jogador. PICKUP: a=id do item solto.
// GIVE: a=item, b=quantidade -> entrega ao jogador mais próximo (<= kGiveMaxDist). Resultado em MsgActAck.result: 1 = ok, 0 = recusado.
// ActHit: a = índice do objeto na ilha, b = ilha (palmeira/coqueiro/tora: precisa MACHADINHA; pedra: precisa PICARETA). Ack.result: 0 recusado (other: 1 longe, 2 ferramenta errada, 3 já sumiu),
// 1 golpe ok, 2 golpe ok e o objeto acabou (sai do mundo; o bitset de MsgTakenSync marca o índice). O servidor sorteia e solta o material no chão.
// ActDie: o cliente avisa que morreu (a vida é do cliente) -> o servidor marca StDead. ActRevive/ActCarry: a = id do jogador caído (perto, morto, não carregado): salvar (RCP por
// kReviveTime s; cancela se qualquer um se afastar/mexer) / carregar no ombro. ActDropCarry: larga quem carrega. Ack.result 1 = ok, 0 = recusado.
enum ActKind : uint8_t { ActDrop = 1, ActPickupDrop = 2, ActGive = 3, ActHit = 4, ActDie = 5, ActRevive = 6, ActCarry = 7, ActDropCarry = 8, ActChestPut = 9, ActChestTake = 10, ActPotion = 11,
                         ActCraft = 12, ActUse = 13, ActInvMove = 14, ActInvSplit = 15, ActInvSort = 16, ActAdReward = 17, ActBuyShip = 18,
                         ActBuyTeamLicense = 19,
                         ActGiveTo = 20 };   // DAR para UM jogador (menu de segurar): a = slot (5 bits) | alvo << 5, b = quantidade   // LICENÇA DE TRIPULAÇÃO pela LOJA do jogo (Ack: other = TeamResult; ver shared/team.h)
// v42 (inventário do SERVIDOR): DROP/GIVE a = item | (slot+1) << 8 (slot 0 = qualquer); CHEST PUT b = n | (slot+1) << 8; POTION b = slot+1.
// CRAFT a = receita (recipeTable); USE a = slot+1, b = item (come/bebe/gasta 1: coco, poções de cura, munição ao recarregar); INV MOVE/SPLIT a = de | para << 8;
// INV SORT (mochila); AD REWARD a = onde (1 loja) -> +kAdRewardCoins com limite de taxa. MsgActAck termina com [rev u32] = revisão do inventário depois da ação.   // ActPotion: a = PotionKind (velocidade/escudo; a cura é do cliente)
enum PotionKind : uint16_t { PotionSpeed = 1, PotionShield = 2 };
constexpr float kSpeedPotionMult = 1.5f, kSpeedPotionSec = 15.0f, kShieldPotionSec = 8.0f;   // +50% de velocidade por 15 s; invulnerável por 8 s
// Baú do navio: PUT a = item | (navio << 8), b = quantidade -> Ack.result 1 + n = quantos coube (o resto volta para quem pediu). TAKE a = slot | (navio << 8), b = quantidade -> Ack.result 1 + item + n = quantos saíram.
constexpr float kHitMaxReach = 2.2f;   // da superfície do objeto ao jogador (m)
constexpr float kGiveMaxDist = 4.0f;
constexpr int kMaxDrops = 64;
constexpr float kDropForward = 0.9f;   // distância à frente em que o item cai

// Coleta (validação no servidor): distância horizontal máxima do jogador ao item e diferença de altura.
constexpr float kCollectMaxDist = 2.8f;
constexpr float kCollectMaxDy = 2.0f;
constexpr uint32_t kMaxTakenBits = (kMaxPacket - 16) * 8;   // bitset cabe num pacote (9 mil objetos)

// DANO ENTRE JOGADORES (servidor autoritativo): ao BATER/CORTAR/MINERAR (evento de ação do input) o servidor espera o instante do impacto e acerta o jogador vivo
// mais perto À FRENTE (cone de ±60°, até kMeleeReach m do centro). O dano depende do que está na MÃO do atacante (HeldTool). A vida é do cliente: o servidor manda
// MsgDamage (confiável por reenvio) e o cliente desconta.
constexpr float kMeleeReach = 1.7f, kMeleeCos = 0.5f;
inline int meleeDamage(uint8_t held) {
    switch (held) {
        case HeldHatchet: return 15;   // machadinha
        case HeldPickaxe: return 18;   // picareta (lenta)
        case HeldTorch:   return 7;    // tocha
        case HeldDagger:  return 25;   // adaga de pedra: a melhor arma de autodefesa
        default:          return 5;    // mão nua
    }
}

// Estado de vida em multiplayer: quem MORRE fica caído no chão (com a TAG "ID + MORTO") até ser salvo (RCP, kReviveTime s) por outro jogador, carregado
// no ombro, ou renascer sozinho (MsgRespawn). O servidor é a autoridade do status e o replica em todo snapshot.
constexpr double kSpawnProtectSec = 10.0;   // invulnerabilidade ao entrar/renascer
enum StatusBits : uint8_t { StDead = 1, StCarried = 2, StCarrying = 4, StRescuing = 8, StGunner = 16, StProtected = 32, StSpeed = 64 };   // StProtected: invencível logo após nascer/logar/renascer (kSpawnProtectSec); some ao atacar   // StGunner: o servidor concedeu um dos canhões (máx. kMaxGunners por navio)
constexpr int kMaxGunners = 2;
constexpr float kReviveTime = 4.0f;      // s de RCP até o jogador caído levantar
constexpr float kRescueMaxDist = 2.6f;   // distância (m) para salvar/carregar um jogador caído

struct PlayerState {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0;  // y = altura dos pés
    float yaw = 0;              // direção do CORPO (0 = -Z), radianos
    float speed = 0;            // m/s horizontal atual (para animações)
    float vy = 0;               // velocidade vertical (pulo)
    uint8_t action = 0;         // bits 0..2 = PlayerAction (1..4), bits 3..7 = contador de disparo (1..31)
    uint8_t held = 0;           // HeldTool: ferramenta na mão (0 = nada)
    uint8_t carried = 0;        // CarriedBits: ferramentas carregadas no corpo
    uint8_t status = 0;         // StatusBits: morto / sendo carregado / carregando / salvando alguém (decidido pelo SERVIDOR)
    uint16_t link = 0;          // id do outro jogador + 1 (carregado: quem me carrega; carregando: quem eu carrego; salvando: quem eu salvo)
    uint8_t ship = 0;           // 0 = fora do navio, 1 = a bordo, 2 = no LEME (parado, controlando o navio)
    uint8_t shipIdx = 0;        // em qual navio (shipState(i)); só vale com ship != 0
    float sa = 0, sb = 0;       // a bordo: posição no espaço do navio (a = para a proa, b = para a direita); x/z do mundo são derivados da pose do navio
    uint8_t aim = 0;            // AimBits (bacamarte): mirando / recarregando (cosmético: o servidor só repassa; mirando também muda a física: ver stepPlayer)
    int8_t aimPitch = 0;        // inclinação da mira em graus (+ = para cima), para os outros verem o cano apontar
    uint8_t skin = 0;           // aparência escolhida na tela PERSONAGEM (cosmético; tabela em engine/src/skins.h; o servidor só guarda o que veio no Join)
};
constexpr int kSkinCount = 4;   // quantas skins existem (engine/src/skins.h tem exatamente kSkinCount entradas)

// ---- Canhões do navio: quem está a bordo vê de cima e toca onde a bala deve cair (o servidor decide tudo) ----
constexpr float kCannonReload = 3.0f;      // s entre disparos de cada jogador
constexpr float kCannonRadius = 5.0f;      // raio do estrago no impacto (m)
constexpr int kCannonDamage = 45;          // dano no centro (cai com a distância até 55%)
inline float cannonFlightTime(float dist) { return 0.9f + dist / 55.0f; }

// Eventos de navio (InputCmd.shipEv, bits 0..2 = evento de borda; bits 3..4 = acelerador do timoneiro: 1 acelerar, 2 ré)
enum ShipEv : uint8_t { ShipEvBoard = 1, ShipEvLeave = 2, ShipEvHelm = 4, ShipEvEvents = 7, ShipThrShift = 3, ShipGunWant = 32 };   // ShipGunWant (bit 5, nível): quer ficar nos canhões

struct InputCmd {
    uint32_t seq = 0;
    float moveX = 0, moveY = 0;  // joystick -1..1 (direita, frente), relativo à câmera
    float camYaw = 0;            // yaw da câmera
    float camPitch = 0;          // pitch da câmera (para voo 3D na direção da mira)
    float dt = 0;
    uint8_t flags = 0;           // InputFlags
    uint8_t held = 0;            // HeldTool empunhada (cosmético; o servidor só valida o intervalo)
    uint8_t carried = 0;         // CarriedBits (cosmético)
    uint8_t shipEv = 0;          // ShipEv: embarcar / desembarcar / assumir-largar o leme (eventos) + acelerador no leme
    uint8_t aim = 0;             // AimBits
};
enum AimBits : uint8_t { AimOn = 1, AimReload = 2, AimMask = 3 };
// Mirando com o bacamarte na mão (em terra ou a bordo, fora d'água): anda devagar (kAimWalkSpeed), não corre e o corpo fica virado para a câmera.
inline bool aimingGun(const PlayerState&, const InputCmd& c) { return (c.aim & AimOn) && c.held == HeldGun && !(c.flags & InputFly); }

// Terreno usado pela simulação (servidor e cliente definem o mesmo mundo). nullptr = chão plano em y=0.
// (altura, bioma e colisão em qualquer ponto do OCEANO: ilha inicial + ilhas geradas + mar fundo; ver terrain.h). nullptr = chão plano em y = 0.
inline const Terrain*& terrainPtr() { static const Terrain* t = nullptr; return t; }
inline void setTerrain(const World* w) { terrain().setMain(w); terrainPtr() = w ? &terrain() : nullptr; }

// ---- Navio: quem está a bordo guarda (sa, sb) no espaço do navio; x/z do mundo vêm da pose atual do navio ----
inline ShipState& shipOf(const PlayerState& p) { return shipState(p.shipIdx < kMaxShips ? p.shipIdx : 0); }
inline void shipCarry(PlayerState& p) {
    if (!p.ship) return;
    shipLocalToWorld(shipOf(p), p.sa, p.sb, p.x, p.z);
}
inline void shipDetachPlayer(PlayerState& p) {   // sai do navio sem mexer na posição (voar, teleporte, navio sumiu)
    ShipState& S = shipOf(p);
    if (S.helmer == p.id && p.ship == 2) { S.helmer = -1; S.cmdSteer = 0; S.cmdThrottle = 0; }
    p.ship = 0;
}
// Desce do navio: cai na água ao lado do casco, do lado em que estava (DESCER e naufrágio)
inline void shipEject(PlayerState& p) {
    if (!p.ship) return;
    const ShipState& S = shipOf(p); const ShipDef& D = shipDef(S.kind);
    shipDetachPlayer(p);
    float side = p.sb >= 0.0f ? 1.0f : -1.0f;
    float la = std::max(-D.leaveA, std::min(D.leaveA, p.sa)), lb = side * (D.hullHalfB + 2.2f);
    shipLocalToWorld(S, la, lb, p.x, p.z);
    const Terrain* W = terrainPtr();
    p.y = W ? std::max(W->heightAt(p.x, p.z), World::kSwimFloorH) : 0.0f;
    p.vy = 0.0f; p.speed = 0.0f;
}
// Navio mais perto ao alcance de embarque de (x, y, z); -1 = nenhum. Usado na física e na HUD: os 2 sempre concordam.
// Alguém "usa" o navio i: está a bordo (convés, leme ou canhão) ou encostado no casco (a até kShipBoardDist m, nadando ao lado, prestes a embarcar).
// Usado pela LIMPEZA de navios (servidor e cliente offline): navio que ninguém usa some quando o contador chega a zero.
inline bool shipInUseBy(int i, const PlayerState& p) {
    if (p.ship && p.shipIdx == i) return true;
    const ShipState& S = shipState(i);
    if (!S.exists) return false;
    const ShipDef& D = shipDef(i);
    float a, b; shipWorldToLocal(S, p.x, p.z, a, b);
    const float ca = std::max(D.lenA0, std::min(D.lenA1, a)), cb = std::max(-D.hullHalfB, std::min(D.hullHalfB, b));
    return (a - ca) * (a - ca) + (b - cb) * (b - cb) <= kShipBoardDist * kShipBoardDist;
}
inline int shipBoardTarget(float x, float y, float z) {
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < kMaxShips; ++i) {
        const ShipState& S = shipState(i);
        if (!shipAfloat(S)) continue;   // afundando / afundado: ninguém embarca
        const ShipDef& D = shipDef(i);
        float a, b; shipWorldToLocal(S, x, z, a, b);
        float ca = std::max(D.lenA0, std::min(D.lenA1, a)), cb = std::max(-D.hullHalfB, std::min(D.hullHalfB, b));
        float d = std::sqrt((a - ca) * (a - ca) + (b - cb) * (b - cb));
        if (d <= kShipBoardDist && y <= D.deckY + 0.5f && d < bd) { bd = d; best = i; }
    }
    return best;
}
// Eventos de borda do input: embarcar (perto do casco), desembarcar (ao lado do casco), assumir/largar o leme (perto dele)
inline void shipEvents(PlayerState& p, uint8_t ev) {
    if ((ev & ShipEvBoard) && !p.ship) {
        int i = shipBoardTarget(p.x, p.y, p.z);
        if (i >= 0) {
            const ShipState& S = shipState(i); const ShipDef& D = shipDef(i);
            float a, b; shipWorldToLocal(S, p.x, p.z, a, b);
            shipClampDeck(D, a, b);
            p.ship = 1; p.shipIdx = (uint8_t)i; p.sa = a; p.sb = b; p.y = D.deckY; p.vy = 0.0f; p.speed = 0.0f;
            shipCarry(p);
        }
    } else if ((ev & ShipEvLeave) && p.ship) {
        shipEject(p);
    } else if ((ev & ShipEvHelm) && p.ship) {
        ShipState& S = shipOf(p); const ShipDef& D = shipDef(S.kind);
        if (p.ship == 2) { shipDetachPlayer(p); p.ship = 1; }
        else if (std::sqrt((p.sa - D.helmA) * (p.sa - D.helmA) + p.sb * p.sb) <= D.helmReach && std::fabs(p.y - D.terraceY) < 0.4f && (S.helmer < 0 || S.helmer == p.id) && (S.owner < 0 || S.owner == p.id)) {
            p.ship = 2; S.helmer = p.id; p.sa = D.helmA; p.sb = 0.0f; p.yaw = S.yaw;
            shipCarry(p);
        }
    }
}

// Altura de apoio do jogador (terreno + mar + convés do navio + cais da muralha).
inline float supportHeightAt(float x, float z, float curY = 1000.0f) {
    const Terrain* W = terrainPtr();
    float h = W ? std::max(W->heightAt(x, z), World::kSwimFloorH) : 0.0f;
    for (int i = 0; i < kMaxShips; ++i) {
        const ShipState& S = shipState(i);
        if (shipAfloat(S)) {
            float a, b; shipWorldToLocal(S, x, z, a, b);
            float f = shipFloor(shipDef(i), a, b);
            if (f > kNoFloor * 0.5f && curY + 1.2f >= f) h = std::max(h, f);
        }
    }
    if (W) { const float f = isleFloorAt(x, z); if (f > -1e8f && curY + 2.4f >= f) h = std::max(h, f); }   // CAIS da muralha (MELHORIAS DA ILHA): de terra ou nadando ao lado (sobe como numa escada)
    return h;
}


// Verdadeiro quando o personagem está em água funda (mar) e na altura de flutuação/nado.
inline bool isSwimmingPos(float x, float y, float z) {
    const Terrain* W = terrainPtr();
    if (!W) return false;
    return W->heightAt(x, z) <= World::kSwimFloorH + 0.05f && y <= World::kSwimFloorH + 0.12f;
}

inline float wrapPi(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a < 0) a += 2.0f * kPi;
    return a - kPi;
}

// Passo de simulação determinístico: usado pelo servidor (autoridade) e pelo cliente (predição).
// Movimento relativo à câmera; o corpo gira suavemente para a direção do movimento. Com terreno: o jogador anda sobre o
// relevo (altura = World::heightAt), boia/nada no mar quando o fundo passa de World::kSwimFloorH (-1.20 m), não sobe
// barrancos > ~49°, e é empurrado para fora de árvores, pedras e construções. Sem terreno: chão plano em y = 0.
// A bordo do navio: anda no espaço do navio (a, b) com o convés como chão; no leme fica parado e o joystick vira o leme.
// Mirando: o corpo vira para onde a câmera olha (rápido, sem esperar o movimento) e o estado de mira vai para o snapshot.
inline void aimFace(PlayerState& p, const InputCmd& c) {
    float diff = wrapPi(c.camYaw - p.yaw), maxTurn = 22.0f * c.dt;
    p.yaw = wrapPi(p.yaw + std::max(-maxTurn, std::min(maxTurn, diff)));
}
inline void setAimState(PlayerState& p, const InputCmd& c) {
    p.aim = p.held == HeldGun ? (uint8_t)(c.aim & AimMask) : 0;
    float deg = std::isfinite(c.camPitch) ? c.camPitch * 57.29578f : 0.0f;
    p.aimPitch = (p.aim & AimOn) ? (int8_t)std::max(-80.0f, std::min(80.0f, deg)) : 0;
}
inline void stepAboard(PlayerState& p, const InputCmd& c) {
    ShipState& S = shipOf(p);
    if (!S.exists) { shipDetachPlayer(p); return; }
    if (S.sinkT > 0.0f) { shipEject(p); return; }   // o navio está afundando: todo mundo vai para a água
    const ShipDef& D = shipDef(S.kind);
    const float dt = c.dt;
    float mx = c.moveX, my = c.moveY;
    float len = std::sqrt(mx * mx + my * my);
    if (len > 1.0f) { mx /= len; my /= len; len = 1.0f; }
    if (p.ship == 2) {
        if ((S.helmer < 0 || S.helmer == p.id) && (S.owner < 0 || S.owner == p.id)) {
            S.helmer = p.id;
            S.cmdSteer = std::max(-1.0f, std::min(1.0f, c.moveX));
            uint8_t t = (c.shipEv >> ShipThrShift) & 3u;
            S.cmdThrottle = t == 1 ? 1 : (t == 2 ? -1 : 0);
        } else p.ship = 1;   // outro jogador já está no leme
        if (p.ship == 2) {
            p.sa = D.helmA; p.sb = 0.0f; p.y = D.terraceY; p.vy = 0.0f; p.speed = 0.0f; p.yaw = S.yaw;
            shipCarry(p);
            p.held = c.held < HeldCount ? c.held : (uint8_t)HeldNone;
            p.carried = c.carried & CarryMask;
            p.aim = 0; p.aimPitch = 0;
            return;
        }
    }
    if (shipFloor(D, p.sa, p.sb) < kNoFloor * 0.5f) {   // posição inicial fora do chão (teleporte/spawn): traz para o convés principal
        float a = p.sa, b = p.sb; shipClampDeck(D, a, b); p.sa = a; p.sb = b; p.y = D.deckY; p.vy = 0.0f;
    }
    const float floor0 = shipFloor(D, p.sa, p.sb);
    const bool wasGrounded = p.y <= floor0 + 0.03f && p.vy <= 0.0f;
    const bool aimG = aimingGun(p, c);
    if (len < 0.01f) {
        p.speed = 0.0f;
    } else {
        float spd = (aimG ? kAimWalkSpeed : ((c.flags & InputRun) ? kRunSpeed : kWalkSpeed)) * len * ((p.status & StSpeed) ? kSpeedPotionMult : 1.0f);
        float dx = std::sin(c.camYaw) * my + std::cos(c.camYaw) * mx;
        float dz = -std::cos(c.camYaw) * my + std::sin(c.camYaw) * mx;
        float n = std::sqrt(dx * dx + dz * dz);
        dx /= n; dz /= n;
        float sy = std::sin(S.yaw), cy = std::cos(S.yaw);
        float da = dx * sy - dz * cy, db = dx * cy + dz * sy;   // direção no espaço do navio
        float na = p.sa + da * spd * dt, nb = p.sb + db * spd * dt, a = na, b = nb;
        if (shipResolve(D, a, b, p.y)) { p.sa = a; p.sb = b; }
        else {   // desliza: tenta cada eixo do navio separado (paredes do toldo, amurada, mastros)
            a = na; b = p.sb;
            if (shipResolve(D, a, b, p.y)) { p.sa = a; p.sb = b; }
            else { a = p.sa; b = nb; if (shipResolve(D, a, b, p.y)) { p.sa = a; p.sb = b; } }
        }
        if (!aimG) {
            float diff = wrapPi(std::atan2(dx, -dz) - p.yaw);
            float maxTurn = kTurnRate * dt;
            p.yaw = wrapPi(p.yaw + std::max(-maxTurn, std::min(maxTurn, diff)));
        }
        p.speed = spd;
    }
    if (aimG) aimFace(p, c);
    const float g = shipFloor(D, p.sa, p.sb);   // chão sob os pés (convés, rampa da escada ou toldo)
    if (wasGrounded) {
        if (c.flags & InputJump) p.vy = kJumpVel;
        else if (p.y - g < 0.6f) { p.y = g; p.vy = 0.0f; }
    }
    if (p.vy > 0.0f || p.y > g + 0.001f) {
        p.vy -= kGravity * dt;
        p.y += p.vy * dt;
        if (p.y <= g && p.vy <= 0.0f) { p.y = g; p.vy = 0.0f; }
    }
    if (p.y < g) { p.y = g; if (p.vy < 0.0f) p.vy = 0.0f; }
    shipCarry(p);
    p.held = c.held < HeldCount ? c.held : (uint8_t)HeldNone;
    p.carried = c.carried & CarryMask;
    setAimState(p, c);
    uint8_t act = (c.flags >> InputActShift) & 7u;
    if (act >= ActPunch && act <= ActPunch3 && wasGrounded) {
        uint8_t seq = ((p.action >> 3) + 1u) & 0x1Fu;
        if (seq == 0) seq = 1u;
        p.action = (uint8_t)((seq << 3) | act);
    }
}

// Caiu/pousou no convés (voando, por exemplo): passa a ser carregado pelo navio
inline void shipTryAutoBoard(PlayerState& p) {
    if (p.ship || p.vy > 0.0f) return;
    for (int i = 0; i < kMaxShips; ++i) {
        const ShipState& S = shipState(i); const ShipDef& D = shipDef(i);
        if (!shipAfloat(S) || std::fabs(p.y - D.deckY) > 0.04f) continue;
        float a, b; shipWorldToLocal(S, p.x, p.z, a, b);
        if (a >= D.aMin && a <= D.aMax && std::fabs(b) <= shipDeckHalfW(D, a)) { p.ship = 1; p.shipIdx = (uint8_t)i; p.sa = a; p.sb = b; return; }
    }
}

// Baú: o jogador está a bordo deste navio, no convés, perto da caixa do baú?
inline bool chestInReach(const PlayerState& p, int ship) {
    if (ship < 0 || ship >= kMaxShips || !p.ship || p.shipIdx != ship || !shipAfloat(shipState(ship))) return false;
    const ShipDef& D = shipDef(ship);
    return std::fabs(p.y - D.deckY) < 0.6f && shipChestDist(D, p.sa, p.sb) <= D.chestReach + 0.6f;
}
// Aplica uma ação de baú (ActChestPut/Take) de `p`. Devolve result do Ack (0/1) e preenche item/n do Ack. Servidor e cliente offline usam esta.
inline uint8_t chestAct(Chest* chests, const PlayerState& p, uint8_t kind, uint16_t a, uint16_t b, uint8_t& item, uint16_t& n, int* shipOut = nullptr) {
    const int ship = a >> 8; item = 0; n = 0;
    if (shipOut) *shipOut = ship;
    if (!chestInReach(p, ship)) return 0;
    { const ShipState& SS = shipState(ship); if (SS.owner >= 0 && SS.owner != (int)p.id) return 0; }   // o baú é do dono do navio
    Chest& c = chests[ship];
    if (kind == ActChestPut) {
        item = (uint8_t)(a & 0xFF);
        const int q = b & 0xFF;   // v42: b = n | (slot+1) << 8
        if (item == 0 || item >= kItemCount || q == 0) return 0;
        int k = chestPut(c, item, q);
        n = (uint16_t)k;
        return k > 0 ? 1 : 0;
    }
    int k = chestTake(c, a & 0xFF, b, item);
    n = (uint16_t)k;
    return k > 0 ? 1 : 0;
}

inline void stepPlayer(PlayerState& p, const InputCmd& c) {
    const Terrain* W = terrainPtr();
    const float lim = W ? Terrain::limit() : kWorldHalf;

    const bool flying = (c.flags & InputFly) != 0;
    if (flying) { if (p.ship) shipDetachPlayer(p); }
    else {
        if (c.shipEv & ShipEvEvents) shipEvents(p, c.shipEv);
        if (p.ship) { stepAboard(p, c); return; }
    }
    if (flying) {
        // --- Modo Voar (God Mode) ---
        float mx = c.moveX, my = c.moveY;
        float len = std::sqrt(mx * mx + my * my);
        if (len > 1.0f) { mx /= len; my /= len; len = 1.0f; }

        float baseSpd = (c.flags & InputFlyFast) ? 24.0f : ((c.flags & InputRun) ? 18.0f : 9.0f);
        float spd = baseSpd * len;

        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        if (len >= 0.01f) {
            float cp = std::cos(c.camPitch);
            float sp = std::sin(c.camPitch);
            // Frente na direção da mira da câmera (3D)
            float fx = std::sin(c.camYaw) * cp;
            float fy = sp;
            float fz = -std::cos(c.camYaw) * cp;
            // Direita (strafe lateral horizontal)
            float rx = std::cos(c.camYaw);
            float rz = std::sin(c.camYaw);

            vx = fx * my + rx * mx;
            vy = fy * my;
            vz = fz * my + rz * mx;
            float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (vlen > 1e-4f) {
                vx = (vx / vlen) * spd;
                vy = (vy / vlen) * spd;
                vz = (vz / vlen) * spd;
            }
        }

        // Subir e descer direto:
        if (c.flags & InputJump) vy += 9.5f;
        if (c.flags & InputFlyDown) vy -= 9.5f;

        p.x += vx * c.dt;
        p.y += vy * c.dt;
        p.z += vz * c.dt;

        p.x = std::max(-lim, std::min(lim, p.x));
        p.z = std::max(-lim, std::min(lim, p.z));

        float g = supportHeightAt(p.x, p.z, p.y);
        if (p.y < g) p.y = g;
        if (p.y > 350.0f) p.y = 350.0f;

        float hlen = std::sqrt(vx * vx + vz * vz);
        p.speed = hlen;
        p.vy = 0.0f;
        if (hlen > 0.05f) {
            float diff = wrapPi(std::atan2(vx, -vz) - p.yaw);
            float maxTurn = kTurnRate * c.dt;
            p.yaw = wrapPi(p.yaw + std::max(-maxTurn, std::min(maxTurn, diff)));
        }
        return;
    }

    const float g0 = supportHeightAt(p.x, p.z, p.y);
    const bool wasGrounded = p.y <= g0 + 0.03f && p.vy <= 0.0f;
    const bool inWater0 = isSwimmingPos(p.x, p.y, p.z);

    // --- horizontal ---
    float mx = c.moveX, my = c.moveY;
    float len = std::sqrt(mx * mx + my * my);
    if (len > 1.0f) { mx /= len; my /= len; len = 1.0f; }
    const bool aimG = aimingGun(p, c) && !inWater0;
    if (len < 0.01f) {
        p.speed = 0;
        collideStructs(p.x, p.z, kPlayerRadius);   // estruturas da ilha (paliçadas...)
        shipHullPushAll(p.x, p.z, kPlayerRadius);   // o casco é sólido: o navio que anda empurra quem está parado na água
    } else {
        float maxSpd = inWater0
            ? ((c.flags & InputRun) ? kFastSwimSpeed : kSwimSpeed)
            : (aimG ? kAimWalkSpeed : ((c.flags & InputRun) ? kRunSpeed : kWalkSpeed));
        float spd = maxSpd * len * ((p.status & StSpeed) ? kSpeedPotionMult : 1.0f);
        float dx = std::sin(c.camYaw) * my + std::cos(c.camYaw) * mx;
        float dz = -std::cos(c.camYaw) * my + std::sin(c.camYaw) * mx;
        float n = std::sqrt(dx * dx + dz * dz);  // = len
        dx /= n; dz /= n;
        auto tryMove = [&](float tx, float tz) -> bool {
            tx = std::max(-lim, std::min(lim, tx));
            tz = std::max(-lim, std::min(lim, tz));
            if (W) {
                float gt = std::max(W->heightAt(tx, tz), World::kSwimFloorH);
                float ddx = tx - p.x, ddz = tz - p.z, d = std::sqrt(ddx * ddx + ddz * ddz);
                if (wasGrounded && d > 1e-5f && (gt - g0) > d * World::kMaxSlope) return false;  // barranco íngreme
                W->pushOut(tx, tz, kPlayerRadius, p.y);                                   // árvores, pedras, construções
            }
            float ty = std::max(p.y, supportHeightAt(tx, tz, p.y));
            collideStructs(tx, tz, kPlayerRadius);                                         // MELHORIAS DA ILHA: paliçada, portão, torre...
            (void)ty;
            shipHullPushAll(tx, tz, kPlayerRadius);                             // casco do navio (nadador não atravessa por baixo)
            p.x = tx; p.z = tz;
            return true;
        };
        float nx = p.x + dx * spd * c.dt, nz = p.z + dz * spd * c.dt;
        if (!tryMove(nx, nz)) { if (!tryMove(nx, p.z)) tryMove(p.x, nz); }   // desliza em paredes/margens
        if (!aimG) {
            float diff = wrapPi(std::atan2(dx, -dz) - p.yaw);
            float maxTurn = kTurnRate * c.dt;
            p.yaw = wrapPi(p.yaw + std::max(-maxTurn, std::min(maxTurn, diff)));
        }
        p.speed = spd;
    }
    if (aimG) aimFace(p, c);

    // --- vertical: gravidade, pulo, flutuação no mar e "colar" no relevo/construções ---
    const float g = supportHeightAt(p.x, p.z, p.y);
    const bool inWater = isSwimmingPos(p.x, p.y, p.z);
    if (wasGrounded) {
        if ((c.flags & InputJump) && !inWater) p.vy = kJumpVel;
        else if (p.y - g < 0.6f) { p.y = g; p.vy = 0.0f; }       // acompanha descidas/subidas suaves, escadas e pisos
    }
    if (p.vy > 0.0f || p.y > g + 0.001f) {
        p.vy -= kGravity * c.dt;
        p.y += p.vy * c.dt;
        if (p.y <= g && p.vy <= 0.0f) { p.y = g; p.vy = 0.0f; }
    }
    if (p.y < g) { p.y = g; if (p.vy < 0.0f) p.vy = 0.0f; }          // nunca abaixo do chão ou da flutuação na água

    shipTryAutoBoard(p);
    p.held = c.held < HeldCount ? c.held : (uint8_t)HeldNone;
    p.carried = c.carried & CarryMask;
    setAimState(p, c);
    uint8_t act = (c.flags >> InputActShift) & 7u;
    if (act >= ActPunch && act <= ActPunch3 && wasGrounded && !inWater) {
        uint8_t seq = ((p.action >> 3) + 1u) & 0x1Fu;
        if (seq == 0) seq = 1u;
        p.action = (uint8_t)((seq << 3) | act);
    }
}

// Tiro: distância até o relevo/água ao longo do raio (d normalizado). kind = ShotTerrain/ShotWater, ou ShotNone (nada até `range`).
// Usado pelo servidor (chumbos) e pelo cliente (ponto da mira e rastros previstos; tiro offline).
inline float gunTraceWorld(float ox, float oy, float oz, float dx, float dy, float dz, float range, uint8_t& kind) {
    kind = ShotNone;
    const Terrain* W = terrainPtr();
    auto ground = [&](float x, float z) { return W ? W->heightAt(x, z) : 0.0f; };
    auto below = [&](float t) { float x = ox + dx * t, y = oy + dy * t, z = oz + dz * t; return y < std::max(ground(x, z), 0.0f); };
    auto solid = [&](float t) { return W && W->blocksShot(ox + dx * t, oy + dy * t, oz + dz * t); };
    if (below(0.0f)) { kind = oy < 0.0f ? ShotWater : ShotTerrain; return 0.0f; }
    float prev = 0.0f;
    for (float t = 0.2f; t <= range; t += 0.2f) {
        if (solid(t)) {   // árvore, pedra, tronco ou ruína no caminho: a bala para
            float lo = prev, hi = t;
            for (int i = 0; i < 6; ++i) { float m = (lo + hi) * 0.5f; if (solid(m)) hi = m; else lo = m; }
            kind = ShotTerrain;
            return hi;
        }
        if (below(t)) {
            float lo = prev, hi = t;
            for (int i = 0; i < 10; ++i) { float m = (lo + hi) * 0.5f; if (below(m)) hi = m; else lo = m; }
            kind = ground(ox + dx * hi, oz + dz * hi) < 0.0f ? ShotWater : ShotTerrain;
            return hi;
        }
        prev = t;
    }
    return range;
}

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : b_(buf), cap_(cap) {}
    template <class T> void put(T v) {
        if (n_ + sizeof(T) > cap_) { ok_ = false; return; }
        std::memcpy(b_ + n_, &v, sizeof(T));
        n_ += sizeof(T);
    }
    void header(uint8_t type) { put<uint32_t>(kProtocolMagic); put<uint8_t>(type); }
    void state(const PlayerState& s) {
        put(s.id); put(s.x); put(s.y); put(s.z); put(s.yaw); put(s.speed); put(s.vy); put(s.action); put(s.held); put(s.carried); put(s.status); put(s.link); put(s.ship); put(s.shipIdx); put(s.sa); put(s.sb); put(s.aim); put(s.aimPitch); put(s.skin);
    }
    void cmd(const InputCmd& c) {
        put(c.seq); put(c.moveX); put(c.moveY); put(c.camYaw); put(c.camPitch); put(c.dt); put(c.flags); put(c.held); put(c.carried); put(c.shipEv); put(c.aim);
    }
    // OUTRO jogador no snapshot: compacto e relativo à MINHA posição (ox, oz) — cm em i16 (alcance ±327 m > kInterestRadius), yaw em u16.
    static int16_t q16(float v, float k) { float q = std::round(v * k); return (int16_t)(q < -32767.0f ? -32767.0f : (q > 32767.0f ? 32767.0f : q)); }
    // i16 em cm (±327,66 m); fora disso manda o marcador -32768 seguido do float exato (voo alto, vizinho ligado longe): nunca satura.
    void q16x(float v, float k) {
        const float q = std::round(v * k);
        if (q >= -32767.0f && q <= 32767.0f) put<int16_t>((int16_t)q); else { put<int16_t>(INT16_MIN); put<float>(v); }
    }
    void stateRel(const PlayerState& s, float ox, float oz) {
        put<uint16_t>(s.id); q16x(s.x - ox, 100.0f); q16x(s.y, 100.0f); q16x(s.z - oz, 100.0f);
        float yw = std::fmod(s.yaw, 6.2831853f); if (yw < 0) yw += 6.2831853f;
        put<uint16_t>((uint16_t)((uint32_t)(yw * (65536.0f / 6.2831853f)) & 0xFFFFu));
        // velocidade: u8 em 0,1 m/s até 25,4; 255 = marcador + float (voo rápido do GOD, poção, navio rápido)
        { const float q = std::round(std::max(0.0f, s.speed) * 10.0f); if (q < 255.0f) put<uint8_t>((uint8_t)q); else { put<uint8_t>(255); put<float>(s.speed); } }
        q16x(s.vy, 100.0f);
        put(s.action); put(s.held); put(s.carried); put(s.status); put<uint16_t>(s.link); put(s.ship); put(s.shipIdx);
        if (s.ship) { q16x(s.sa, 100.0f); q16x(s.sb, 100.0f); }
        put(s.aim); put(s.aimPitch); put(s.skin);
    }
    void ship(const ShipState& S) {
        put<uint8_t>(S.exists ? 1 : 0); put<uint8_t>(S.gen); put(S.x); put(S.z); put(S.yaw); put(S.speed); put(S.steer); put(S.cmdSteer);
        put<uint16_t>(S.helmer < 0 ? kNoPlayer : (uint16_t)S.helmer); put<int8_t>(S.cmdThrottle);
        put<int16_t>(S.hp); put(S.sinkT);
        put<uint16_t>(S.owner < 0 ? kNoPlayer : (uint16_t)S.owner);
    }
    void chest(int ship, const Chest& c) {
        put<uint8_t>((uint8_t)ship); put<uint16_t>(c.ver);
        for (const auto& s : c.s) { put<uint8_t>(s.id); put<uint16_t>(s.n); }
    }
    void inv(uint32_t rev, int32_t coins, const InvSlot* s, uint8_t ships = 0) {
        put<uint32_t>(rev); put<int32_t>(coins); put<uint8_t>(ships);
        for (int i = 0; i < kInvSlots; ++i) { put<uint8_t>(s[i].id); put<uint16_t>(s[i].n); }
    }
    void bytes(const void* p, size_t n) { for (size_t i = 0; i < n; ++i) put<uint8_t>(((const uint8_t*)p)[i]); }
    size_t size() const { return n_; }
    bool ok() const { return ok_; }
private:
    uint8_t* b_; size_t cap_, n_ = 0; bool ok_ = true;
};

// MsgJoin completo (preenchido até kJoinPacketSize): buf precisa de >= kJoinPacketSize bytes. cookie 0 = ainda sem desafio.
inline size_t writeJoin(uint8_t* buf, uint8_t skin, uint32_t cookie) {
    std::memset(buf, 0, (size_t)kJoinPacketSize);
    Writer w(buf, (size_t)kJoinPacketSize);
    w.header(MsgJoin); w.put<uint8_t>(skin); w.put<uint32_t>(cookie);
    return (size_t)kJoinPacketSize;
}

class Reader {
public:
    Reader(const uint8_t* buf, size_t len) : b_(buf), len_(len) {}
    template <class T> T get() {
        T v{};
        if (n_ + sizeof(T) > len_) { ok_ = false; return v; }
        std::memcpy(&v, b_ + n_, sizeof(T));
        n_ += sizeof(T);
        return v;
    }
    // Lê e valida o cabeçalho; devolve o tipo ou 0 se inválido.
    uint8_t header() {
        if (get<uint32_t>() != kProtocolMagic) return 0;
        uint8_t t = get<uint8_t>();
        return ok_ ? t : 0;
    }
    PlayerState state() {
        PlayerState s;
        s.id = get<uint16_t>(); s.x = get<float>(); s.y = get<float>(); s.z = get<float>();
        s.yaw = get<float>(); s.speed = get<float>(); s.vy = get<float>(); s.action = get<uint8_t>(); s.held = get<uint8_t>(); s.carried = get<uint8_t>(); s.status = get<uint8_t>(); s.link = get<uint16_t>();
        s.ship = get<uint8_t>(); s.shipIdx = get<uint8_t>(); s.sa = get<float>(); s.sb = get<float>();
        s.aim = get<uint8_t>(); s.aimPitch = get<int8_t>(); s.skin = get<uint8_t>(); if (s.skin >= kSkinCount) s.skin = 0;
        return s;
    }
    float g16x(float k) { const int16_t v = get<int16_t>(); return v == INT16_MIN ? get<float>() : v * k; }   // par do Writer::q16x
    PlayerState stateRel(float ox, float oz) {
        PlayerState s;
        s.id = get<uint16_t>(); s.x = ox + g16x(0.01f); s.y = g16x(0.01f); s.z = oz + g16x(0.01f);
        s.yaw = get<uint16_t>() * (6.2831853f / 65536.0f); if (s.yaw > 3.14159265f) s.yaw -= 6.2831853f;
        { const uint8_t sp = get<uint8_t>(); s.speed = sp == 255 ? get<float>() : sp * 0.1f; }
        s.vy = g16x(0.01f);
        s.action = get<uint8_t>(); s.held = get<uint8_t>(); s.carried = get<uint8_t>(); s.status = get<uint8_t>(); s.link = get<uint16_t>();
        s.ship = get<uint8_t>(); s.shipIdx = get<uint8_t>();
        if (s.ship) { s.sa = g16x(0.01f); s.sb = g16x(0.01f); }
        s.aim = get<uint8_t>(); s.aimPitch = get<int8_t>(); s.skin = get<uint8_t>(); if (s.skin >= kSkinCount) s.skin = 0;
        return s;
    }
    InputCmd cmd() {
        InputCmd c;
        c.seq = get<uint32_t>(); c.moveX = get<float>(); c.moveY = get<float>();
        c.camYaw = get<float>(); c.camPitch = get<float>(); c.dt = get<float>(); c.flags = get<uint8_t>(); c.held = get<uint8_t>(); c.carried = get<uint8_t>(); c.shipEv = get<uint8_t>();
        c.aim = get<uint8_t>();
        return c;
    }
    ShipState ship() {
        ShipState S;
        S.exists = get<uint8_t>() != 0; S.gen = get<uint8_t>(); S.x = get<float>(); S.z = get<float>(); S.yaw = get<float>(); S.speed = get<float>();
        S.steer = get<float>(); S.cmdSteer = get<float>();
        uint16_t h = get<uint16_t>(); S.helmer = h == kNoPlayer ? -1 : (int)h; S.cmdThrottle = get<int8_t>();
        S.hp = get<int16_t>(); S.sinkT = get<float>();
        { uint16_t o = get<uint16_t>(); S.owner = o == kNoPlayer ? -1 : (int)o; }
        if (!std::isfinite(S.sinkT) || S.sinkT < 0.0f) S.sinkT = 0.0f;
        return S;
    }
    // [navio u8][ver u16][slots]: devolve o navio (-1 = inválido)
    int chest(Chest& c) {
        int ship = get<uint8_t>(); c.ver = get<uint16_t>();
        for (auto& s : c.s) { s.id = get<uint8_t>(); s.n = get<uint16_t>(); if (s.id >= kItemCount || s.n > itemMaxStack(s.id)) s = {}; }
        return ok_ && ship < kMaxShips ? ship : -1;
    }
    void inv(uint32_t& rev, int32_t& coins, InvSlot* s, uint8_t* ships = nullptr) {
        rev = get<uint32_t>(); coins = get<int32_t>(); { uint8_t sh = get<uint8_t>(); if (ships) *ships = sh; }
        for (int i = 0; i < kInvSlots; ++i) { s[i].id = get<uint8_t>(); s[i].n = get<uint16_t>(); }
        invSanitize(s);
    }
    bool bytes(void* p, size_t n) { for (size_t i = 0; i < n; ++i) ((uint8_t*)p)[i] = get<uint8_t>(); return ok_; }
    size_t left() const { return len_ - n_; }
    bool ok() const { return ok_; }
private:
    const uint8_t* b_; size_t len_, n_ = 0; bool ok_ = true;
};

inline bool finite(float v) { return std::isfinite(v); }

}  // namespace e0net
