package com.rungo.isolatedMT;

import android.app.Activity;
import android.app.AlertDialog;
import android.util.Log;

import com.android.billingclient.api.AcknowledgePurchaseParams;
import com.android.billingclient.api.BillingClient;
import com.android.billingclient.api.BillingClientStateListener;
import com.android.billingclient.api.BillingFlowParams;
import com.android.billingclient.api.BillingResult;
import com.android.billingclient.api.ConsumeParams;
import com.android.billingclient.api.PendingPurchasesParams;
import com.android.billingclient.api.ProductDetails;
import com.android.billingclient.api.Purchase;
import com.android.billingclient.api.QueryProductDetailsParams;
import com.android.billingclient.api.QueryPurchasesParams;
import com.google.android.gms.ads.AdError;
import com.google.android.gms.ads.AdRequest;
import com.google.android.gms.ads.FullScreenContentCallback;
import com.google.android.gms.ads.LoadAdError;
import com.google.android.gms.ads.MobileAds;
import com.google.android.gms.ads.interstitial.InterstitialAd;
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback;
import com.google.android.gms.ads.rewarded.RewardedAd;
import com.google.android.gms.ads.rewarded.RewardedAdLoadCallback;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Ponte AdMob + Google Play Billing para o C++ (engine/src/monetize.cpp chama estes métodos estáticos por JNI).
 * Tudo que chega da Google vira um EVENTO de texto numa fila ("TIPO|a|b"), que o jogo lê com poll() ~4x/s:
 *   REWARD|tag|qtd       anúncio premiado assistido até o fim (tag = onde foi pedido: 1 loja, 2 renascer)
 *   AD_CLOSED|tag        anúncio fechado (premiado ou intersticial; tag 0 = intersticial)
 *   AD_FAIL|tag|motivo   não havia anúncio pronto / falhou ao mostrar
 *   BUY_OK|sku|teste|purchaseToken|orderId   compra concluída (teste = 1 se foi a compra SIMULADA do modo teste; token/pedido vazios no teste)
 *   BUY_CANCEL|sku  BUY_FAIL|sku|motivo  BUY_PENDING|sku
 *   PRICE|sku|preço      preço formatado vindo da Play (ex.: "R$ 4,90")
 * MODO TESTE: blocos de anúncio de TESTE do Google (sempre exibem anúncio de teste, sem risco à conta AdMob).
 * Na loja, se o produto não existir na Play (app ainda não publicado num canal de teste com os produtos criados),
 * a compra é SIMULADA por uma janela "COMPRA DE TESTE" (nenhuma cobrança). Em produção (test = false) não há simulação.
 */
public final class Monetize {
    private static final String TAG = "Isolated.pay";
    // IDs de TESTE oficiais do Google AdMob (https://developers.google.com/admob/android/test-ads)
    private static final String TEST_REWARDED = "ca-app-pub-3940256099942544/5224354917";
    private static final String TEST_INTERSTITIAL = "ca-app-pub-3940256099942544/1033173712";

    // Aparelhos de DESENVOLVIMENTO: sempre recebem anúncio de teste, mesmo no build de produção (evita invalid traffic / suspensão).
    // ID que o próprio SDK imprime no logcat (tag Ads: "setTestDeviceIds(...)"). moto g75 ZY22LX8FHV.
    private static final String[] TEST_DEVICES = { "FF6454CE4A1B1DE0C7AB36927F49E7CF" };

    private static Activity act;
    private static boolean test = true;
    private static String rewardedId = TEST_REWARDED, interstitialId = TEST_INTERSTITIAL;
    private static final ConcurrentLinkedQueue<String> events = new ConcurrentLinkedQueue<>();

    private static volatile RewardedAd rewarded;
    private static volatile InterstitialAd interstitial;
    private static volatile boolean loadingRewarded, loadingInterstitial, showing, adsReady;
    private static int rewardTag;
    private static boolean rewardEarned;

    private static BillingClient billing;
    private static volatile boolean billingReady;
    private static final Set<String> consumables = new HashSet<>(), products = new HashSet<>();
    private static final Map<String, ProductDetails> details = Collections.synchronizedMap(new HashMap<>());
    private static final Set<String> handled = Collections.synchronizedSet(new HashSet<>());

