#include "wifi_lock.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated.net", __VA_ARGS__)

namespace e0 {
namespace {

jobject g_lock = nullptr;  // referência global ao WifiManager.WifiLock

struct Jni {  // anexa a thread à JVM se preciso
    JavaVM* vm; JNIEnv* env = nullptr; bool attached = false;
    explicit Jni(JavaVM* v) : vm(v) {
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true; else env = nullptr;
        }
    }
    ~Jni() { if (attached) vm->DetachCurrentThread(); }
    bool failed(const char* where) {  // true (e limpa) se houve exceção Java
        if (env->ExceptionCheck()) { env->ExceptionClear(); LOG("wifi lock: exceção JNI em %s", where); return true; }
        return false;
    }
};

}  // namespace

bool wifiLowLatencyAcquire(ANativeActivity* a) {
    if (!a || g_lock) return g_lock != nullptr;
    Jni j(a->vm);
    JNIEnv* env = j.env;
    if (!env) return false;
    jobject ctx = a->clazz;
    jmethodID getSS = env->GetMethodID(env->GetObjectClass(ctx), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject wm = env->CallObjectMethod(ctx, getSS, env->NewStringUTF("wifi"));
    if (j.failed("getSystemService") || !wm) return false;
    jclass wmc = env->GetObjectClass(wm);
    jmethodID create = env->GetMethodID(wmc, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    jobject lock = env->CallObjectMethod(wm, create, 4 /*WIFI_MODE_FULL_LOW_LATENCY*/, env->NewStringUTF("isolated-lowlatency"));
    if (j.failed("createWifiLock") || !lock) return false;
    jclass lc = env->GetObjectClass(lock);
    env->CallVoidMethod(lock, env->GetMethodID(lc, "setReferenceCounted", "(Z)V"), JNI_FALSE);
    env->CallVoidMethod(lock, env->GetMethodID(lc, "acquire", "()V"));
    if (j.failed("acquire")) return false;
    bool held = env->CallBooleanMethod(lock, env->GetMethodID(lc, "isHeld", "()Z"));
    if (held) g_lock = env->NewGlobalRef(lock);
    LOG("Wi-Fi low-latency lock: %s", held ? "ATIVO" : "não ativou");
    return held;
}

void wifiLowLatencyRelease(ANativeActivity* a) {
    if (!a || !g_lock) return;
    Jni j(a->vm);
    if (!j.env) return;
    jclass lc = j.env->GetObjectClass(g_lock);
    j.env->CallVoidMethod(g_lock, j.env->GetMethodID(lc, "release", "()V"));
    j.failed("release");
    j.env->DeleteGlobalRef(g_lock);
    g_lock = nullptr;
}

void hideSystemBars(ANativeActivity* a) {
    // A View/Window só pode ser mexida na thread de UI do Android: delega a GameActivity.hideSystemBarsUi (runOnUiThread).
    if (!a) return;
    Jni j(a->vm);
    JNIEnv* env = j.env;
    if (!env) return;
    jobject act = a->clazz;
    jclass ac = env->GetObjectClass(act);
    jmethodID m = env->GetMethodID(ac, "hideSystemBarsUi", "()V");
    if (j.failed("hideSystemBarsUi (metodo)") || !m) return;
    env->CallVoidMethod(act, m);
    j.failed("hideSystemBarsUi");
}

}  // namespace e0
