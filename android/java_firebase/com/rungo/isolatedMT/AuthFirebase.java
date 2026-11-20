package com.rungo.isolatedMT;

import android.app.Activity;
import android.os.CancellationSignal;

import androidx.annotation.NonNull;
import androidx.credentials.ClearCredentialStateRequest;
import androidx.credentials.Credential;
import androidx.credentials.CredentialManager;
import androidx.credentials.CredentialManagerCallback;
import androidx.credentials.CustomCredential;
import androidx.credentials.GetCredentialRequest;
import androidx.credentials.GetCredentialResponse;
import androidx.credentials.exceptions.ClearCredentialException;
import androidx.credentials.exceptions.GetCredentialCancellationException;
import androidx.credentials.exceptions.GetCredentialException;
import androidx.credentials.exceptions.NoCredentialException;

import com.google.android.gms.tasks.OnCompleteListener;
import com.google.android.gms.tasks.Task;
import com.google.android.libraries.identity.googleid.GetGoogleIdOption;
import com.google.android.libraries.identity.googleid.GoogleIdTokenCredential;
import com.google.firebase.FirebaseApp;
import com.google.firebase.FirebaseNetworkException;
import com.google.firebase.auth.AuthCredential;
import com.google.firebase.auth.AuthResult;
import com.google.firebase.auth.FirebaseAuth;
import com.google.firebase.auth.FirebaseAuthUserCollisionException;
import com.google.firebase.auth.FirebaseUser;
import com.google.firebase.auth.GetTokenResult;
import com.google.firebase.auth.GoogleAuthProvider;

import java.util.concurrent.Executor;

/**
 * Login pelo FIREBASE AUTH. Só é compilado quando existe android/google-services.json (build.sh). Chamado por Auth.java (reflexão).
 *  - Google: Credential Manager (seletor de contas do Android) -> idToken do Google -> FirebaseAuth.signInWithCredential.
 *    Se o jogador já era VISITANTE (conta anônima do Firebase), a conta anônima é LIGADA à do Google (linkWithCredential): o uid
 *    continua o mesmo e nada se perde. Se aquele Google já tinha conta no jogo, entra nela.
 *  - Visitante: FirebaseAuth.signInAnonymously.
 * Resultado: Auth.push("OK", provedor, uid, nome, email, idToken) ou Auth.push("FAIL", provedor, motivo).
 */
final class AuthFirebase {
    private AuthFirebase() {}

    private static Executor main(final Activity a) {
        return new Executor() { @Override public void execute(Runnable r) { a.runOnUiThread(r); } };
    }

    static void init(Activity a) {
        if (FirebaseApp.getApps(a).isEmpty()) FirebaseApp.initializeApp(a);   // lê os recursos gerados do google-services.json
    }

    private static String reason(Exception e) {
        if (e instanceof FirebaseNetworkException) return "SEM_INTERNET";
        return "ERRO:" + (e == null ? "?" : e.getClass().getSimpleName());
    }

    /** sucesso: pega o idToken (para o servidor do jogo validar no futuro) e avisa o C++ */
    private static void finish(final String provider, final FirebaseUser u) {
        if (u == null) { Auth.push("FAIL", provider, "ERRO:sem usuario"); return; }
        u.getIdToken(false).addOnCompleteListener(new OnCompleteListener<GetTokenResult>() {
            @Override public void onComplete(@NonNull Task<GetTokenResult> t) {
                String tok = t.isSuccessful() && t.getResult() != null ? t.getResult().getToken() : "";
                Auth.push("OK", provider, u.getUid(), u.getDisplayName(), u.getEmail(), tok);
            }
        });
    }

    static void signInAnonymously(final Activity a) {
        final FirebaseAuth auth = FirebaseAuth.getInstance();
        if (auth.getCurrentUser() != null && auth.getCurrentUser().isAnonymous()) { finish("guest", auth.getCurrentUser()); return; }
        auth.signInAnonymously().addOnCompleteListener(a, new OnCompleteListener<AuthResult>() {
            @Override public void onComplete(@NonNull Task<AuthResult> t) {
                if (t.isSuccessful()) finish("guest", auth.getCurrentUser());
                else Auth.push("FAIL", "guest", reason(t.getException()));
            }
        });
    }

