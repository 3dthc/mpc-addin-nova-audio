#!/bin/sh
# Gen2 device build (MPC Live III and kin): MPC there is a 64-bit binary with no 32-bit libraries, so an addin it
# preloads has to be aarch64 too. arm64v8/gcc:11-bullseye has glibc 2.31, well inside the Gen2 ceiling (2.39) and
# the 2.x one (about 2.32), so one library covers every MPC OS the hardware runs. Needs Docker with QEMU for arm64.
#
# NOT RUN ON A GEN2 DEVICE. This builds and links, and the hooks are the same C as the armv7 build, but nobody has
# loaded it into a Gen2 MPC: see the status section of README.md before trusting it with a live set.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$DIR/build/package-aarch64"
docker run --rm --platform linux/arm64 -v "$DIR":/w -w /w arm64v8/gcc:11-bullseye sh -euc '
  apt-get update -qq >/dev/null && apt-get install -y -qq libasound2-dev >/dev/null
  gcc -shared -fPIC -fvisibility=hidden -std=gnu11 -O2 -Wall -Wextra -Isrc \
      -o build/package-aarch64/libnova_audio.so src/nova_audio.c -ldl -Wl,--no-undefined
  strip build/package-aarch64/libnova_audio.so
  echo "arch: $(readelf -h build/package-aarch64/libnova_audio.so | grep Machine)"
  echo "exported: $(readelf --dyn-syms -W build/package-aarch64/libnova_audio.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND | awk "{print \$8}" | tr "\n" " ")"
  echo "highest glibc: $(readelf -V build/package-aarch64/libnova_audio.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)"
'
cp "$DIR/addin.manifest" "$DIR/build/package-aarch64/" 2>/dev/null || true
echo "built: build/package-aarch64/libnova_audio.so"
