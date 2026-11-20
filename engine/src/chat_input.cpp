#include "chat_input.h"

#include <android/native_activity.h>
#include <jni.h>

namespace e0 {
namespace {
struct Jni {  // anexa a thread do jogo à JVM só durante a chamada
    JavaVM* vm = nullptr; JNIEnv* env = nullptr; bool attached = false;
    explicit Jni(ANativeActivity* a) {
        if (!a || !a->vm) return;
        vm = a->vm;
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true; else env = nullptr;
        }
    }
    ~Jni() {
        if (env && env->ExceptionCheck()) env->ExceptionClear();
        if (attached) vm->DetachCurrentThread();
    }
};
}  // namespace

bool chatInputOpen(ANativeActivity* a, const char* hint) {
    Jni j(a);
    if (!j.env) return false;
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "showChatInput", "(Ljava/lang/String;)V");
    if (!m) { env->ExceptionClear(); return false; }
    env->CallStaticVoidMethod(c, m, env->NewStringUTF(hint ? hint : ""));
    return !env->ExceptionCheck();
}

bool chatInputPoll(ANativeActivity* a, std::string& out) {
    Jni j(a);
    if (!j.env) { out = "\x01"; return true; }
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "takeInputResult", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); out = "\x01"; return true; }
    jstring r = (jstring)env->CallStaticObjectMethod(c, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); out = "\x01"; return true; }
    if (!r) return false;
    const char* s = env->GetStringUTFChars(r, nullptr);
    out = s ? s : "";
    if (s) env->ReleaseStringUTFChars(r, s);
    return true;
}

bool shareText(ANativeActivity* a, const std::string& utf8) {
    Jni j(a);
    if (!j.env) return false;
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "shareText", "(Ljava/lang/String;)V");
    if (!m) { env->ExceptionClear(); return false; }
    env->CallStaticVoidMethod(c, m, env->NewStringUTF(utf8.c_str()));
    return !env->ExceptionCheck();
}

std::string pushToken(ANativeActivity* a) {
    Jni j(a);
    if (!j.env) return "";
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "pushToken", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); return ""; }
    jstring r = (jstring)env->CallStaticObjectMethod(c, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    if (!r) return "";
    const char* s = env->GetStringUTFChars(r, nullptr);
    std::string out = s ? s : "";
    if (s) env->ReleaseStringUTFChars(r, s);
    return out;
}

bool pushPoll(ANativeActivity* a, std::string& out) {
    Jni j(a);
    if (!j.env) return false;
    JNIEnv* env = j.env;
    jclass c = env->GetObjectClass(a->clazz);
    jmethodID m = env->GetStaticMethodID(c, "takePush", "()Ljava/lang/String;");
    if (!m) { env->ExceptionClear(); return false; }
    jstring r = (jstring)env->CallStaticObjectMethod(c, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!r) return false;
    const char* s = env->GetStringUTFChars(r, nullptr);
    out = s ? s : "";
    if (s) env->ReleaseStringUTFChars(r, s);
    return true;
}
}  // namespace e0
