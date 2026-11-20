// Isolated - sistema de animação esquelética (65 ossos Mixamo / Y Bot) e skinning GPU.
// Puro C++ (sem dependências de OpenGL/Android): testável no PC via tools/animtest.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vecmath.h"

namespace e0 {

constexpr int kMaxBones = 65;
constexpr int kBoneRows = kMaxBones * 3;  // 65 matrizes afins 3x4 = 195 vec4

struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct BoneTransform {
    Vec3 t{0, 0, 0};
    Quat q{0, 0, 0, 1};
};

// Índices dos clipes exportados em assets/ybot.e0a (ver tools/export_anims.py)
enum AnimClipId : int {
    ClipIdle = 0,    // parado (Locomotion Pack/idle.fbx)
    ClipWalk = 1,    // andando (Locomotion Pack/walking.fbx)
    ClipRun = 2,     // correndo (Locomotion Pack/running.fbx)
    ClipJump = 3,    // impulso de subida do pulo (Action Adventure Pack/jumping up.fbx)
    ClipFall = 4,    // no ar / caindo (Pro Longbow Pack/fall a loop.fbx)
    ClipLand = 5,    // amortecimento de pouso (Pro Longbow Pack/fall a land to standing idle 01.fbx)
    ClipPunch = 6,   // BATER 1 - soco esquerdo (Pro Longbow Pack/standing melee punch.fbx)
    ClipPunch2 = 7,  // BATER 2 - gancho direito (Right Hook.fbx)
    ClipPunch3 = 8,  // BATER 3 - direto direito (Cross Punch.fbx)
    ClipChop = 9,      // CORTAR (Pro Melee Axe Pack/standing melee attack horizontal.fbx)
    ClipMine = 10,     // MINERAR (Pro Melee Axe Pack/standing melee attack downward.fbx)
    ClipPickup = 11,   // COLETAR - pega no chão e leva a mão ao bolso/cintura ao levantar
    ClipSwimIdle = 12, // BOIAR - mantendo-se na superfície da água (Treading Water.fbx)
    ClipSwim = 13,     // NADAR - braçada contínua na água (Swimming To Edge.fbx)
    ClipToolMine = 14,   // TMINE: picareta erguida com as 2 mãos e cravada no chão (Pro Melee Axe Pack/combo attack ver. 2 f62..108)
    ClipArmIdle = 15,    // AIDLE: braço direito segurando ferramenta, parado (Pro Melee Axe Pack/standing idle) - camada 31..54
    ClipArmWalk = 16,    // AWALK: idem andando (standing walk forward) - camada 31..54 em fase com ANDAR
    ClipArmRun = 17,     // ARUN: idem correndo (standing run forward) - camada 31..54 em fase com CORRER
    ClipDeath = 18,      // MORRER: cai no chão desmaiado e fica deitado no último quadro (Pro Longbow Pack/standing death backward 01.fbx)
    ClipRescue = 19,     // RESGATE: RCP em loop (quem está salvando um jogador caído) - Administering Cpr.fbx
    ClipGetUp = 20,      // LEVANTAR: levanta do chão (quem foi salvo / renasceu) - Getting Up.fbx
    ClipArmUp = 21,      // BRACO: pose (export_anims.py): braço direito de quem CARREGA, dobrado segurando a perna (camada dos ossos 31..34)
    ClipDeathFwd = 22,   // OMBRO: pose (export_anims.py): corpo dobrado sobre o ombro direito, já no espaço de quem carrega (cabeça e braços pendurados na frente, pernas atrás)
    // BACAMARTE (Pro Rifle Pack; a arma vai presa à mão direita, ver tools/export_gun.py)
    ClipGunIdle = 23,    // GIDLE: parado com a arma pronta
    ClipGunAim = 24,     // GAIM: mirando parado (coronha no ombro)
    ClipGunWalk = 25,    // GWALK: andando para a frente com a arma (também a direção "frente" ao andar mirando)
    ClipGunBack = 26,    // GBACK: andando para trás mirando
    ClipGunLeft = 27,    // GLEFT: andando de lado (esquerda) mirando
    ClipGunRight = 28,   // GRIGHT: andando de lado (direita) mirando
    ClipGunRun = 29,     // GRUN: correndo com a arma
    ClipGunReload = 30,  // GRELOAD: recarregando (camada do tronco, ossos 1..54)
    ClipViewIdle0 = 31,  // VIDLE1..6 (31..36): idles calmos da tela PERSONAGEM (Breathing Idle, Standing Idle, 03, 04, Unarmed Looking, 02 Looking)
    ClipViewIdleN = 6,
    ClipCount = 37
};

// ESQUELETO PRÓPRIO de um personagem (assets/pirate_<m|f>.e0k, tools/export_rig.py): as juntas ficam no corpo DELE (não no do Y Bot) e a malha fica
// na pose em que foi modelada (pose A). As orientações dos ossos seguem as do Y Bot, então as rotações locais dos clipes valem sem conversão; muda só a
// translação local de cada osso (comprimento) e a do quadril é reescalada pela altura do quadril: raiz = rootRest + hipScale * (t_clipe - ybotRoot).
// attach[b]: leva um ponto do espaço de repouso do Y Bot (onde estão as ferramentas, a pegada, a TAG e as poses de carregar) ao espaço local do osso b do
// personagem (= invBind do Y Bot + ajuste: pegada na mão menor, costas mais grossas, topo da cabeça).
struct Skeleton {
    bool valid = false;
    Mat4 invBind[kMaxBones];
    Vec3 t[kMaxBones];
    Vec3 rootRest{0, 0, 0}, ybotRoot{0, 0, 0};
    float hipScale = 1.0f;
    Mat4 attach[kMaxBones];
    bool loadFromMemory(const uint8_t* data, size_t len);
};

class AnimBank {
public:
    struct Clip {
        char name[16] = {};
        float fps = 30.0f;
        float duration = 0.0f;
        uint32_t numFrames = 0;
        bool loop = true;
        std::vector<BoneTransform> frames;  // numFrames * numBones
    };

