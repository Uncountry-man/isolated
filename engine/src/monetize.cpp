#include "monetize.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated.pay", __VA_ARGS__)

namespace e0 {
namespace {

// IDs REAIS do AdMob (produção, test = false). Vazios = usa os de teste do Google mesmo em produção.
// Ao publicar: preencher com os blocos criados no console do AdMob e trocar o APPLICATION_ID no AndroidManifest.xml.
constexpr const char* kRewardedUnit = "ca-app-pub-1741105850242816/7697794981";
constexpr const char* kInterstitialUnit = "ca-app-pub-1741105850242816/2605075391";

struct Jni {  // anexa a thread à JVM se preciso (mesmo esquema de wifi_lock.cpp)
    JavaVM* vm; JNIEnv* env = nullptr; bool attached = false;
    explicit Jni(JavaVM* v) : vm(v) {
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true; else env = nullptr;
        }
    }
    ~Jni() { if (attached) vm->DetachCurrentThread(); }
    bool failed(const char* where) {
        if (env && env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); LOG("exceção JNI em %s", where); return true; }
        return false;
    }
};

}  // namespace

bool Monetize::init(ANativeActivity* a, bool test, const std::string& skus) {
    if (cls_ || !a) return cls_ != nullptr;
    Jni j(a->vm);
    JNIEnv* env = j.env;
    if (!env) return false;
    // FindClass numa thread nativa só enxerga as classes do sistema: carrega pelo ClassLoader da atividade.
    jobject act = a->clazz;
    jobject loader = env->CallObjectMethod(act, env->GetMethodID(env->GetObjectClass(act), "getClassLoader", "()Ljava/lang/ClassLoader;"));
    if (j.failed("getClassLoader") || !loader) return false;
    jmethodID load = env->GetMethodID(env->GetObjectClass(loader), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.rungo.isolatedMT.Monetize");
    jobject c = env->CallObjectMethod(loader, load, name);
    env->DeleteLocalRef(name);
    if (j.failed("loadClass Monetize") || !c) { LOG("classe Monetize não encontrada (APK sem classes.dex?)"); return false; }
    jclass cls = (jclass)env->NewGlobalRef(c);
    jmethodID m = env->GetStaticMethodID(cls, "init", "(Landroid/app/Activity;ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (j.failed("init id") || !m) { env->DeleteGlobalRef(cls); return false; }
    jstring s = env->NewStringUTF(skus.c_str()), ru = env->NewStringUTF(kRewardedUnit), iu = env->NewStringUTF(kInterstitialUnit);
    env->CallStaticVoidMethod(cls, m, act, (jboolean)test, s, ru, iu);
    env->DeleteLocalRef(s); env->DeleteLocalRef(ru); env->DeleteLocalRef(iu);
    if (j.failed("Monetize.init")) { env->DeleteGlobalRef(cls); return false; }
    act_ = a; cls_ = cls;
    LOG("monetização iniciada (%s)", test ? "MODO TESTE" : "PRODUCAO");
    return true;
}

void Monetize::shutdown() {
    if (!cls_ || !act_) return;
    Jni j(act_->vm);
    if (j.env) j.env->DeleteGlobalRef((jclass)cls_);
    cls_ = nullptr;
}

bool Monetize::poll(std::vector<std::string>& events) {
    if (!cls_) return false;
    Jni j(act_->vm);
    JNIEnv* env = j.env;
    if (!env) return false;
    jclass cls = (jclass)cls_;
    static jmethodID mPoll = nullptr, mStatus = nullptr;
    if (!mPoll) { mPoll = env->GetStaticMethodID(cls, "poll", "()Ljava/lang/String;"); mStatus = env->GetStaticMethodID(cls, "status", "()I"); }
    for (int i = 0; i < 16; ++i) {
        jstring s = (jstring)env->CallStaticObjectMethod(cls, mPoll);
        if (j.failed("poll") || !s) break;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) { events.emplace_back(c); env->ReleaseStringUTFChars(s, c); }
        env->DeleteLocalRef(s);
    }
    status_ = env->CallStaticIntMethod(cls, mStatus);
    j.failed("status");
    return true;
}

static void callStatic(ANativeActivity* a, void* cls, const char* name, const char* sig, int iarg, const char* sarg) {
    if (!cls || !a) return;
    Jni j(a->vm);
    JNIEnv* env = j.env;
    if (!env) return;
    jmethodID m = env->GetStaticMethodID((jclass)cls, name, sig);
    if (j.failed(name) || !m) return;
    if (sarg) { jstring s = env->NewStringUTF(sarg); env->CallStaticVoidMethod((jclass)cls, m, s); env->DeleteLocalRef(s); }
    else if (sig[1] == 'I') env->CallStaticVoidMethod((jclass)cls, m, (jint)iarg);
    else env->CallStaticVoidMethod((jclass)cls, m);
    j.failed(name);
}

void Monetize::showRewarded(int tag) { callStatic(act_, cls_, "showRewarded", "(I)V", tag, nullptr); }
void Monetize::showInterstitial() { callStatic(act_, cls_, "showInterstitial", "()V", 0, nullptr); }
void Monetize::buy(const std::string& sku) { callStatic(act_, cls_, "buy", "(Ljava/lang/String;)V", 0, sku.c_str()); }

}  // namespace e0
