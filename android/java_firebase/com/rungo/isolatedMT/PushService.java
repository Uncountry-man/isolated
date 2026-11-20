package com.rungo.isolatedMT;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

import com.google.firebase.messaging.FirebaseMessagingService;
import com.google.firebase.messaging.RemoteMessage;

import java.util.Map;

/**
 * Recebe as mensagens do FCM. Com o app em SEGUNDO PLANO o próprio sistema mostra a notificação (mensagens "notification" do console);
 * com o app ABERTO (ou mensagens só com "data": title/body) esta classe monta a notificação. Tocar nela abre o jogo.
 */
public class PushService extends FirebaseMessagingService {
    @Override public void onNewToken(String token) {
        Push.sToken = token;
        android.util.Log.i("Isolated.push", "novo token FCM: " + token);
    }

    @Override public void onMessageReceived(RemoteMessage m) {
        String title = null, body = null;
        if (m.getNotification() != null) { title = m.getNotification().getTitle(); body = m.getNotification().getBody(); }
        Map<String, String> d = m.getData();
        if (title == null && d != null) title = d.get("title");
        if (body == null && d != null) body = d.get("body");
        if (title == null && body == null) return;
        String action = d == null ? null : d.get("action");
        if (GameActivity.isForeground()) { GameActivity.deliverPush(title, body, action); return; }   // jogo aberto: banner DENTRO do jogo (tema do jogo), sem notificação do sistema
        PushFirebase.ensureChannel(this);
        final boolean crew = "tripulacao".equals(action);
        Intent open = new Intent(this, GameActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        if (action != null) open.putExtra("action", action);   // ANTES do PendingIntent (senão a ação não vai junto)
        PendingIntent pi = PendingIntent.getActivity(this, crew ? 7702 : 0, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, crew ? PushFirebase.CHANNEL_CREW : PushFirebase.CHANNEL) : new Notification.Builder(this);
        b.setSmallIcon(R.drawable.ic_notify).setContentTitle(title == null ? "Isolated" : title).setContentText(body == null ? "" : body)
         .setStyle(new Notification.BigTextStyle().bigText(body == null ? "" : body)).setAutoCancel(true).setContentIntent(pi);
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(crew ? 7702 : (int) (System.currentTimeMillis() & 0x7FFFFFF), b.build());   // tripulação: 1 notificação que se atualiza
    }
}
