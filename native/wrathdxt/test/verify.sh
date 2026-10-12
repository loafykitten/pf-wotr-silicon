#!/usr/bin/env bash
# Check libwrathdxt against the game's own DXT compressor, on the game's own Mono.
# usage: test/verify.sh [Wrath build dir, default builds/arm64] [fuzz images, default 3000]
# Runs: tables; fuzz over all modes, 3 seeds; the game's own entry point; crafted refine
# vectors (Mono's float rounding at Quantize); and a benchmark. Exits non-zero on any
# mismatch.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
build="$(cd "${1:-$root/builds/arm64}" && pwd)"
images="${2:-3000}"
out="$here/build"
lib="$here/../build/libwrathdxt.dylib"
mkdir -p "$out"

"$here/../build.sh" >/dev/null
cflags=(-std=c11 -O2 -arch arm64 -ffp-contract=off -fwrapv -Wall -Wextra -Werror)
clang "${cflags[@]}" -o "$out/monohost" "$here/monohost.c"
clang "${cflags[@]}" -fvisibility=hidden -DWRATHDXT_TEST -dynamiclib -o "$out/libwrathdxt-test.dylib" "$here/../wrathdxt.c"
clang "${cflags[@]}" -Wno-unused-function -o "$out/refine_vectors" "$here/refine_vectors.c"
"$out/refine_vectors" > "$out/refine-vectors.txt"
dotnet build "$here/Verify/Verify.csproj" -c Release -o "$out/verify" -p:WrathBuild="$build" -v q -nologo >/dev/null

run() { "$out/monohost" "$build/Wrath.app" "$out/verify/Verify.exe" "$@"; }
run "$lib" tables
for seed in 1 2 3; do run "$lib" fuzz "$images" "$seed"; done
run "$lib" game $((images / 10)) 4
run "$out/libwrathdxt-test.dylib" refine "$out/refine-vectors.txt"
run "$lib" bench
echo "verify: all checks passed"
