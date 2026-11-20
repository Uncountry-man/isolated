cmake_minimum_required(VERSION 3.22)
project(isolated LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(native_app_glue STATIC ${GLUE}/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${GLUE})

add_library(isolated SHARED
    engine/src/engine.cpp
    engine/src/renderer.cpp
    engine/src/loading.cpp
    engine/src/main_menu.cpp
    engine/src/anim.cpp
    engine/src/net_client.cpp
    engine/src/camera_rig.cpp
    engine/src/terrain_renderer.cpp
    engine/src/map_gen.cpp
    engine/src/audio.cpp
    shared/world.cpp
    shared/world_sync.cpp
    engine/src/wifi_lock.cpp
    engine/src/chat_input.cpp
    engine/src/monetize.cpp
    engine/src/auth.cpp
    game/main_game.cpp)
target_include_directories(isolated PRIVATE engine/src)
# IP do servidor de jogo (definido pelo build.sh; vazio = offline)
set(E0_SERVER_HOST "" CACHE STRING "IPv4 do servidor")
if(E0_SERVER_HOST)
    target_compile_definitions(isolated PRIVATE E0_SERVER_HOST="${E0_SERVER_HOST}")
endif()
# Monetização: E0_ADS_TEST=1 (padrão) = blocos de anúncio de TESTE do Google + compra simulada se o produto não existir na Play.
# Produção: E0_ADS_TEST=0 ./build.sh (exige os IDs reais em monetize.cpp e no AndroidManifest.xml)
set(E0_ADS_TEST ON CACHE BOOL "anuncios/compras em modo teste")
if(E0_ADS_TEST)
    target_compile_definitions(isolated PRIVATE E0_ADS_TEST=1)
else()
    target_compile_definitions(isolated PRIVATE E0_ADS_TEST=0)
endif()
# Build de PRODUÇÃO (E0_RELEASE=1 ./build.sh): E0_DEBUG_UI=0 tira do binário o painel de debug, a aba GOD, teleportes, troca de hora,
# spawn de navio, voo, vida infinita e os cheats (e o envio das mensagens MsgDebug*). Desenvolvimento: E0_DEBUG_UI=1 (padrão).
set(E0_DEBUG_UI ON CACHE BOOL "painel de debug e cheats")
if(E0_DEBUG_UI)
    target_compile_definitions(isolated PRIVATE E0_DEBUG_UI=1)
else()
    target_compile_definitions(isolated PRIVATE E0_DEBUG_UI=0)
endif()
# Modo cinema (E0_CINEMA=1 ./build.sh): só para fazer vídeos; o jogo normal não leva nada disto
set(E0_CINEMA OFF CACHE BOOL "modo cinema")
if(E0_CINEMA)
    target_sources(isolated PRIVATE engine/src/cinema_rec.cpp)
    target_compile_definitions(isolated PRIVATE E0_CINEMA=1)
    target_link_libraries(isolated mediandk)
endif()
target_compile_options(isolated PRIVATE -Wall -Wextra -O3 -gline-tables-only -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden)
# .so menor: só exporta ANativeActivity_onCreate + JNI (o glue usa visibilidade default), tira seções sem uso.
# SEM --strip-all aqui: o .so sai com símbolos + tabela de linhas (-gline-tables-only, não muda o código gerado); o build.sh
# grava a cópia "strippada" no APK/AAB e guarda os símbolos para a Play (build/symbols, BUNDLE-METADATA/...debugsymbols).
target_link_options(isolated PRIVATE -Wl,--gc-sections -Wl,--icf=safe)
# -u: garante que ANativeActivity_onCreate do glue seja exportado
target_link_libraries(isolated -Wl,-u,ANativeActivity_onCreate native_app_glue android EGL GLESv3 aaudio mediandk log
    -Wl,-z,max-page-size=16384)
