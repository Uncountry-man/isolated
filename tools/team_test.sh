#!/usr/bin/env bash
# Teste da TRIPULAÇÃO de ponta a ponta: contas de teste + servidor com --data, `teamtest run`, REINICIA o servidor e `teamtest after`.
# Uso: tools/team_test.sh [porta]   (depois de ./build_server.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-7797}"
T="$(mktemp -d "${TMPDIR:-/tmp}/e0team.XXXXXX")"
python3 tools/make_test_jwt.py build/jwttest isolated-multiplayer alice bob carol >/dev/null
cp worlds/world.e0w "$T/world.e0w"
build/teamtest seed "$T/data"
run() { build/server "$PORT" --debug --no-protect --no-islands --world "$T/world.e0w" --data "$T/data" --jwk build/jwttest/test.jwk --fcm-cmd "cat >> $T/push.log" >>"$T/server.log" 2>&1 & echo $!; }
stop() { kill -TERM "$1" 2>/dev/null; for i in $(seq 100); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; kill -9 "$1"; }
PID=$(run); sleep 2.5
set +e
build/teamtest run "$PORT" build/jwttest; R1=$?
stop "$PID"
echo "--- servidor REINICIADO ---"
PID=$(run); sleep 2.5
build/teamtest after "$PORT" build/jwttest; R2=$?
NETTEST_PUSHLOG="$T/push.log" build/nettest "$PORT" teaminv build/jwttest; R3=$?
stop "$PID"
set -e
echo "--- log do servidor: $T/server.log ---"; grep -E "tripulacao|Tripulacoes" "$T/server.log" | tail -30
[ $R1 -eq 0 ] && [ $R2 -eq 0 ] && [ $R3 -eq 0 ] && echo "TRIPULACAO: TODOS OS TESTES PASSARAM" || { echo "TRIPULACAO: FALHOU ($R1/$R2/$R3)"; exit 1; }
