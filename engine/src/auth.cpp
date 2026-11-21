#include "auth.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Isolated.auth", __VA_ARGS__)

namespace e0 {
namespace {

struct Jni {  // anexa a thread à JVM se preciso (mesmo esquema de monetize.cpp)
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

// eventos do Java: campos separados por \x1f (nomes podem ter qualquer caractere)
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> f; std::string cur;
    for (char c : s) { if (c == '\x1f') { f.push_back(cur); cur.clear(); } else cur += c; }
    f.push_back(cur);
    return f;
}
std::string clean(const std::string& s) {   // o arquivo é "chave=valor" por linha
    std::string o; for (char c : s) if (c != '\n' && c != '\r') o += c; return o;
}

}  // namespace

void Auth::init(ANativeActivity* a, const std::string& dataDir) {
    act_ = a;
    path_ = dataDir.empty() ? std::string() : dataDir + "/account.cfg";
    load();
    if (!a) return;
    Jni j(a->vm);
    JNIEnv* env = j.env;
    if (!env) return;
    // FindClass numa thread nativa só enxerga as classes do sistema: carrega pelo ClassLoader da atividade.
    jobject act = a->clazz;
    jobject loader = env->CallObjectMethod(act, env->GetMethodID(env->GetObjectClass(act), "getClassLoader", "()Ljava/lang/ClassLoader;"));
    if (j.failed("getClassLoader") || !loader) return;
    jmethodID load = env->GetMethodID(env->GetObjectClass(loader), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("com.rungo.isolatedMT.Auth");
    jobject c = env->CallObjectMethod(loader, load, name);
    env->DeleteLocalRef(name);
    if (j.failed("loadClass Auth") || !c) { LOG("classe Auth não encontrada"); return; }
    jclass cls = (jclass)env->NewGlobalRef(c);
    jmethodID m = env->GetStaticMethodID(cls, "init", "(Landroid/app/Activity;)Z");
    if (j.failed("Auth.init id") || !m) { env->DeleteGlobalRef(cls); return; }
    cloud_ = env->CallStaticBooleanMethod(cls, m, act) == JNI_TRUE;
    if (j.failed("Auth.init")) { env->DeleteGlobalRef(cls); cloud_ = false; return; }
    cls_ = cls;
    LOG("conta: %s | Firebase %s", signedIn() ? (acc_.provider == Google ? "GOOGLE" : acc_.provider == Email ? "E-MAIL" : "VISITANTE") : "nenhuma", cloud_ ? "CONFIGURADO" : "nao configurado (visitante local, Google desativado)");
    if (cloud_ && signedIn() && acc_.cloud) refreshToken();   // sessão do Firebase continua válida: pega um token novo
}

void Auth::shutdown() {
    if (!cls_ || !act_) return;
    Jni j(act_->vm);
    if (j.env) j.env->DeleteGlobalRef((jclass)cls_);
    cls_ = nullptr;
}

void Auth::call(const char* name) {
    if (!cls_ || !act_) return;
    Jni j(act_->vm);
    JNIEnv* env = j.env;
    if (!env) return;
    jmethodID m = env->GetStaticMethodID((jclass)cls_, name, "()V");
    if (j.failed(name) || !m) return;
    env->CallStaticVoidMethod((jclass)cls_, m);
    j.failed(name);
}

void Auth::signInGoogle() { call("signInGoogle"); }   // sem Java/Firebase: o próprio Java devolve FAIL NAO_CONFIGURADO

void Auth::signInGuest() {
    if (!cloud_ || !cls_) { makeLocalGuest(); pendingGuest_ = true; return; }   // sem Firebase: visitante local na hora
    call("signInGuest");
}

void Auth::signInEmail(const std::string& email, const std::string& pass) {
    if (!cloud_ || !cls_ || !act_) { pendingFail_ = FailNotConfigured; return; }
    Jni j(act_->vm);
    JNIEnv* env = j.env;
    if (!env) return;
    jmethodID m = env->GetStaticMethodID((jclass)cls_, "signInEmail", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (j.failed("signInEmail id") || !m) { pendingFail_ = FailError; return; }
    jstring je = env->NewStringUTF(email.c_str()), jp = env->NewStringUTF(pass.c_str());
    env->CallStaticVoidMethod((jclass)cls_, m, je, jp);
    j.failed("signInEmail");
    env->DeleteLocalRef(je); env->DeleteLocalRef(jp);
    LOG("login por e-mail pedido");   // nunca registra o e-mail nem a senha
}

void Auth::refreshToken() { if (cloud_) call("refreshToken"); }

void Auth::signOut() {
    if (cloud_) call("signOut");
    acc_ = Account{}; token_.clear();
    save();
    LOG("saiu da conta");
}

void Auth::makeLocalGuest() {
    std::random_device rd;
    std::mt19937_64 rng(((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    char id[40]; std::snprintf(id, sizeof id, "local-%016llx", (unsigned long long)rng());
    acc_ = Account{}; acc_.provider = Guest; acc_.cloud = false; acc_.uid = id; acc_.name = "VISITANTE";
    save();
    LOG("visitante local criado: %s", id);
}

Auth::Result Auth::poll(Fail& fail) {
    fail = FailNone;
    if (pendingGuest_) { pendingGuest_ = false; return SignedIn; }
    if (pendingFail_ != FailNone) { fail = pendingFail_; pendingFail_ = FailNone; return Failed; }
    if (!cls_ || !act_) return Nothing;
    Jni j(act_->vm);
    JNIEnv* env = j.env;
    if (!env) return Nothing;
    static jmethodID mPoll = nullptr;
    if (!mPoll) mPoll = env->GetStaticMethodID((jclass)cls_, "poll", "()Ljava/lang/String;");
    if (j.failed("Auth.poll id") || !mPoll) return Nothing;
    jstring s = (jstring)env->CallStaticObjectMethod((jclass)cls_, mPoll);
    if (j.failed("Auth.poll") || !s) return Nothing;
    std::string ev;
    if (const char* c = env->GetStringUTFChars(s, nullptr)) { ev = c; env->ReleaseStringUTFChars(s, c); }
    env->DeleteLocalRef(s);
    // OK  \x1f provedor(google|guest) \x1f uid \x1f nome \x1f email \x1f idToken
    // FAIL\x1f provedor \x1f motivo (NAO_CONFIGURADO | CANCELADO | SEM_INTERNET | ERRO:<texto>)
    const std::vector<std::string> f = split(ev);
    if (f.size() >= 6 && f[0] == "OK") {
        if (!acc_.cloud && acc_.provider == Guest && acc_.uid.rfind("local-", 0) == 0) prevLocal_ = acc_.uid;   // era visitante LOCAL: o servidor migra os dados 1 vez
        acc_ = Account{};
        acc_.provider = f[1] == "google" ? Google : f[1] == "email" ? Email : Guest;
        acc_.cloud = true; acc_.uid = clean(f[2]); acc_.name = clean(f[3]); acc_.email = clean(f[4]);
        if (acc_.name.empty() && acc_.provider != Email) acc_.name = acc_.provider == Google ? "PIRATA" : "VISITANTE";   // e-mail sem nome: a tripulação usa "PIRATA XXXX"
        token_ = f[5];
        save();
        LOG("entrou: %s uid %s", acc_.provider == Google ? "GOOGLE" : acc_.provider == Email ? "E-MAIL" : "VISITANTE", acc_.uid.c_str());
        return SignedIn;
    }
    if (f.size() >= 3 && f[0] == "FAIL") {
        const std::string& r = f[2];
        fail = r == "NAO_CONFIGURADO" ? FailNotConfigured : r == "CANCELADO" ? FailCanceled : r == "SEM_INTERNET" ? FailNetwork
             : r == "CREDENCIAL" ? FailCredential : r == "EMAIL_INVALIDO" ? FailEmailInvalid : r == "MUITAS_TENTATIVAS" ? FailTooMany : FailError;
        LOG("falhou (%s): %s", f[1].c_str(), r.c_str());
        if (f[1] == "guest" && fail != FailCanceled) { makeLocalGuest(); return SignedIn; }   // visitante nunca fica preso: cai no local
        return Failed;
    }
    if (f.size() >= 2 && f[0] == "TOKEN") { token_ = f[1]; return Nothing; }   // refreshToken: só troca o token
    return Nothing;
}

void Auth::load() {
    acc_ = Account{};
    if (path_.empty()) return;
    FILE* fp = std::fopen(path_.c_str(), "r");
    if (!fp) return;
    char line[512];
    while (std::fgets(line, sizeof line, fp)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "provider") acc_.provider = v == "google" ? Google : v == "email" ? Email : (v == "guest" ? Guest : None);
        else if (k == "cloud") acc_.cloud = v == "1";
        else if (k == "uid") acc_.uid = v;
        else if (k == "prevlocal") prevLocal_ = v;
        else if (k == "name") acc_.name = v;
        else if (k == "email") acc_.email = v;
    }
    std::fclose(fp);
    if (acc_.uid.empty()) acc_ = Account{};
}

void Auth::save() const {
    if (path_.empty()) return;
    if (acc_.provider == None) { std::remove(path_.c_str()); return; }
    const std::string tmp = path_ + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "w");
    if (!fp) return;
    std::fprintf(fp, "provider=%s\ncloud=%d\nuid=%s\nname=%s\nemail=%s\n", acc_.provider == Google ? "google" : acc_.provider == Email ? "email" : "guest", acc_.cloud ? 1 : 0,
                 acc_.uid.c_str(), acc_.name.c_str(), acc_.email.c_str());
    if (!prevLocal_.empty()) std::fprintf(fp, "prevlocal=%s\n", prevLocal_.c_str());
    std::fclose(fp);
    std::rename(tmp.c_str(), path_.c_str());
}

}  // namespace e0
