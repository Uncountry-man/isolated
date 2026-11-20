#!/usr/bin/env bash
# ==============================================================================
# adb_game.sh - Controle de alta velocidade via ADB para Isolated (Android)
# Dispositivo alvo: moto g75 5G (ZY22LX8FHV) - Resolução: 2388x1080 Landscape
# ==============================================================================
set -e

SERIAL="${ADB_SERIAL:-ZY22LX8FHV}"
ADB="adb"
if [ -n "$SERIAL" ] && $ADB devices | grep -q "$SERIAL"; then
    ADB="adb -s $SERIAL"
fi

# Coordenadas fixas da HUD (calculadas para 2388x1080 Landscape)
JOY_X=272
JOY_Y=823
RUN_X=2168
RUN_Y=841
CAM_X=2168
CAM_Y=575
ACTION_X=1938
ACTION_Y=841
COLLECT_X=1938
COLLECT_Y=621
LOOK_X=1500
LOOK_Y=500

# Abas de Debug
# GERAL: 1770, REDE: 1835, JOG: 1895, MUNDO: 1965, OUTROS: 2045, GRAF: 2115, CAM: 2170, TERRA: 2235, GOD: 2310
TAB_Y=42

cmd_move() {
    local dir="${1:-forward}"
    local secs="${2:-1.0}"
    # Converte segundos em milissegundos inteiros
    local ms=$(awk "BEGIN {print int($secs * 1000)}")
    if [ "$ms" -lt 100 ]; then ms=100; fi

    local x2=$JOY_X
    local y2=$JOY_Y

    case "$dir" in
        forward|frente|up|w)
            y2=$((JOY_Y - 170))
            ;;
        back|backward|tras|down|s)
            y2=$((JOY_Y + 170))
            ;;
        left|esquerda|a)
            x2=$((JOY_X - 150))
            ;;
        right|direita|d)
            x2=$((JOY_X + 150))
            ;;
        *)
            echo "Direção inválida: $dir (use forward, back, left, right)" >&2
            return 1
            ;;
    esac

    $ADB shell input swipe $JOY_X $JOY_Y $x2 $y2 $ms
}

cmd_jump() {
    # Duplo toque rápido no botão de corrida (≤ 0.3s)
    $ADB shell "input tap $RUN_X $RUN_Y; sleep 0.08; input tap $RUN_X $RUN_Y"
}

cmd_action() {
    # Bater, cortar ou minerar
    $ADB shell input tap $ACTION_X $ACTION_Y
}

cmd_collect() {
    # Coletar graveto ou pedra
    $ADB shell input tap $COLLECT_X $COLLECT_Y
}

cmd_cam_preset() {
    # Alternar preset da câmera (Normal -> Perto -> Longe -> 1ª pessoa)
    $ADB shell input tap $CAM_X $CAM_Y
}

cmd_look() {
    local dir="${1:-right}"
    local dist="${2:-400}"
    local ms=250

    local x2=$LOOK_X
    local y2=$LOOK_Y

    case "$dir" in
        left|esquerda)
            x2=$((LOOK_X - dist))
            ;;
        right|direita)
            x2=$((LOOK_X + dist))
            ;;
        up|cima)
            y2=$((LOOK_Y + dist))
            ;;
        down|baixo)
            y2=$((LOOK_Y - dist))
            ;;
        *)
            echo "Direção de câmera inválida: $dir (use left, right, up, down)" >&2
            return 1
            ;;
    esac

    $ADB shell input swipe $LOOK_X $LOOK_Y $x2 $y2 $ms
}

cmd_tab() {
    local tab="${1:-GERAL}"
    case "$tab" in
        GERAL)   $ADB shell input tap 1770 $TAB_Y ;;
        REDE)    $ADB shell input tap 1835 $TAB_Y ;;
        JOG)     $ADB shell input tap 1895 $TAB_Y ;;
        MUNDO)   $ADB shell input tap 1965 $TAB_Y ;;
        OUTROS)  $ADB shell input tap 2045 $TAB_Y ;;
        GRAF)    $ADB shell input tap 2115 $TAB_Y ;;
        CAM)     $ADB shell input tap 2170 $TAB_Y ;;
        TERRA)   $ADB shell input tap 2235 $TAB_Y ;;
        GOD)     $ADB shell input tap 2310 $TAB_Y ;;
        *) echo "Aba desconhecida: $tab" >&2; return 1 ;;
    esac
}

cmd_god_fly() {
    # Abre aba GOD e clica na linha 1 (VOAR)
    cmd_tab GOD
    sleep 0.05
    $ADB shell input tap 2200 95
}

cmd_god_up() {
    cmd_tab GOD
    sleep 0.05
    $ADB shell input tap 2200 122
}

cmd_god_down() {
    cmd_tab GOD
    sleep 0.05
    $ADB shell input tap 2200 149
}

cmd_screenshot() {
    local out="${1:-/tmp/screenshot.png}"
    $ADB exec-out screencap -p > "$out"
    echo "Screenshot salvo em: $out"
}

cmd_status() {
    $ADB logcat -d -s Isolated | grep "DEBUG" | tail -n 3
}

# Loop de execução de sequência
cmd_seq() {
    for action in "$@"; do
        eval "$0 $action"
    done
}

case "$1" in
    move|andar)
        shift; cmd_move "$@" ;;
    jump|pular|pulo)
        cmd_jump ;;
    action|acao|bater|cortar|minerar)
        cmd_action ;;
    collect|coletar)
        cmd_collect ;;
    cam|camera)
        cmd_cam_preset ;;
    look|olhar|girar)
        shift; cmd_look "$@" ;;
    tab|aba)
        shift; cmd_tab "$@" ;;
    god-fly)
        cmd_god_fly ;;
    god-up)
        cmd_god_up ;;
    god-down)
        cmd_god_down ;;
    screenshot|print)
        shift; cmd_screenshot "$@" ;;
    status|log)
        cmd_status ;;
    seq|sequencia)
        shift; cmd_seq "$@" ;;
    *)
        echo "Uso: $0 <move|jump|action|collect|cam|look|tab|god-fly|god-up|god-down|screenshot|status|seq> [args...]"
        echo "Exemplos:"
        echo "  $0 move forward 2.0     # Anda para frente por 2 segundos"
        echo "  $0 jump                 # Pula (duplo toque rápido)"
        echo "  $0 look right 300       # Gira a câmera para a direita"
        echo "  $0 action               # Executa ação (bater/cortar/minerar)"
        echo "  $0 collect              # Coleta item"
        echo "  $0 seq 'move forward 1.5' 'jump' 'move forward 1.0'"
        exit 1
        ;;
esac