    private static void ev(String s) { events.add(s); Log.i(TAG, "evento " + s); }

    /** Chamado uma vez pelo C++ (thread do jogo). skus = "id:c,id:c,..." (c = 1 consumível, 0 permanente). */
    public static void init(final Activity a, boolean testMode, String skus, String rewardedUnit, String interstitialUnit) {
        if (act != null) return;
        act = a; test = testMode;
        if (rewardedUnit != null && !rewardedUnit.isEmpty() && !testMode) rewardedId = rewardedUnit;
        if (interstitialUnit != null && !interstitialUnit.isEmpty() && !testMode) interstitialId = interstitialUnit;
        for (String s : skus.split(",")) {
            String[] p = s.split(":");
            if (p[0].isEmpty()) continue;
            products.add(p[0]);
            if (p.length > 1 && p[1].equals("1")) consumables.add(p[0]);
        }
        Log.i(TAG, "init: modo " + (test ? "TESTE" : "PRODUCAO") + ", " + products.size() + " produtos");
        MobileAds.setRequestConfiguration(new com.google.android.gms.ads.RequestConfiguration.Builder()
            .setTestDeviceIds(java.util.Arrays.asList(TEST_DEVICES)).build());
        new Thread(() -> MobileAds.initialize(a, st -> {   // a inicialização é pesada: fora da thread da UI
            Log.i(TAG, "AdMob inicializado");
            adsReady = true;
            a.runOnUiThread(() -> { loadRewarded(); loadInterstitial(); });
        }), "admob-init").start();
        a.runOnUiThread(Monetize::startBilling);
    }

    public static void shutdown() {
        if (billing != null) { billing.endConnection(); billing = null; }
        act = null;
    }

    /** Próximo evento ou null. */
    public static String poll() { return events.poll(); }

    /** bit0 premiado pronto, bit1 intersticial pronto, bit2 Play Billing conectado, bit3 anúncio na tela, bit4 modo teste */
    public static int status() {
        return (rewarded != null ? 1 : 0) | (interstitial != null ? 2 : 0) | (billingReady ? 4 : 0)
             | (showing ? 8 : 0) | (test ? 16 : 0);
    }

    // ------------------------------------------------------------------ AdMob
    private static void loadRewarded() {
        if (act == null || !adsReady || rewarded != null || loadingRewarded) return;
        loadingRewarded = true;
        RewardedAd.load(act, rewardedId, new AdRequest.Builder().build(), new RewardedAdLoadCallback() {
            @Override public void onAdLoaded(RewardedAd ad) { rewarded = ad; loadingRewarded = false; Log.i(TAG, "premiado carregado"); }
            @Override public void onAdFailedToLoad(LoadAdError e) {
                loadingRewarded = false; Log.w(TAG, "premiado falhou: " + e.getMessage());
                retry(Monetize::loadRewarded);
            }
        });
    }

    private static void loadInterstitial() {
        if (act == null || !adsReady || interstitial != null || loadingInterstitial) return;
        loadingInterstitial = true;
        InterstitialAd.load(act, interstitialId, new AdRequest.Builder().build(), new InterstitialAdLoadCallback() {
            @Override public void onAdLoaded(InterstitialAd ad) { interstitial = ad; loadingInterstitial = false; Log.i(TAG, "intersticial carregado"); }
            @Override public void onAdFailedToLoad(LoadAdError e) {
                loadingInterstitial = false; Log.w(TAG, "intersticial falhou: " + e.getMessage());
                retry(Monetize::loadInterstitial);
            }
        });
    }

    private static void retry(Runnable r) {   // nova tentativa em 30 s (sem rede, sem anúncio)
        if (act != null) act.getWindow().getDecorView().postDelayed(r, 30000);
    }

