#!/usr/bin/env bash
# Build the native DXT compressor for WrathPerf.
# Output: native/wrathdxt/build/libwrathdxt.dylib (arm64)
# Check it against the game's own compressor with test/verify.sh before installing.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="$here/build"
mkdir -p "$out"

# -ffp-contract=off: no fused multiply-add, which rounds differently from Mono's code.
# -fwrapv: C# integer overflow semantics. No -ffast-math, ever.
clang -std=c11 -O2 -arch arm64 -mmacosx-version-min=11.0 \
  -ffp-contract=off -fwrapv -fvisibility=hidden -Wall -Wextra -Werror \
  -dynamiclib -install_name @loader_path/libwrathdxt.dylib \
  -o "$out/libwrathdxt.dylib" "$here/wrathdxt.c"

lipo -archs "$out/libwrathdxt.dylib"
nm -gU "$out/libwrathdxt.dylib" | awk '{print $3}'
echo "built $out/libwrathdxt.dylib"
