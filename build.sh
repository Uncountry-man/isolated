#!/usr/bin/env bash
# Compila, empacota, assina, instala e abre o jogo no aparelho via adb.
# Uso: ./build.sh [--no-run]
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="$(ls -d "$SDK"/ndk/* | sort -V | tail -1)"
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
# targetSdk/compileSdk: a Play exige >= 36 (2026). Precisa de ~/Android/Sdk/platforms/android-$TARGET_SDK.
TARGET_SDK="${E0_TARGET_SDK:-36}"
JAR="$SDK/platforms/android-$TARGET_SDK/android.jar"
[ -f "$JAR" ] || { echo "Falta $JAR (instale a platform android-$TARGET_SDK no SDK)"; exit 1; }
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
OBJCOPY="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objcopy"
CMAKE="$(ls -d "$SDK"/cmake/* | sort -V | tail -1)/bin"
ADB="$SDK/platform-tools/adb"
PKG=com.rungo.isolatedMT
OUT=build; ABI=arm64-v8a

# IP do servidor: E0_SERVER_HOST=... ./build.sh  (padrão: IP LAN desta máquina; "offline" desativa)
SERVER_HOST="${E0_SERVER_HOST:-$(hostname -I 2>/dev/null | awk '{print $1}')}"
[ "$SERVER_HOST" = "offline" ] && SERVER_HOST=""
# Modo cinema: E0_CINEMA=1 ./build.sh. App separado (não sobrescreve o jogo), sempre offline: o tempo do mundo precisa seguir o roteiro.
CINEMA=OFF; RENAME=()
if [ "${E0_CINEMA:-0}" = "1" ]; then CINEMA=ON; SERVER_HOST=""; PKG=com.rungo.isolatedMT.cinema; RENAME=(--rename-manifest-package "$PKG"); echo "MODO CINEMA: $PKG"; fi
echo "Servidor de jogo: ${SERVER_HOST:-<offline>}"
# RELEASE: E0_RELEASE=1 KS_PASS=... ./build.sh  -> APK assinado com a chave real + build/isolated.aab (ver PUBLICAR_NA_PLAY.md).
#   Chave: E0_KEYSTORE (padrão ~/isolated-release.jks), alias E0_KEY_ALIAS (padrão isolated), senha na variavel KS_PASS.
#   E0_VERSION_CODE (padrão 1; sobe a cada upload na Play) e E0_VERSION_NAME (padrão 1.0). Release implica --no-run.
RELEASE="${E0_RELEASE:-0}"
# Painel de debug + cheats (aba GOD, teleportes, hora, navio, voo): no build normal; FORA do binário em release (E0_DEBUG_UI=0). Pode forçar com E0_DEBUG_UI=0/1.
DEBUG_UI="${E0_DEBUG_UI:-}"; [ -z "$DEBUG_UI" ] && { [ "$RELEASE" = "1" ] && [ "$CINEMA" = "OFF" ] && DEBUG_UI=0 || DEBUG_UI=1; }
[ "$DEBUG_UI" = "1" ] && DEBUG_UI_CM=ON || DEBUG_UI_CM=OFF
echo "Painel de debug/cheats: $DEBUG_UI_CM"
VCODE="${E0_VERSION_CODE:-1}"; VNAME="${E0_VERSION_NAME:-1.0}"
KEYSTORE="${E0_KEYSTORE:-$HOME/isolated-release.jks}"; KEYALIAS="${E0_KEY_ALIAS:-isolated}"
if [ "$RELEASE" = "1" ]; then
  [ -f "$KEYSTORE" ] || { echo "Chave não encontrada: $KEYSTORE"; exit 1; }
  [ -n "${KS_PASS:-}" ] || { echo "Defina a senha: read -s -p 'Senha: ' KS_PASS; export KS_PASS"; exit 1; }
  [ "${E0_ADS_TEST:-1}" = "1" ] && echo "AVISO: release com anúncios/compras de TESTE (use E0_ADS_TEST=0 na versão final)"
fi

# 1. código nativo: só arm64-v8a (celulares reais). Emulador x86_64: E0_ABIS="arm64-v8a x86_64" ./build.sh
ABIS="${E0_ABIS:-arm64-v8a}"
for A in $ABIS; do
  BDIR="$OUT/native-$A"
  [ "$A" = "arm64-v8a" ] && BDIR="$OUT/native"
  "$CMAKE/cmake" -S . -B "$BDIR" -G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$A" -DANDROID_PLATFORM=android-26 -DANDROID_NDK="$NDK" -DCMAKE_BUILD_TYPE=Release -DE0_SERVER_HOST="$SERVER_HOST" -DE0_CINEMA=$CINEMA -DE0_ADS_TEST=${E0_ADS_TEST:-1} -DE0_DEBUG_UI=$DEBUG_UI_CM >/dev/null
  "$CMAKE/cmake" --build "$BDIR"
