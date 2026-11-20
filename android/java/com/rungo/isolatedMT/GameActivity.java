package com.rungo.isolatedMT;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.inputmethod.InputMethodManager;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import java.text.Normalizer;
import android.text.method.DigitsKeyListener;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;

/** Atividade do jogo: a NativeActivity de sempre (o jogo é todo C++, libisolated.so).
 *  Tem código Java próprio para AdMob e Google Play Billing (ver Monetize) e dois ajudantes do MENU PRINCIPAL chamados por JNI
 *  (engine/src/main_menu.cpp): caixa de texto para o endereço do servidor e a folha de compartilhamento para convidar amigos. */
public class GameActivity extends NativeActivity {
    private static volatile GameActivity sInst;
    /** Resultado da caixa de texto: null = ainda aberta / nada novo; "\u0001" = cancelada; senão o texto digitado. */
    private static volatile String sInput;
    /** MENSAGENS PUSH DENTRO DO JOGO: com o app aberto o PushService não cria notificação do sistema, enfileira aqui "título\u001fcorpo\u001fação" e o C++
     *  (engine/src/chat_input.cpp: pushPoll) mostra o banner do tema do jogo. Título e corpo vazios + ação = só executar a ação (toque numa notificação do sistema). */
    private static final java.util.concurrent.ConcurrentLinkedQueue<String> sPush = new java.util.concurrent.ConcurrentLinkedQueue<String>();
    private static volatile boolean sResumed;
    public static boolean isForeground() { return sResumed; }
    public static void deliverPush(String title, String body, String action) {
        if (sPush.size() < 8) sPush.add((title == null ? "" : title) + "\u001f" + (body == null ? "" : body) + "\u001f" + (action == null ? "" : action));
    }
    public static String takePush() { return sPush.poll(); }
    /** Token FCM deste aparelho ("" se ainda não chegou): o jogo manda ao servidor para as notificações do chat da TRIPULAÇÃO. */
    public static String pushToken() { return Push.token(); }
    private void pushIntent(Intent i) {
        if (i == null) return;
        String act = i.getStringExtra("action");   // dados da mensagem FCM chegam como extras da intent ao tocar na notificação
        if (act != null && !act.isEmpty()) { deliverPush("", "", act); i.removeExtra("action"); }
    }
    @Override protected void onNewIntent(Intent i) { super.onNewIntent(i); pushIntent(i); }
    @Override protected void onResume() { super.onResume(); sResumed = true; }
    @Override protected void onPause() { sResumed = false; super.onPause(); }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        sInst = this;
        pushIntent(getIntent());
        Push.start(this);   // notificações push (FCM); só faz algo se o app tem Firebase
    }

    /** Esconde as barras do sistema (tela cheia imersiva). Chamado pelo C++ (thread do jogo) via JNI: View/Window só podem ser
     *  mexidas na THREAD DE UI, então tudo vai por runOnUiThread (antes era feito direto da thread GL). */
    public void hideSystemBarsUi() {
        runOnUiThread(new Runnable() { @Override public void run() { applyImmersive(); } });
    }

    @SuppressWarnings("deprecation")
    private void applyImmersive() {
        final android.view.View dv = getWindow().getDecorView();
        // STABLE | HIDE_NAV | FULLSCREEN | LAYOUT_HIDE_NAV | LAYOUT_FULLSCREEN | IMMERSIVE_STICKY
        dv.setSystemUiVisibility(0x1706);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            android.view.WindowInsetsController c = dv.getWindowInsetsController();
            if (c != null) {
                c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(android.view.WindowInsets.Type.systemBars());
            }
        }
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersive();   // já na thread de UI
    }

    @Override protected void onDestroy() {
        sInst = null;
        Monetize.shutdown();
        super.onDestroy();
    }

    /** Abre uma caixa de texto (teclado numérico com ponto) com o texto inicial. O resultado sai em takeInputResult(). */
    public static void showTextInput(final String title, final String initial, final String hint) {
        final GameActivity a = sInst;
        if (a == null) { sInput = "\u0001"; return; }
        sInput = null;
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                final EditText et = new EditText(a);
                et.setKeyListener(DigitsKeyListener.getInstance("0123456789.:"));
                et.setRawInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
                et.setSingleLine(true);
                et.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE);   // paisagem: não cobrir a caixa com o teclado grande
                et.setHint(title + " - " + hint);   // sem título no diálogo: em paisagem o teclado come o espaço
                et.setText(initial);
                et.setSelection(et.getText().length());
                AlertDialog d = new AlertDialog.Builder(a)
                    .setView(et)
                    .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                        @Override public void onClick(DialogInterface di, int w) { sInput = et.getText().toString().trim(); }
                    })
                    .setNegativeButton("Cancelar", new DialogInterface.OnClickListener() {
                        @Override public void onClick(DialogInterface di, int w) { sInput = "\u0001"; }
                    })
                    .setOnCancelListener(new DialogInterface.OnCancelListener() {
                        @Override public void onCancel(DialogInterface di) { sInput = "\u0001"; }
                    })
                    .create();
                d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN);
                d.show();
                et.requestFocus();
            }
        });
    }

    /** CHAT: caixa de texto escura CENTRALIZADA na área livre acima do teclado do sistema (forçado). Janela em tela cheia, translúcida, com
     *  SOFT_INPUT_ADJUST_RESIZE: ao abrir o teclado a janela encolhe e a caixa fica no centro do que sobrou. Sem botões: a tecla de envio do
     *  teclado manda; tocar fora ou VOLTAR cancela. O resultado (letras com acento incluídas, no máx. 80) sai em takeInputResult(); "\u0001" = cancelado. */
    public static void showChatInput(final String hint) {
        final GameActivity a = sInst;
        if (a == null) { sInput = "\u0001"; return; }
        sInput = null;
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                final EditText et = new EditText(a);
                et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
                et.setSingleLine(true);
                et.setFilters(new InputFilter[] { new InputFilter.LengthFilter(80) });
                et.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_SEND | EditorInfo.IME_FLAG_NO_FULLSCREEN);
                et.setHint(hint);
                et.setHintTextColor(Color.argb(150, 255, 255, 255));
                et.setTextColor(Color.WHITE);
                et.setTextSize(20f);
                et.setPadding(36, 28, 36, 28);
                et.setBackgroundColor(Color.rgb(12, 12, 12));
                final android.app.Dialog d = new android.app.Dialog(a, android.R.style.Theme_Translucent_NoTitleBar);
                final android.widget.FrameLayout root = new android.widget.FrameLayout(a);
                root.setBackgroundColor(Color.argb(110, 0, 0, 0));
                android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(
                    (int) (a.getResources().getDisplayMetrics().widthPixels * 0.62f), android.view.ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
                root.addView(et, lp);
                root.setOnClickListener(new android.view.View.OnClickListener() {
                    @Override public void onClick(android.view.View v) { sInput = "\u0001"; d.dismiss(); }
                });
                d.setContentView(root);
                // centraliza na área livre acima do teclado: o teclado (IME) entra como inset e vira folga embaixo da raiz
                if (android.os.Build.VERSION.SDK_INT >= 30) d.getWindow().setDecorFitsSystemWindows(false);
                root.setOnApplyWindowInsetsListener(new android.view.View.OnApplyWindowInsetsListener() {
                    @Override public android.view.WindowInsets onApplyWindowInsets(android.view.View v, android.view.WindowInsets ins) {
                        int bottom = 0;
                        if (android.os.Build.VERSION.SDK_INT >= 30) bottom = ins.getInsets(android.view.WindowInsets.Type.ime()).bottom;
                        else bottom = ins.getSystemWindowInsetBottom();
                        v.setPadding(0, 0, 0, bottom);
                        return ins;
                    }
                });
                d.setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override public void onCancel(DialogInterface di) { sInput = "\u0001"; }
                });
                et.setOnEditorActionListener(new android.widget.TextView.OnEditorActionListener() {
                    @Override public boolean onEditorAction(android.widget.TextView v, int actionId, android.view.KeyEvent e) {
                        sInput = cleanChat(et.getText().toString());
                        d.dismiss();
                        return true;
                    }
                });
                d.getWindow().setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT);
                d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
                d.show();
                root.requestApplyInsets();
                et.requestFocus();
                // força o teclado do sistema (algumas versões do Android não o abrem sozinhas num diálogo sobre a NativeActivity)
                final InputMethodManager imm = (InputMethodManager) a.getSystemService(Activity.INPUT_METHOD_SERVICE);
                et.post(new Runnable() { @Override public void run() { if (imm != null) imm.showSoftInput(et, InputMethodManager.SHOW_FORCED); } });
                et.postDelayed(new Runnable() { @Override public void run() { if (imm != null && d.isShowing()) imm.showSoftInput(et, InputMethodManager.SHOW_FORCED); } }, 250);
            }
        });
    }

    /** Compõe os acentos (NFC), deixa só ASCII imprimível e letras latinas acentuadas (U+00C0..U+00FF sem × e ÷) e limita a 80 caracteres. */
    private static String cleanChat(String s) {
        String n = Normalizer.normalize(s, Normalizer.Form.NFC);
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < n.length() && b.length() < 80; i++) {
            char c = n.charAt(i);
            if ((c >= 32 && c <= 126) || (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7)) b.append(c);
        }
        return b.toString().trim();
    }

    /** Devolve (e consome) o resultado da caixa de texto; null se ainda não há. */
    public static String takeInputResult() {
        String r = sInput;
        if (r != null) sInput = null;
        return r;
    }

    /** Folha de compartilhamento do Android com um texto (convidar amigos). */
    public static void shareText(final String text) {
        final Activity a = sInst;
        if (a == null) return;
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                Intent i = new Intent(Intent.ACTION_SEND);
                i.setType("text/plain");
                i.putExtra(Intent.EXTRA_TEXT, text);
                a.startActivity(Intent.createChooser(i, "Convidar amigos"));
            }
        });
    }
}