    bool loadFromMemory(const uint8_t* data, size_t len);
    bool loadFromFile(const char* path);
    bool valid() const { return numBones_ == kMaxBones && !clips_.empty(); }

    int numBones() const { return numBones_; }
    int numClips() const { return (int)clips_.size(); }
    const Clip& clip(int idx) const { return clips_[idx]; }
    float clipDuration(int idx) const { return (idx >= 0 && idx < (int)clips_.size()) ? clips_[idx].duration : 0.0f; }
    static const char* clipLabel(int idx);

    // Amostra um clipe no tempo `timeSec` (em segundos) ou na fase normalizada `phase01` [0,1).
    void sampleTime(int clipIdx, float timeSec, BoneTransform* outPose) const;
    void samplePhase(int clipIdx, float phase01, BoneTransform* outPose) const;
    void restPose(BoneTransform* outPose) const;

    // Mescla duas poses locais (t = 0 -> a, t = 1 -> b).
    static void blendPose(const BoneTransform* a, const BoneTransform* b, float t, BoneTransform* out);
    // Mescla apenas a parte superior do corpo (ossos 1..54: coluna, pescoço, cabeça, ombros, braços e mãos).
    static void blendUpperBody(const BoneTransform* base, const BoneTransform* upper, float t, BoneTransform* out);

    // Calcula as matrizes finais de skinning S[b] = G_pose[b] * invBind[b] e preenche as 195 linhas (3 vec4 por osso).
    // Com `sk` usa o esqueleto do personagem (comprimentos e bind dele) e, se `attachRows`, também G_pose[b] * sk->attach[b] (peças presas ao corpo).
    void computeSkinning(const BoneTransform* localPose, Vec4* outRows195, const Skeleton* sk = nullptr, Vec4* attachRows = nullptr) const;
    // IK dos braços para quem tem esqueleto próprio (braços mais curtos que os do Y Bot): a mão direita vai para onde a do Y Bot fica em relação ao
    // ombro (arma no ombro igual) e a esquerda para o mesmo ponto da arma em que a do Y Bot segura. wR/wL = 0..1. Mão mantém a rotação da animação.
    void armIK(BoneTransform* pose, const Skeleton& sk, float wR, float wL) const;
    // Esqueleto usado por quem não escolheu outro (AnimPlayer sem setSkeleton): o renderer põe o do modelo 0.
    void setDefaultSkeleton(const Skeleton* s) { defSk_ = s; }
    const Skeleton* defaultSkeleton() const { return defSk_; }
    // Preenche `outRows195` com matrizes identidade (T-pose de repouso).
    static void fillIdentityRows(Vec4* outRows195);

private:
    int numBones_ = 0;
    int8_t parents_[kMaxBones] = {};
    Mat4 invBind_[kMaxBones] = {};
    BoneTransform rest_[kMaxBones] = {};
    std::vector<Clip> clips_;
    const Skeleton* defSk_ = nullptr;
};

struct AnimInput {
    float speed = 0.0f;      // velocidade horizontal (m/s)
    float vy = 0.0f;         // velocidade vertical (m/s)
    bool grounded = true;    // pés no chão / superfície de flutuação?
    bool swimming = false;   // em água funda (boiando / nadando)?
    int triggerAction = 0;   // 0=nenhuma, 1=Bater1, 2=Cortar, 3=Minerar, 4=Coletar, 5=Bater2, 6=Bater3
    int forceClip = -1;      // -1 = AUTO (gameplay); 0..ClipCount-1 = fixa um clipe (debug)
    bool carrying = false;   // carregando alguém no ombro: braço direito dobrado segurando a perna (ClipArmUp)
    bool carried = false;    // sou o corpo carregado: pose OMBRO
    bool rescuing = false;   // fazendo RCP em um jogador caído (RESGATE em loop)
    bool dead = false;       // morto: toca MORRER uma vez e mantém o último quadro (sobrepõe tudo)
    int tool = 0;            // e0net::HeldTool (1 machadinha, 2 picareta): braço/mão de quem segura a ferramenta e golpes próprios
    // BACAMARTE (tool == e0net::HeldGun): locomoção com a arma, mira, recarga e coice
    bool aiming = false;     // mirando: coronha no ombro, corpo de frente para a mira, anda em 4 direções
    float aimPitch = 0.0f;   // rad (+ = olhando para cima): inclina o tronco para o cano seguir a mira
    float moveAngle = 0.0f;  // direção do movimento em relação ao corpo (0 = frente, +pi/2 = direita, pi = trás)
    bool reloading = false;  // recarregando (GRELOAD no tronco)
    bool fired = false;      // EVENTO: disparou neste frame (coice no tronco/braços)
};

// Controlador de animação por personagem (mantém fase da caminhada/corrida, transições de pulo/pouso e ação).
class AnimPlayer {
public:
    static constexpr float kGetUpEnd = 2.05f;   // s: fim útil do clipe LEVANTAR (2,73 s no arquivo; o resto é só ficar parado)
    void reset();
    void update(float dt, const AnimInput& in, const AnimBank& bank, bool computeMatrices = true);