done

# 2. Java (AdMob + Google Play Billing; ver CLAUDE.md "Monetização"). Sem Gradle: tools/android_deps.py baixa os AARs,
#    tools/merge_manifest.py mescla os manifests, aapt2 gera os R.java das bibliotecas, javac + d8 geram os .dex.
DEPS_LIST="com.google.android.gms:play-services-ads:25.5.0 com.android.billingclient:billing:9.1.0"
# FIREBASE (login com Google / visitante na nuvem; ver CLAUDE.md "Login"): liga sozinho quando existe android/google-services.json.
# O JSON vira res/values (tools/google_services.py, no lugar do plugin do Gradle) e entra android/java_firebase/ + as bibliotecas abaixo.
FIREBASE=0
rm -rf $OUT/fbres
if [ -f android/google-services.json ]; then
  if python3 tools/google_services.py android/google-services.json $PKG $OUT/fbres/values/firebase.xml; then
    FIREBASE=1
    DEPS_LIST="$DEPS_LIST com.google.firebase:firebase-auth:23.1.0 androidx.credentials:credentials:1.3.0 androidx.credentials:credentials-play-services-auth:1.3.0 com.google.android.libraries.identity.googleid:googleid:1.1.1 com.google.firebase:firebase-messaging:24.1.0 com.google.firebase:firebase-inappmessaging:21.0.0 com.google.firebase:firebase-analytics:22.1.2"
  fi
fi
echo "Firebase: $([ $FIREBASE = 1 ] && echo LIGADO || echo 'desligado (sem android/google-services.json)')"
DEPS=$OUT/deps
if [ ! -f $DEPS/resolved.txt ] || [ "$(cat $DEPS/roots.txt 2>/dev/null)" != "$DEPS_LIST" ]; then
  rm -rf $DEPS/jars $DEPS/res $DEPS/manifests $OUT/libres $OUT/dexdeps
  python3 tools/android_deps.py $DEPS $DEPS_LIST && echo "$DEPS_LIST" > $DEPS/roots.txt
