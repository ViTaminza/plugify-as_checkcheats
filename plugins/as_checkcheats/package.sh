#!/bin/bash
# Assembles build/package/as_checkcheats: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/as_checkcheats"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/as_checkcheats/libas_checkcheats.so" "$OUT/bin/"
cp "$ROOT/build/plugins/as_checkcheats/as_checkcheats.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
