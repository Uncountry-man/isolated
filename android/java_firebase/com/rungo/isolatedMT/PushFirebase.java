package com.rungo.isolatedMT;

import android.Manifest;
import android.app.Activity;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

import androidx.annotation.NonNull;

import com.google.android.gms.tasks.OnCompleteListener;
import com.google.android.gms.tasks.Task;
import com.google.firebase.FirebaseApp;
import com.google.firebase.messaging.FirebaseMessaging;
import com.google.firebase.inappmessaging.FirebaseInAppMessaging;
import com.google.firebase.inappmessaging.FirebaseInAppMessagingDisplay;
import com.google.firebase.inappmessaging.FirebaseInAppMessagingDisplayCallbacks;
import com.google.firebase.inappmessaging.model.Action;
import com.google.firebase.inappmessaging.model.BannerMessage;
import com.google.firebase.inappmessaging.model.CardMessage;
import com.google.firebase.inappmessaging.model.InAppMessage;
import com.google.firebase.inappmessaging.model.ModalMessage;
import com.google.firebase.inappmessaging.model.Text;
import com.google.firebase.installations.FirebaseInstallations;

/**
 * Firebase Cloud Messaging: canal de notificação, permissão (Android 13+), inscrição no tópico "geral" e token do aparelho (log "Isolated.push").
 * Enviar: console do Firebase > Engajamento > Messaging > Nova campanha (para o tópico "geral", ou "usuários do app"); o token serve para teste individual.
 */
final class PushFirebase {
    static final String CHANNEL = "geral";
    static final String TOPIC = "geral";
    static final String CHANNEL_CREW = "tripulacao";
    private PushFirebase() {}

    static void ensureChannel(Context c) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        if (nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "Avisos do Isolated", NotificationManager.IMPORTANCE_DEFAULT);
            ch.setDescription("Novidades, eventos e avisos do jogo");
            nm.createNotificationChannel(ch);
        }
        if (nm.getNotificationChannel(CHANNEL_CREW) == null) {   // mensagens do chat da TRIPULAÇÃO (o servidor manda com channel_id "tripulacao")
            NotificationChannel ch = new NotificationChannel(CHANNEL_CREW, "Tripula\u00e7\u00e3o", NotificationManager.IMPORTANCE_HIGH);
            ch.setDescription("Mensagens do chat da sua tripula\u00e7\u00e3o");
            nm.createNotificationChannel(ch);
        }
    }

    private static String txt(Text t) { return t == null || t.getText() == null ? "" : t.getText(); }
    private static String act(Action a) {   // "isolated://loja" ou só "loja" (campo "abrir URL" do console); qualquer outra URL é ignorada
        if (a == null || a.getActionUrl() == null) return "";
        String u = a.getActionUrl().trim();
        int i = u.indexOf("://");
        if (i >= 0) { if (!u.startsWith("isolated")) return ""; u = u.substring(i + 3); }
        while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
        return u;
    }

    /** FIREBASE IN-APP MESSAGING com exibição PRÓPRIA: o SDK escolhe a campanha (console > In-App Messaging) e entrega aqui; quem desenha é o JOGO
     *  (banner do tema, GameActivity.deliverPush -> engine_push.inc). Banner/cartão/modal viram o mesmo banner; só-imagem é ignorada. */
    static void startInApp() {
        FirebaseInAppMessaging.getInstance().setMessageDisplayComponent(new FirebaseInAppMessagingDisplay() {
            @Override public void displayMessage(InAppMessage m, FirebaseInAppMessagingDisplayCallbacks cb) {
                String title = "", body = "", action = "";
                if (m instanceof BannerMessage) { BannerMessage b = (BannerMessage) m; title = txt(b.getTitle()); body = txt(b.getBody()); action = act(b.getAction()); }
                else if (m instanceof CardMessage) { CardMessage c = (CardMessage) m; title = txt(c.getTitle()); body = txt(c.getBody()); action = act(c.getPrimaryAction()); }
                else if (m instanceof ModalMessage) { ModalMessage c = (ModalMessage) m; title = txt(c.getTitle()); body = txt(c.getBody()); action = act(c.getAction()); }
                android.util.Log.i("Isolated.push", "in-app recebida: '" + title + "' / '" + body + "' acao='" + action + "'");
                if (title.isEmpty() && body.isEmpty()) { cb.messageDismissed(FirebaseInAppMessagingDisplayCallbacks.InAppMessagingDismissType.UNKNOWN_DISMISS_TYPE); return; }
                GameActivity.deliverPush(title, body, action);
                cb.impressionDetected();
                cb.messageDismissed(FirebaseInAppMessagingDisplayCallbacks.InAppMessagingDismissType.AUTO);
            }
        });
        FirebaseInstallations.getInstance().getId().addOnCompleteListener(new OnCompleteListener<String>() {
            @Override public void onComplete(@NonNull Task<String> t) {
                if (t.isSuccessful()) android.util.Log.i("Isolated.push", "ID de instalacao (FID): " + t.getResult());
            }
        });
    }

    static void start(final Activity a) {
        if (FirebaseApp.getApps(a).isEmpty()) FirebaseApp.initializeApp(a);
        ensureChannel(a);
        if (Build.VERSION.SDK_INT >= 33 && a.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            a.requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 7701);   // aparece 1x; se negar, o Android lembra
        }
        startInApp();
        FirebaseMessaging fm = FirebaseMessaging.getInstance();
        fm.subscribeToTopic(TOPIC);
        fm.getToken().addOnCompleteListener(new OnCompleteListener<String>() {
            @Override public void onComplete(@NonNull Task<String> t) {
                if (t.isSuccessful()) { Push.sToken = t.getResult(); android.util.Log.i("Isolated.push", "token FCM: " + t.getResult()); }
                else android.util.Log.w("Isolated.push", "sem token FCM", t.getException());
            }
        });
    }
}
