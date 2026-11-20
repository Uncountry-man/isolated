# Publicar o Isolated na Play Store (chave real + AAB)

- **Nome do app (hoje):** Isolated Multiplayer (`android:label` no `AndroidManifest.xml`)
- **Pacote:** `com.rungo.isolatedMT`

> O `build.sh` hoje só assina com a chave de debug (linha do `apksigner`) e não gera AAB.
> Também não há `versionCode`/`versionName` no manifest.

## 1. Criar a chave de produção (uma vez só)
```bash
keytool -genkeypair -v -keystore ~/isolated-release.jks -alias isolated \
  -keyalg RSA -keysize 4096 -validity 10000
```
- Guarde o `.jks` e as senhas em pelo menos 2 lugares fora do PC (pendrive, gerenciador de senhas). Perdeu a chave de upload = só recupera pedindo reset à Play.
- Nunca coloque o `.jks` no projeto nem em git.

## 2. Usar o Play App Signing
Na Play Console, ao criar o app, deixe o **Play App Signing** ativado (padrão). A chave acima vira a **chave de upload**; a Google guarda a chave final de distribuição e você só envia AABs assinados com a de upload.

## 3. Preparar o app para release
- **Versão:** acrescentar `android:versionCode="1"` e `android:versionName="1.0"` no `<manifest>`. Cada upload novo precisa de `versionCode` maior.
- **AdMob:** trocar o App ID de teste (`ca-app-pub-3940256099942544~3347511713`) no `AndroidManifest.xml` pelo real e preencher `kRewardedUnit` / `kInterstitialUnit` em `engine/src/monetize.cpp`.
- **Build de produção:** `E0_ADS_TEST=0` (sem isso continuam anúncios de teste e compra simulada).
- **Servidor:** `E0_SERVER_HOST=<ip ou domínio público>` (o IP da LAN não serve para quem baixar da loja).
- **Consentimento (UMP/GDPR):** a biblioteca já vem no app, falta chamá-la.

## 4. Gerar o APK assinado com a chave real
No `build.sh`, trocar a linha do `apksigner` por:
```bash
"$BT/apksigner" sign --ks ~/isolated-release.jks --ks-alias isolated \
  --out $OUT/isolated.apk $OUT/aligned.apk
```
A senha é pedida no terminal (ou use `--ks-pass env:KS_PASS`).

## 5. Gerar o AAB (sem Gradle)
1. `aapt2 link --proto-format` para gerar o APK base em formato proto.
2. Reorganizar em `base.zip`: `manifest/`, `dex/`, `lib/`, `assets/`, `res/`.
3. `java -jar bundletool-all.jar build-bundle --modules=base.zip --output=isolated.aab`
4. Assinar: `jarsigner -keystore ~/isolated-release.jks isolated.aab isolated` (AAB usa `jarsigner`, não `apksigner`).

- **Falta instalar o bundletool:** baixe `bundletool-all.jar` em https://github.com/google/bundletool/releases
- Teste antes de subir: `bundletool build-apks` + `install-apks`.

## 6. Subir na Play Console
1. Criar o app → Testes → **Teste interno**.
2. Subir o `.aab`.
3. Cadastrar os produtos consumíveis: `coins_500`, `coins_2500`, `coins_10000`, `coins_50000`.
4. Preencher classificação de conteúdo, **política de privacidade** (obrigatória: AdMob + login) e Segurança dos dados.
5. Se usar Firebase: adicionar o SHA-1 da chave de upload e o da "assinatura de apps" (Play Console → Integridade do app) no console do Firebase.

## Pendências
- [ ] Criar a chave `.jks` e guardar backup
- [ ] `versionCode`/`versionName` no manifest
- [ ] App ID e blocos reais do AdMob
- [ ] Servidor público (IP/domínio)
- [ ] Baixar o bundletool e automatizar o AAB no `build.sh` (`E0_RELEASE=1`)
- [ ] Política de privacidade (URL pública)
- [ ] Chamar o consentimento UMP