    /** Mostra um anúncio PREMIADO. tag volta no evento REWARD/AD_CLOSED/AD_FAIL. */
    public static void showRewarded(final int tag) {
        final Activity a = act;
        if (a == null) return;
        a.runOnUiThread(() -> {
            final RewardedAd ad = rewarded;
            if (ad == null || showing) { ev("AD_FAIL|" + tag + "|SEM ANUNCIO PRONTO"); loadRewarded(); return; }
            rewarded = null; showing = true; rewardTag = tag; rewardEarned = false;
            ad.setFullScreenContentCallback(new FullScreenContentCallback() {
                @Override public void onAdDismissedFullScreenContent() { showing = false; ev("AD_CLOSED|" + rewardTag); loadRewarded(); }
                @Override public void onAdFailedToShowFullScreenContent(AdError e) {
                    showing = false; ev("AD_FAIL|" + rewardTag + "|" + e.getMessage()); loadRewarded();
                }
            });
            ad.show(a, item -> { if (!rewardEarned) { rewardEarned = true; ev("REWARD|" + rewardTag + "|" + item.getAmount()); } });
        });
    }

    /** Mostra um INTERSTICIAL se houver um pronto (senão não faz nada). */
    public static void showInterstitial() {
        final Activity a = act;
        if (a == null) return;
        a.runOnUiThread(() -> {
            final InterstitialAd ad = interstitial;
            if (ad == null || showing) { loadInterstitial(); return; }
            interstitial = null; showing = true;
            ad.setFullScreenContentCallback(new FullScreenContentCallback() {
                @Override public void onAdDismissedFullScreenContent() { showing = false; ev("AD_CLOSED|0"); loadInterstitial(); }
                @Override public void onAdFailedToShowFullScreenContent(AdError e) { showing = false; ev("AD_FAIL|0|" + e.getMessage()); loadInterstitial(); }
            });
            ad.show(a);
        });
    }

    // ------------------------------------------------------------------ Google Play Billing
    private static void startBilling() {
        if (act == null) return;
        billing = BillingClient.newBuilder(act)
            .setListener(Monetize::onPurchases)
            .enablePendingPurchases(PendingPurchasesParams.newBuilder().enableOneTimeProducts().build())
            .enableAutoServiceReconnection()
            .build();
        billing.startConnection(new BillingClientStateListener() {
            @Override public void onBillingSetupFinished(BillingResult r) {
                billingReady = r.getResponseCode() == BillingClient.BillingResponseCode.OK;
                Log.i(TAG, "Play Billing: " + (billingReady ? "CONECTADO" : "falhou (" + r.getResponseCode() + " " + r.getDebugMessage() + ")"));
                if (billingReady) { queryProducts(); restore(); }
            }
            @Override public void onBillingServiceDisconnected() { billingReady = false; Log.w(TAG, "Play Billing desconectado"); }
        });
    }

    private static void queryProducts() {
        List<QueryProductDetailsParams.Product> list = new ArrayList<>();
        for (String id : products)
            list.add(QueryProductDetailsParams.Product.newBuilder().setProductId(id).setProductType(BillingClient.ProductType.INAPP).build());
        if (list.isEmpty()) return;
        billing.queryProductDetailsAsync(QueryProductDetailsParams.newBuilder().setProductList(list).build(), (r, res) -> {
            for (ProductDetails d : res.getProductDetailsList()) {
                details.put(d.getProductId(), d);
                ProductDetails.OneTimePurchaseOfferDetails o = d.getOneTimePurchaseOfferDetails();
                if (o != null) ev("PRICE|" + d.getProductId() + "|" + o.getFormattedPrice());
            }
            Log.i(TAG, "produtos na Play: " + res.getProductDetailsList().size() + " encontrados, "
                + res.getUnfetchedProductList().size() + " nao cadastrados (" + r.getResponseCode() + ")");
        });
    }

    /** Compras feitas e não entregues (app fechado no meio, compra pendente que foi paga depois...). */
    private static void restore() {
        billing.queryPurchasesAsync(QueryPurchasesParams.newBuilder().setProductType(BillingClient.ProductType.INAPP).build(),
            (r, list) -> { if (r.getResponseCode() == BillingClient.BillingResponseCode.OK) for (Purchase p : list) deliver(p); });
    }