    const Vec4* boneRows() const { return rows_; }
    // Linhas para o que vai PRESO ao corpo (ferramenta na mão, itens guardados, TAG, corpo carregado no ombro): espaço de repouso do Y Bot -> pose.
    const Vec4* attachRows() const { return sk_ ? attach_ : rows_; }
    void setSkeleton(const Skeleton* s) { skSet_ = s; }
    bool gettingUp() const { return gettingUp_; }   // levantando do chão (LEVANTAR): o jogo trava o movimento enquanto dura
    const char* activeName() const { return activeName_; }
    float activePhase() const { return activePhase_; }
    bool inAction() const { return actClip_ >= 0 && !actFadeOut_; }
    float locoPhase() const { return locoPhase_; }     // fase do ciclo andar/correr (0..1, 2 passos por ciclo)
    float swimPhase() const { return swimPhase_; }     // fase da braçada (0..1)
    bool hasQueuedAction() const { return queuedClip_ >= 0; }
    // Clipe de ação que COMEÇOU a tocar desde a última chamada (-1 = nenhum): o jogo usa para marcar o momento do golpe.
    int popActionStart() { int c = startedClip_; startedClip_ = -1; return c; }
    const char* queuedName() const { return queuedClip_ >= 0 ? AnimBank::clipLabel(queuedClip_) : nullptr; }
    // Ferramenta deve ser desenhada? (some ao nadar: o personagem "guarda" a ferramenta enquanto está na água)
    bool toolVisible() const { return toolW_ > 0.5f; }
    float aimWeight() const { return aimW_ * gunW_; }   // 0..1: quanto a pose de mira está aplicada

private:
    void applyFist(float w, BoneTransform* pose);
    bool init_ = false;
    bool wasGrounded_ = true;
    bool jumped_ = false;
    bool landing_ = false;
    bool actFadeOut_ = false;
    float smoothSpeed_ = 0.0f;
    float idleTime_ = 0.0f;
    float locoPhase_ = 0.0f;
    float swimPhase_ = 0.0f;
    float swimWeight_ = 0.0f;
    float airTime_ = 0.0f;
    float airWeight_ = 0.0f;
    float landTime_ = 0.0f;
    float landWeight_ = 0.0f;
    int actClip_ = -1;
    int queuedClip_ = -1;
    int prevActClip_ = -1;
    float actTime_ = 0.0f;
    int startedClip_ = -1;
    float actWeight_ = 0.0f;
    float debugTime_ = 0.0f;
    int forceCur_ = -1, forcePrev_ = -1; float forcePrevT_ = 0.0f, forceFade_ = 1.0f;   // forceClip: troca com transição de 0,6 s
    float deadT_ = 0.0f;    // tempo desde que morreu (MORRER)
    BoneTransform guPose_[kMaxBones]; float guW_ = 0.0f;   // última pose do LEVANTAR, dissolvida na locomoção (0,25 s)
    float carryW_ = 0.0f;   // peso da camada do braço erguido
    bool wasDead_ = false, gettingUp_ = false; float getUpT_ = 0.0f, rescueT_ = 0.0f;   // transição morto -> de pé (LEVANTAR) e RCP
    float toolW_ = 0.0f;     // 0 = sem ferramenta, 1 = braço/mão de quem segura (transição ~0.15 s; zera ao nadar)
    float gunW_ = 0.0f, aimW_ = 0.0f, reloadW_ = 0.0f, reloadT_ = 0.0f, recoilT_ = 10.0f, pitchS_ = 0.0f, angS_ = 0.0f;   // bacamarte
    bool wasReloading_ = false;
    void gunPose(const AnimInput& in, const AnimBank& bank, BoneTransform* out);
    static void pitchSpine(BoneTransform* pose, float pitch, float yaw);
    const char* activeName_ = "PARADO";
    float activePhase_ = 0.0f;
    Vec4 rows_[kBoneRows] = {};
    Vec4 attach_[kBoneRows] = {};
    const Skeleton* skSet_ = nullptr;   // escolhido pelo jogo (skin do jogador)
    const Skeleton* sk_ = nullptr;      // em uso neste quadro (skSet_ ou o padrão do banco)
};

}  // namespace e0
