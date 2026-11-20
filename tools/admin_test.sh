#!/usr/bin/env bash
# Teste dos ADMINS (comandos de debug sem --debug): servidor SEM --debug, com --data/--jwk/--admins num diretório temporário; roda
# `nettest admin` (que reescreve/apaga o arquivo de admins durante o teste) e confere o log de auditoria. Uso: tools/admin_test.sh [porta]
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-7796}"
T="$(mktemp -d "${TMPDIR:-/tmp}/e0admin.XXXXXX")"
python3 tools/make_test_jwt.py build/jwttest isolated-multiplayer alice bob carol >/dev/null
build/server "$PORT" --flat --data "$T/data" --jwk build/jwttest/test.jwk --admins "$T/admins.txt" >"$T/server.log" 2>&1 & PID=$!
sleep 2
set +e
build/nettest "$PORT" admin build/jwttest "$T/admins.txt"; R=$?
kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
set -e
echo "--- log do servidor ($T/server.log) ---"; grep -E "\[admin\]|\[debug\]" "$T/server.log" | head -40
L=0
grep -q "\[admin\] .* fb:alice (jogador [0-9]*) MOEDAS +100" "$T/server.log" || { echo "FALHOU: log de auditoria sem a linha MOEDAS da alice"; L=1; }
grep -q "\[admin\] .* fb:alice (jogador [0-9]*) TELEPORTE" "$T/server.log" || { echo "FALHOU: log sem TELEPORTE"; L=1; }
grep -q "\[admin\] .* fb:alice (jogador [0-9]*) VOAR" "$T/server.log" || { echo "FALHOU: log sem VOAR"; L=1; }
if grep -E "\[admin\] .* (fb:bob|anon:)" "$T/server.log"; then echo "FALHOU: comando de NAO-admin no log de admin"; L=1; fi
grep -q "\[debug\] .* IGNORADO" "$T/server.log" || { echo "FALHOU: comandos ignorados nao foram contados no log"; L=1; }
grep -q "linha(s) invalida(s)" "$T/server.log" || { echo "FALHOU: linha invalida do arquivo nao foi avisada"; L=1; }
[ $R -eq 0 ] && [ $L -eq 0 ] && echo "ADMINS: TODOS OS TESTES PASSARAM" || { echo "ADMINS: FALHOU ($R/$L)"; exit 1; }