    /** Pede a compra de um produto (sku = id do produto na Play Console). */
    public static void buy(final String sku) {
        final Activity a = act;
        if (a == null) return;
        a.runOnUiThread(() -> {
            ProductDetails d = details.get(sku);
            if (d == null || billing == null || !billingReady) {
                if (test) { fakePurchase(a, sku); return; }
                ev("BUY_FAIL|" + sku + "|" + (billingReady ? "PRODUTO INDISPONIVEL" : "GOOGLE PLAY INDISPONIVEL"));
                return;
            }
            BillingFlowParams.ProductDetailsParams.Builder pp = BillingFlowParams.ProductDetailsParams.newBuilder().setProductDetails(d);
            ProductDetails.OneTimePurchaseOfferDetails o = d.getOneTimePurchaseOfferDetails();
            if (o != null && o.getOfferToken() != null) pp.setOfferToken(o.getOfferToken());
            BillingResult r = billing.launchBillingFlow(a, BillingFlowParams.newBuilder()
                .setProductDetailsParamsList(Collections.singletonList(pp.build())).build());
            if (r.getResponseCode() != BillingClient.BillingResponseCode.OK) ev("BUY_FAIL|" + sku + "|" + r.getDebugMessage());
        });
    }

    private static String skuOf(Purchase p) { List<String> s = p.getProducts(); return s.isEmpty() ? "" : s.get(0); }

    private static void onPurchases(BillingResult r, List<Purchase> list) {
        int c = r.getResponseCode();
        if (c == BillingClient.BillingResponseCode.OK && list != null) { for (Purchase p : list) deliver(p); return; }
        if (c == BillingClient.BillingResponseCode.USER_CANCELED) { ev("BUY_CANCEL|"); return; }
        if (c == BillingClient.BillingResponseCode.ITEM_ALREADY_OWNED) { restore(); return; }
        ev("BUY_FAIL||" + r.getDebugMessage());
    }

    /** Entrega ao jogo e finaliza na Play: consumível = consume (pode comprar de novo); permanente = acknowledge.
     *  Compra não finalizada em 3 dias é reembolsada pela Google. */
    private static void deliver(final Purchase p) {
        final String sku = skuOf(p);
        if (p.getPurchaseState() == Purchase.PurchaseState.PENDING) { ev("BUY_PENDING|" + sku); return; }
        if (p.getPurchaseState() != Purchase.PurchaseState.PURCHASED) return;
        boolean consumable = consumables.contains(sku);
        if (!consumable && p.isAcknowledged()) return;               // já entregue antes
        if (!handled.add(p.getPurchaseToken())) return;              // já está sendo entregue
        ev("BUY_OK|" + sku + "|0|" + p.getPurchaseToken() + "|" + (p.getOrderId() == null ? "" : p.getOrderId()));   // o C++ guarda o token (não entrega 2x) p/ validar no servidor
        if (consumable) {
            billing.consumeAsync(ConsumeParams.newBuilder().setPurchaseToken(p.getPurchaseToken()).build(),
                (r, tok) -> Log.i(TAG, "consumido " + sku + ": " + r.getResponseCode()));
        } else {
            billing.acknowledgePurchase(AcknowledgePurchaseParams.newBuilder().setPurchaseToken(p.getPurchaseToken()).build(),
                r -> Log.i(TAG, "confirmado " + sku + ": " + r.getResponseCode()));
        }
    }

    /** MODO TESTE: o produto não está cadastrado na Play (ou a Play não está disponível): simula a folha de compra. */
    private static void fakePurchase(Activity a, final String sku) {
        showing = true;
        new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("COMPRA DE TESTE")
            .setMessage("Produto: " + sku + "\n\nO produto ainda não existe na Google Play Console "
                + "(ou a Play não está disponível). Esta compra é SIMULADA: nenhuma cobrança será feita.")
            .setPositiveButton("COMPRAR (TESTE)", (dlg, w) -> ev("BUY_OK|" + sku + "|1|test-" + System.currentTimeMillis() + "|"))
            .setNegativeButton("CANCELAR", (dlg, w) -> ev("BUY_CANCEL|" + sku))
            .setOnCancelListener(dlg -> ev("BUY_CANCEL|" + sku))
            .setOnDismissListener(dlg -> showing = false)
            .show();
    }
}
