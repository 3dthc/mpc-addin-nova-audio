#!/bin/sh
# Device build, the way the catalog expects: arm32v7/gcc:11-bullseye has glibc 2.31, so the library loads on
# MPC OS 2.x (about 2.32) as well as 3.x. Needs Docker with QEMU for arm/v7.
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$DIR/build/package"
docker run --rm --platform linux/arm/v7 -v "$DIR":/w -w /w arm32v7/gcc:11-bullseye sh -euc '
  gcc -shared -fPIC -fvisibility=hidden -std=gnu11 -O2 -Wall -Wextra -Isrc -Ithird_party \
      -o build/package/libnova_audio.so src/nova_audio.c -ldl -Wl,--no-undefined
  strip build/package/libnova_audio.so
  echo "exported: $(readelf --dyn-syms -W build/package/libnova_audio.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND | awk "{print \$8}" | tr "\n" " ")"
  echo "highest glibc: $(readelf -V build/package/libnova_audio.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)"
'
cp "$DIR/addin.manifest" "$DIR/build/package/" 2>/dev/null || true
echo "built: build/package/libnova_audio.so"
