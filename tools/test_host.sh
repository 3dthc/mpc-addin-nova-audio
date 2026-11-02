#!/bin/sh
# Offline tests: a fake libasound and a fake MPC, both sample layouts, under UBSan. No device needed.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp "$DIR"/tests/*.c "$T/" 2>/dev/null || { echo "tests/ missing"; exit 1; }
cc -shared -fPIC -o "$T/libasound.so.2" "$T/fakeasound.c"
cc -shared -fPIC -fvisibility=hidden -std=gnu11 -O2 -Wall -Wextra -DNOVA_AUDIO_TEST -fsanitize=undefined \
   -I"$DIR/src" -o "$T/libnova_audio.so" "$DIR/src/nova_audio.c" -ldl
cc -std=gnu11 -I"$DIR/src" -o "$T/MPC" "$T/MPC.c" "$T/libasound.so.2" -ldl -Wl,-rpath,"$T"
cp "$T/MPC" "$T/notmpc"
cd "$T"
NOVA_AUDIO_TEST_CARD=0 LD_PRELOAD="$T/libnova_audio.so" ./MPC
NOVA_AUDIO_TEST_CARD=0 LD_PRELOAD="$T/libnova_audio.so" ./MPC n
NOVA_AUDIO_TEST_CARD=0 LD_PRELOAD="$T/libnova_audio.so" ./notmpc | head -1
echo "PASSED"
