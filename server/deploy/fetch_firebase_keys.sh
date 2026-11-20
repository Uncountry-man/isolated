#!/usr/bin/env bash
# Baixa as chaves públicas (JWK) que o Google usa para assinar os idTokens do Firebase. O servidor relê o arquivo quando ele muda.
set -euo pipefail
D=/opt/isolated/data; OUT="$D/firebase_jwk.json"; TMP="$OUT.tmp"
URL="https://www.googleapis.com/service_accounts/v1/jwk/securetoken@system.gserviceaccount.com"
curl -fsS --max-time 30 --retry 3 "$URL" -o "$TMP"
grep -q '"keys"' "$TMP" && grep -q '"kid"' "$TMP" || { echo "resposta invalida"; rm -f "$TMP"; exit 1; }
mv -f "$TMP" "$OUT"
echo "chaves do Firebase atualizadas: $(grep -c '"kid"' "$OUT") chave(s)"