    static void signInGoogle(final Activity a) {
        final String webId = a.getString(a.getResources().getIdentifier("default_web_client_id", "string", a.getPackageName()));
        GetGoogleIdOption opt = new GetGoogleIdOption.Builder()
                .setFilterByAuthorizedAccounts(false)   // mostra todas as contas Google do aparelho
                .setServerClientId(webId)
                .setAutoSelectEnabled(false)
                .build();
        GetCredentialRequest req = new GetCredentialRequest.Builder().addCredentialOption(opt).build();
        CredentialManager cm = CredentialManager.create(a);
        cm.getCredentialAsync(a, req, new CancellationSignal(), main(a),
                new CredentialManagerCallback<GetCredentialResponse, GetCredentialException>() {
                    @Override public void onResult(GetCredentialResponse r) {
                        Credential c = r.getCredential();
                        if (!(c instanceof CustomCredential) || !GoogleIdTokenCredential.TYPE_GOOGLE_ID_TOKEN_CREDENTIAL.equals(c.getType())) {
                            Auth.push("FAIL", "google", "ERRO:credencial inesperada"); return;
                        }
                        GoogleIdTokenCredential g = GoogleIdTokenCredential.createFrom(((CustomCredential) c).getData());
                        withGoogle(a, GoogleAuthProvider.getCredential(g.getIdToken(), null));
                    }
                    @Override public void onError(@NonNull GetCredentialException e) {
                        if (e instanceof GetCredentialCancellationException) Auth.push("FAIL", "google", "CANCELADO");
                        else if (e instanceof NoCredentialException) Auth.push("FAIL", "google", "ERRO:nenhuma conta Google no aparelho");
                        else Auth.push("FAIL", "google", "ERRO:" + e.getClass().getSimpleName());
                    }
                });
    }

    /** visitante anônimo -> liga ao Google (mesmo uid); senão (ou se o Google já tem conta) entra direto com o Google */
    private static void withGoogle(final Activity a, final AuthCredential cred) {
        final FirebaseAuth auth = FirebaseAuth.getInstance();
        final FirebaseUser cur = auth.getCurrentUser();
        final OnCompleteListener<AuthResult> plain = new OnCompleteListener<AuthResult>() {
            @Override public void onComplete(@NonNull Task<AuthResult> t) {
                if (t.isSuccessful()) finish("google", auth.getCurrentUser());
                else Auth.push("FAIL", "google", reason(t.getException()));
            }
        };
        if (cur != null && cur.isAnonymous()) {
            cur.linkWithCredential(cred).addOnCompleteListener(a, new OnCompleteListener<AuthResult>() {
                @Override public void onComplete(@NonNull Task<AuthResult> t) {
                    if (t.isSuccessful()) finish("google", auth.getCurrentUser());
                    else if (t.getException() instanceof FirebaseAuthUserCollisionException) auth.signInWithCredential(cred).addOnCompleteListener(a, plain);
                    else Auth.push("FAIL", "google", reason(t.getException()));
                }
            });
        } else {
            auth.signInWithCredential(cred).addOnCompleteListener(a, plain);
        }
    }

    static void refreshToken(Activity a) {
        FirebaseUser u = FirebaseAuth.getInstance().getCurrentUser();
        if (u == null) return;
        u.getIdToken(true).addOnCompleteListener(new OnCompleteListener<GetTokenResult>() {
            @Override public void onComplete(@NonNull Task<GetTokenResult> t) {
                if (t.isSuccessful() && t.getResult() != null) Auth.push("TOKEN", t.getResult().getToken());
            }
        });
    }

    static void signOut(Activity a) {
        FirebaseAuth.getInstance().signOut();
        CredentialManager.create(a).clearCredentialStateAsync(new ClearCredentialStateRequest(), new CancellationSignal(), main(a),
                new CredentialManagerCallback<Void, ClearCredentialException>() {   // próxima vez o seletor de contas aparece de novo
                    @Override public void onResult(Void v) {}
                    @Override public void onError(@NonNull ClearCredentialException e) {}
                });
    }
}
