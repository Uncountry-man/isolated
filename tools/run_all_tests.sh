#!/usr/bin/env bash
# Roda TODOS os testes do PC (depois de ./build_server.sh): unitários + nettest em todos os modos (cada um com um servidor novo) + persistência.
# Uso: tools/run_all_tests.sh        Saída: uma linha por teste e o resumo no fim (exit 1 se algum falhar).
cd "$(dirname "$0")/.."
T="$(mktemp -d "${TMPDIR:-/tmp}/e0tests.XXXXXX")"; cp worlds/world.e0w "$T/w.e0w"
P=7811; FAIL=(); OKS=()
res() { if [ "$2" -eq 0 ]; then OKS+=("$1"); echo "[ok]    $1"; else FAIL+=("$1"); echo "[FALHA] $1 (log: $T/$1.log)"; fi; }
unit() { local n=$1; shift; "$@" >"$T/$n.log" 2>&1; res "$n" $?; }
net() {   # nome modo args_do_servidor...
    local n=$1 mode=$2; shift 2; P=$((P+1))
    build/server $P "$@" >"$T/$n.server.log" 2>&1 & local pid=$!; sleep 2.2
    if [ -n "$mode" ]; then timeout 300 build/nettest $P $mode >"$T/$n.log" 2>&1; else timeout 300 build/nettest $P >"$T/$n.log" 2>&1; fi
    local r=$?; kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null; res "$n" $r
}
unit worldtest build/worldtest
unit oceantest build/oceantest
unit shiptest build/shiptest
unit camtest build/camtest
unit animtest build/animtest
unit chesttest build/chesttest
python3 tools/make_test_jwt.py build/jwttest isolated-multiplayer alice bob carol >/dev/null
unit authtest build/authtest build/jwttest
net nettest-basico "" --flat --debug
net nettest-nodebug nodebug --flat
net nettest-world world --debug --world "$T/w.e0w"
net nettest-ship ship --debug --world "$T/w.e0w" --ships-at-start --own-all-ships --ship-respawn 3
net nettest-rescue rescue --flat --debug --no-protect
net nettest-gun gun --flat --debug --no-protect
net nettest-chat chat --flat --debug
net nettest-summon summon --debug --own-all-ships --world "$T/w.e0w"
net nettest-shipown shipown --debug --world "$T/w.e0w"
net nettest-cleanup cleanup --debug --world "$T/w.e0w" --own-all-ships --ship-cleanup 6
tools/persist_test.sh $((P+1)) >"$T/persist.log" 2>&1; res persist $?
tools/admin_test.sh $((P+2)) >"$T/admin.log" 2>&1; res admin $?
tools/team_test.sh $((P+3)) >"$T/team.log" 2>&1; res tripulacao $?
tools/conquest_test.sh $((P+4)) >"$T/conquest.log" 2>&1; res dominacao $?
echo; echo "${#OKS[@]} ok, ${#FAIL[@]} falha(s): ${FAIL[*]}   (logs em $T)"
[ ${#FAIL[@]} -eq 0 ]
