#!/bin/bash
# Assembles build/package/lr_unusual_kills: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/lr_unusual_kills"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/lr_unusual_kills/liblr_unusual_kills.so" "$OUT/bin/"
cp "$ROOT/build/plugins/lr_unusual_kills/lr_unusual_kills.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
