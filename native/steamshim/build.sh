#!/usr/bin/env bash
# Build the Steamworks SDK 1.41 compatibility shim as a universal libsteam_api.dylib,
# plus the current universal libsteam_api it forwards to (libsteam_api_modern.dylib).
# Output: native/steamshim/build/{libsteam_api.dylib,libsteam_api_modern.dylib}
#
# Inputs, fetched on first run and not committed (Valve's SDK files):
#   sdk-1.41/steam/  SDK 1.41 headers, from github.com/rlabrecque/Steamworks.NET-CodeGen
#                    commit 82717610e3 ("ship the steam 1.41 header files")
#   the modern libsteam_api.dylib from the Steamworks.NET Standalone release below
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
sdk="$here/sdk-1.41"
out="$here/build"
swnet_release="2025.164.1"
mkdir -p "$out"

if [[ ! -f "$sdk/steam/steam_api_flat.h" ]]; then
  echo "fetching SDK 1.41 headers..."
  mkdir -p "$sdk/steam"
  gh api 'repos/rlabrecque/Steamworks.NET-CodeGen/git/trees/82717610e3?recursive=1' \
    -q '.tree[] | select(.type=="blob") | .path' | grep '^steam/' | while IFS= read -r p; do
    gh api "repos/rlabrecque/Steamworks.NET-CodeGen/contents/$p?ref=82717610e3" -q .content | base64 -d > "$sdk/$p"
  done
fi

modern="$out/libsteam_api_modern.dylib"
if [[ ! -f "$modern" ]]; then
  echo "fetching the current universal libsteam_api (Steamworks.NET $swnet_release)..."
  tmp="$(mktemp -d)"
  gh release download "$swnet_release" -R rlabrecque/Steamworks.NET -p "Steamworks.NET-Standalone_$swnet_release.zip" -D "$tmp"
  unzip -q "$tmp"/*.zip -d "$tmp/x"
  cp "$tmp/x/OSX-Linux-x64/steam_api.bundle/Contents/MacOS/libsteam_api.dylib" "$modern"
  rm -rf "$tmp"
  # Give it its own install name: it's loaded next to our libsteam_api.dylib, and two
  # images with the same install name in one process is asking for trouble.
  install_name_tool -id @loader_path/libsteam_api_modern.dylib "$modern" 2>/dev/null
  codesign --force --sign - "$modern"
fi

echo "generating and compiling..."
python3 -I "$here/gen_flat.py" "$sdk/steam" "$out/flat_gen.cpp"
cxx=(clang++ -std=c++17 -O2 -fvisibility=hidden -arch arm64 -arch x86_64 -mmacosx-version-min=11.0)
"${cxx[@]}" -I"$sdk" -Wno-everything -c "$out/flat_gen.cpp" -o "$out/flat_gen.o"
"${cxx[@]}" -Wall -Wextra -Wno-unused-parameter -c "$here/core.cpp" -o "$out/core.o"
"${cxx[@]}" -dynamiclib -install_name @loader_path/libsteam_api.dylib \
  -o "$out/libsteam_api.dylib" "$out/flat_gen.o" "$out/core.o"

# Every function the game imports must be exported (list from Steamworks.NET 11.0.0's
# NativeMethods).
missing="$(comm -23 <(sort -u "$here/game-imports.txt") <(nm -gU "$out/libsteam_api.dylib" | awk '{print $3}' | sed 's/^_//' | sort -u))"
if [[ -n "$missing" ]]; then
  echo "missing exports the game imports:" >&2
  echo "$missing" >&2
  exit 1
fi
lipo -archs "$out/libsteam_api.dylib"
echo "built $out/libsteam_api.dylib ($(nm -gU "$out/libsteam_api.dylib" | wc -l | tr -d ' ') exports)"
