#!/usr/bin/env bash
# Teste de PERSISTÊNCIA de ponta a ponta: sobe o servidor com --data num diretório temporário, roda `nettest persist1`, REINICIA o
# servidor (SIGTERM = desligamento limpo) e roda `nettest persist2`. Uso: tools/persist_test.sh [porta]   (depois de ./build_server.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-7795}"
T="$(mktemp -d "${TMPDIR:-/tmp}/e0persist.XXXXXX")"
python3 tools/make_test_jwt.py build/jwttest isolated-multiplayer alice bob carol >/dev/null
cp worlds/world.e0w "$T/world.e0w"
run() { build/server "$PORT" --debug --no-islands --world "$T/world.e0w" --data "$T/data" --jwk build/jwttest/test.jwk >>"$T/server.log" 2>&1 & echo $!; }
stop() { kill -TERM "$1" 2>/dev/null; for i in $(seq 100); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; kill -9 "$1"; }
PID=$(run); sleep 2.5
set +e
build/nettest "$PORT" persist1 build/jwttest; R1=$?
stop "$PID"
echo "--- servidor REINICIADO ---"
PID=$(run); sleep 2.5
build/nettest "$PORT" persist2 build/jwttest; R2=$?
stop "$PID"
set -e
echo "--- log do servidor: $T/server.log ---"; grep -E "conta|compra|moedas|dados|Estado do mundo|Contas:" "$T/server.log" | tail -25
ls -R "$T/data" | head -20
[ $R1 -eq 0 ] && [ $R2 -eq 0 ] && echo "PERSISTENCIA: TODOS OS TESTES PASSARAM" || { echo "PERSISTENCIA: FALHOU ($R1/$R2)"; exit 1; }
