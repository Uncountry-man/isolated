#!/usr/bin/env bash
# Teste da DOMINAÇÃO DE ILHAS + MELHORIAS DA ILHA de ponta a ponta: 2 tripulações (teamtest conqseed) + servidor com mundo, ilhas e tempos acelerados;
# nettest conquest; REINICIA o servidor e confere que a dona continua (nettest conquest2).  Uso: tools/conquest_test.sh [porta]
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-7798}"
T="$(mktemp -d "${TMPDIR:-/tmp}/e0conq.XXXXXX")"
cp worlds/world.e0w "$T/world.e0w"
build/teamtest conqseed "$T/data" >/dev/null
run() { build/server "$PORT" --debug --no-protect --world "$T/world.e0w" --data "$T/data" --cap-time-scale 0.05 --payout-sec 4 --own-all-ships >>"$T/server.log" 2>&1 & echo $!; }
stop() { kill -TERM "$1" 2>/dev/null; for i in $(seq 100); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; kill -9 "$1"; }
PID=$(run); for i in $(seq 60); do grep -q "Isolated server: UDP" "$T/server.log" && break; sleep 0.25; done
set +e
build/nettest "$PORT" conquest "$T/world.e0w"; R1=$?
build/nettest "$PORT" isle "$T/world.e0w"; R3=$?
stop "$PID"
echo "--- servidor REINICIADO ---"
PID=$(run); for i in $(seq 60); do [ "$(grep -c 'Isolated server: UDP' "$T/server.log")" -ge 2 ] && break; sleep 0.25; done
build/nettest "$PORT" conquest2 "$T/world.e0w"; R2=$?
build/nettest "$PORT" isle2 "$T/world.e0w"; R4=$?
stop "$PID"
set -e
echo "--- log do servidor: $T/server.log ---"; grep -E "\[ilha\]|Dominacao|estruturas" "$T/server.log" | tail -16
[ $R1 -eq 0 ] && [ $R2 -eq 0 ] && [ $R3 -eq 0 ] && [ $R4 -eq 0 ] && echo "DOMINACAO + MELHORIAS: TODOS OS TESTES PASSARAM" || { echo "DOMINACAO: FALHOU ($R1/$R2/$R3/$R4)"; exit 1; }
