#!/usr/bin/env bash
# Compila o servidor dedicado e as ferramentas de teste (host Linux). Uso: ./build_server.sh [run]
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -pthread server/server_main.cpp shared/world.cpp shared/world_sync.cpp -o build/server
g++ -std=c++17 -O2 -Wall -Wextra tools/nettest.cpp shared/world.cpp shared/world_sync.cpp -o build/nettest -lpthread
g++ -std=c++17 -O2 -Wall -Wextra -pthread tools/bot.cpp shared/world.cpp shared/world_sync.cpp -o build/bot
g++ -std=c++17 -O2 -Wall -Wextra tools/camtest.cpp engine/src/camera_rig.cpp -o build/camtest
g++ -std=c++17 -O2 -Wall -Wextra tools/worldtest.cpp shared/world.cpp -o build/worldtest
g++ -std=c++17 -O2 -Wall -Wextra tools/shiptest.cpp shared/world.cpp -o build/shiptest
g++ -std=c++17 -O2 -Wall -Wextra tools/oceantest.cpp shared/world.cpp -o build/oceantest
g++ -std=c++17 -O2 -Wall -Wextra -pthread tools/maptest.cpp engine/src/map_gen.cpp shared/world.cpp -o build/maptest
g++ -std=c++17 -O2 -Wall -Wextra tools/animtest.cpp engine/src/anim.cpp -o build/animtest
g++ -std=c++17 -O2 -Wall -Wextra tools/posebox.cpp engine/src/anim.cpp -o build/posebox
g++ -std=c++17 -O2 -Wall -Wextra tools/carrypreview.cpp engine/src/anim.cpp -o build/carrypreview
g++ -std=c++17 -O2 -Wall -Wextra -pthread tools/shipbake.cpp -o build/shipbake
g++ -std=c++17 -O2 -Wall -Wextra tools/cinematest.cpp -o build/cinematest
g++ -std=c++17 -O2 -Wall -Wextra tools/chesttest.cpp shared/world.cpp -o build/chesttest
g++ -std=c++17 -O2 -Wall -Wextra tools/authtest.cpp -o build/authtest
g++ -std=c++17 -O2 -Wall -Wextra -pthread tools/teamtest.cpp shared/world.cpp -o build/teamtest
echo "ok: build/server build/nettest build/bot build/camtest build/worldtest build/animtest build/posebox build/carrypreview build/chesttest build/authtest"
if [ "${1:-}" = "run" ]; then
    exec build/server
fi
