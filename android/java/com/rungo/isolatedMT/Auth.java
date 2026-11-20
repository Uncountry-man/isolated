package com.rungo.isolatedMT;

import android.app.Activity;

import java.lang.reflect.Method;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * CONTA DO JOGADOR (tela de login). Chamado pelo C++ (engine/src/auth.cpp) por JNI; resultados voltam numa fila de texto (poll()).
 *
 * Sempre compilado. O Firebase fica em AuthFirebase.java (android/java_firebase/), que o build.sh SÓ compila quando existe
 * android/google-services.json (junto com as bibliotecas do Firebase). Aqui ele é chamado por REFLEXÃO, então este arquivo compila
 * com ou sem o Firebase:
 *   - com Firebase: Google (Credential Manager + FirebaseAuth) e visitante = conta anônima do Firebase;
 *   - sem Firebase: available() = false; Google responde FAIL NAO_CONFIGURADO e o C++ cria o visitante local.
 *
 * Eventos (campos separados por \u001f):
 *   OK    provedor(google|guest) uid nome email idToken
 *   FAIL  provedor motivo(NAO_CONFIGURADO | CANCELADO | SEM_INTERNET | ERRO:texto)
 *   TOKEN idToken                       (resposta de refreshToken)
 */
public final class Auth {
    static final String SEP = "\u001f";
    private static final ConcurrentLinkedQueue<String> events = new ConcurrentLinkedQueue<>();
    private static Activity act;
    private static Class<?> fb;   // com.rungo.isolatedMT.AuthFirebase (null se o app não tem Firebase)

    private Auth() {}

    static void push(String... f) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < f.length; ++i) { if (i > 0) b.append(SEP); b.append(f[i] == null ? "" : f[i]); }
        events.add(b.toString());
    }

    /** Liga o Firebase se o app foi compilado com ele. Devolve true = Firebase disponível. */
    public static boolean init(Activity a) {
        act = a;
        fb = null;
        try {
            Class<?> c = Class.forName("com.rungo.isolatedMT.AuthFirebase");
            int id = a.getResources().getIdentifier("default_web_client_id", "string", a.getPackageName());
            if (id == 0) { android.util.Log.w("Isolated.auth", "AuthFirebase presente mas falta default_web_client_id (google-services.json sem cliente web)"); return false; }
            Method init = c.getDeclaredMethod("init", Activity.class);
            init.setAccessible(true);
            init.invoke(null, a);
            fb = c;
            return true;
        } catch (ClassNotFoundException e) {
            return false;   // app sem Firebase (normal até configurar)
        } catch (Throwable t) {
            android.util.Log.e("Isolated.auth", "Firebase não iniciou", t);
            return false;
        }
    }

    public static boolean available() { return fb != null; }

    public static String poll() { return events.poll(); }

    private static void callFb(String name) {
        try {
            Method m = fb.getDeclaredMethod(name, Activity.class);
            m.setAccessible(true);
            m.invoke(null, act);
        } catch (Throwable t) {
            android.util.Log.e("Isolated.auth", "AuthFirebase." + name, t);
            push("FAIL", name.contains("Google") ? "google" : "guest", "ERRO:" + t.getClass().getSimpleName());
        }
    }

    public static void signInGoogle() {
        if (fb == null) { push("FAIL", "google", "NAO_CONFIGURADO"); return; }
        callFb("signInGoogle");
    }

    public static void signInGuest() {
        if (fb == null) { push("FAIL", "guest", "NAO_CONFIGURADO"); return; }
        callFb("signInAnonymously");
    }

    public static void refreshToken() { if (fb != null) callFb("refreshToken"); }

    public static void signOut() { if (fb != null) callFb("signOut"); }
}