fi
JAVA="${JAVA_HOME:-$HOME/.jdks/current}/bin"
mkdir -p $OUT/libres
LIBRES=()
for d in $DEPS/res/*/; do
  n=$(basename "$d")
  [ -f $OUT/libres/$n.zip ] || "$BT/aapt2" compile --dir "$d" -o $OUT/libres/$n.zip
  LIBRES+=(-R $OUT/libres/$n.zip)
done
python3 tools/merge_manifest.py android/AndroidManifest.xml $PKG $OUT/AndroidManifest.xml $DEPS/manifests/*.xml
EXTRA=$(awk '{print $2}' $DEPS/packages.txt | sort -u | paste -sd: -)

# 3. APK (recursos + manifest)
rm -rf $OUT/apk $OUT/gen $OUT/classes $OUT/dexapp $OUT/symbols && mkdir -p $OUT/apk/lib/$ABI $OUT/gen $OUT/classes $OUT/dexapp
"$BT/aapt2" compile --dir android/res -o $OUT/res.zip
FBRES=()
if [ $FIREBASE = 1 ]; then "$BT/aapt2" compile --dir $OUT/fbres -o $OUT/fbres.zip; FBRES=(-R $OUT/fbres.zip); fi
"$BT/aapt2" link -o $OUT/base.apk -I "$JAR" --manifest $OUT/AndroidManifest.xml \
  --min-sdk-version 26 --target-sdk-version "$TARGET_SDK" --version-code "$VCODE" --version-name "$VNAME" -A assets -0 ogg "${LIBRES[@]}" -R $OUT/res.zip "${FBRES[@]}" --auto-add-overlay \
  --java $OUT/gen --extra-packages "$EXTRA" --proguard $OUT/aapt_rules.pro "${RENAME[@]}"
if [ "$RELEASE" = "1" ]; then   # mesmo link em formato proto (base do AAB)
  "$BT/aapt2" link --proto-format -o $OUT/base_proto.apk -I "$JAR" --manifest $OUT/AndroidManifest.xml \
    --min-sdk-version 26 --target-sdk-version "$TARGET_SDK" --version-code "$VCODE" --version-name "$VNAME" -A assets "${LIBRES[@]}" -R $OUT/res.zip "${FBRES[@]}" --auto-add-overlay \
    --extra-packages "$EXTRA" "${RENAME[@]}"
fi

# 4. código Java -> dex (as bibliotecas só são convertidas uma vez: build/dexdeps)
DEPJARS=$(ls $DEPS/jars/*.jar | paste -sd: -)
JSRC=(android/java $OUT/gen); [ $FIREBASE = 1 ] && JSRC+=(android/java_firebase)
find "${JSRC[@]}" -name '*.java' > $OUT/javafiles.txt
"$JAVA/javac" -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$JAR:$BT/core-lambda-stubs.jar" -cp "$DEPJARS" -d $OUT/classes @$OUT/javafiles.txt
# R8 (padrão): app + bibliotecas encolhidos juntos (tira classes/métodos não usados das bibliotecas do AdMob/Billing).
#   Regras: as que as bibliotecas trazem (tools/collect_proguard.py) + as do aapt2 (manifest/recursos) + android/proguard-app.pro.
#   E0_R8=0 ./build.sh volta ao d8 sem encolher (bibliotecas convertidas 1 vez em build/dexdeps).
if [ "${E0_R8:-1}" = "1" ]; then
  python3 tools/collect_proguard.py $DEPS $OUT/deps_rules.pro >/dev/null
  rm -rf $OUT/r8out && mkdir -p $OUT/r8out
  rm -f $OUT/appclasses.jar; (cd $OUT/classes && zip -q -r ../appclasses.jar .)
  "$JAVA/java" -Xmx3g -cp "$BT/lib/d8.jar" com.android.tools.r8.R8 --release --min-api 26 --lib "$JAR" \
    --pg-conf android/proguard-app.pro --pg-conf $OUT/deps_rules.pro --pg-conf $OUT/aapt_rules.pro \
    --pg-map-output $OUT/r8map.txt --output $OUT/r8out $OUT/appclasses.jar $DEPS/jars/*.jar > $OUT/r8.log 2>&1 \
    || { tail -40 $OUT/r8.log; echo "R8 falhou (log em $OUT/r8.log); use E0_R8=0 ./build.sh"; exit 1; }
  i=1; for f in $(ls $OUT/r8out/classes*.dex | sort -V); do if [ $i = 1 ]; then cp "$f" $OUT/apk/classes.dex; else cp "$f" $OUT/apk/classes$i.dex; fi; i=$((i+1)); done
else
  if [ ! -f $OUT/dexdeps/classes.dex ]; then
    mkdir -p $OUT/dexdeps
    echo "d8: convertendo as bibliotecas Java (só na 1a vez)..."
    "$BT/d8" --release --min-api 26 --lib "$JAR" --output $OUT/dexdeps $DEPS/jars/*.jar > $OUT/d8deps.log 2>&1 || { cat $OUT/d8deps.log; exit 1; }
  fi
  "$BT/d8" --release --min-api 26 --lib "$JAR" --classpath $DEPS/jars --output $OUT/dexapp $(find $OUT/classes -name '*.class')
  cp $OUT/dexapp/classes.dex $OUT/apk/classes.dex
  i=2; for f in $(ls $OUT/dexdeps/classes*.dex | sort -V); do cp "$f" $OUT/apk/classes$i.dex; i=$((i+1)); done
fi
# recursos Java das bibliotecas (META-INF/services do Kotlin coroutines etc.)
mkdir -p $OUT/apk/META-INF
for j in $DEPS/jars/*.jar; do
  unzip -qo "$j" 'META-INF/services/*' -d $OUT/apk 2>/dev/null || true
done

for A in $ABIS; do
  BDIR="$OUT/native-$A"
  [ "$A" = "arm64-v8a" ] && BDIR="$OUT/native"
  mkdir -p "$OUT/apk/lib/$A" "$OUT/symbols/$A"
  # o .so do CMake sai COM símbolos (linhas de código): no APK vai a cópia sem nada; os símbolos ficam em build/symbols/<abi>/
  # (libisolated.so = completo, p/ o native-debug-symbols.zip; libisolated.so.dbg = só a depuração, vai no AAB)
  "$STRIP" --strip-all -o "$OUT/apk/lib/$A/libisolated.so" "$BDIR/libisolated.so"
  cp "$BDIR/libisolated.so" "$OUT/symbols/$A/libisolated.so"
  "$OBJCOPY" --only-keep-debug "$BDIR/libisolated.so" "$OUT/symbols/$A/libisolated.so.dbg"
done
cp $OUT/base.apk $OUT/unaligned.apk
(cd $OUT/apk && zip -q -r ../unaligned.apk lib classes*.dex META-INF)
rm -f $OUT/isolated.apk
"$BT/zipalign" -f -P 16 4 $OUT/unaligned.apk $OUT/aligned.apk
if [ "$RELEASE" = "1" ]; then
  "$BT/apksigner" sign --ks "$KEYSTORE" --ks-key-alias "$KEYALIAS" --ks-pass env:KS_PASS --out $OUT/isolated.apk $OUT/aligned.apk
  echo "APK (CHAVE REAL): $OUT/isolated.apk"
  # AAB: módulo base = APK proto + dex/ + lib/ (manifest vai para manifest/), bundletool monta, jarsigner assina.
  rm -rf $OUT/aab && mkdir -p $OUT/aab/base && (cd $OUT/aab/base && unzip -q ../../base_proto.apk \
    && mkdir manifest dex && mv AndroidManifest.xml manifest/ \
    && cp ../../apk/classes*.dex dex/ && cp -r ../../apk/lib lib \
    && mkdir -p root && cp -r ../../apk/META-INF root/META-INF && zip -q -r -X ../base.zip .)
  echo '{"compression":{"uncompressedGlob":["assets/**/*.e0i","assets/**/*.e0m","assets/**/*.e0a"]}}' > $OUT/aab/BundleConfig.json
  rm -f $OUT/isolated.aab
  # metadados que a Play lê do AAB: mapa do R8 (desofuscação) e símbolos nativos (BUNDLE-METADATA/...)
  META=()
  [ "${E0_R8:-1}" = "1" ] && META+=("--metadata-file=com.android.tools.build.obfuscation/proguard.map:$OUT/r8map.txt")
  for A in $ABIS; do META+=("--metadata-file=com.android.tools.build.debugsymbols/$A/libisolated.so.dbg:$OUT/symbols/$A/libisolated.so.dbg"); done
  "$JAVA/java" -Djava.io.tmpdir="${TMPDIR:-/tmp}" -jar tools/bin/bundletool.jar build-bundle --modules=$OUT/aab/base.zip --config=$OUT/aab/BundleConfig.json "${META[@]}" --output=$OUT/isolated.aab
  "$JAVA/jarsigner" -keystore "$KEYSTORE" -storepass:env KS_PASS -sigalg SHA256withRSA -digestalg SHA-256 $OUT/isolated.aab "$KEYALIAS" >/dev/null
  echo "AAB (CHAVE REAL): $OUT/isolated.aab  (versionCode $VCODE, versionName $VNAME, targetSdk $TARGET_SDK)"
  # cópias para releases/ (o mapa e os símbolos já estão dentro do AAB; os arquivos soltos servem p/ upload manual na Play Console)
  REL=releases/isolated-$VNAME-code$VCODE; mkdir -p releases
  cp $OUT/isolated.aab $REL.aab; cp $OUT/isolated.apk $REL.apk
  [ "${E0_R8:-1}" = "1" ] && cp $OUT/r8map.txt $REL-mapping.txt
  rm -f $REL-native-debug-symbols.zip
  (cd $OUT/symbols && zip -q -r "$OLDPWD/$REL-native-debug-symbols.zip" $(for A in $ABIS; do echo $A/libisolated.so; done))
  echo "Copiados: $REL.aab, $REL.apk, $REL-mapping.txt, $REL-native-debug-symbols.zip"
  exit 0
fi
"$BT/apksigner" sign --ks ~/.android/debug.keystore --ks-pass pass:android --out $OUT/isolated.apk $OUT/aligned.apk
echo "APK: $OUT/isolated.apk"

# 5. instalar e abrir
[ "${1:-}" = "--no-run" ] && exit 0
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
  ADB_ARGS=(-s "$ADB_SERIAL")
elif "$ADB" devices | grep -q "127.0.0.1:6555[[:space:]]*device"; then
  ADB_ARGS=(-s "127.0.0.1:6555")
fi
"$ADB" "${ADB_ARGS[@]}" install -r $OUT/isolated.apk
"$ADB" "${ADB_ARGS[@]}" shell am force-stop $PKG
"$ADB" "${ADB_ARGS[@]}" shell am start -n $PKG/com.rungo.isolatedMT.GameActivity
