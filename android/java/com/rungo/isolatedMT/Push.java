package com.rungo.isolatedMT;

import android.app.Activity;
import java.lang.reflect.Method;

/**
 * NOTIFICAÇÕES PUSH (Firebase Cloud Messaging). Sempre compilado; a parte do Firebase está em PushFirebase.java
 * (android/java_firebase/), que o build.sh só compila quando existe android/google-services.json. Chamado por GameActivity.onCreate.
 */
public final class Push {
    private Push() {}
    /** Token do Firebase Cloud Messaging deste aparelho ("" = ainda não chegou / sem Firebase). O jogo manda ao servidor (TRIPULAÇÃO: push do chat). */
    static volatile String sToken = "";
    public static String token() { String t = sToken; return t == null ? "" : t; }

    public static void start(Activity a) {
        try {
            Class<?> c = Class.forName("com.rungo.isolatedMT.PushFirebase");
            Method m = c.getDeclaredMethod("start", Activity.class);
            m.setAccessible(true);
            m.invoke(null, a);
        } catch (ClassNotFoundException e) {
            // app sem Firebase: sem push
        } catch (Throwable t) {
            android.util.Log.e("Isolated.push", "push não iniciou", t);
        }
    }
}
