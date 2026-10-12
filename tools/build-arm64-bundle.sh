#!/usr/bin/env bash
# Build builds/arm64/Wrath.app: the game's own data and managed code on Unity
# 2020.3.48f1's native arm64 Mono player. Never touches the Steam install (the bundle is
# an APFS clone of it). Rerun after every game update.
#
# usage: tools/build-arm64-bundle.sh [--incremental-gc=MS | --no-incremental-gc]
#   --incremental-gc=MS Unity's incremental GC time slice (boot.config gc-max-time-slice).
#                       On by default at 3 ms (Unity's default): it removed every
#                       GC-coincident hitch in a cutscene benchmark at no measurable
#                       cost. --no-incremental-gc turns it off.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
steam_common="$HOME/Library/Application Support/Steam/steamapps"
src="$steam_common/common/Pathfinder Second Adventure/Wrath.app"
variation="macos_arm64_nondevelopment_mono"
template="/Applications/Unity/Hub/Editor/2020.3.48f1/Unity.app/Contents/PlaybackEngines/MacStandaloneSupport/Variations/$variation/UnityPlayer.app/Contents"
out="$root/builds/arm64"
app="$out/Wrath.app"
c="$app/Contents"

gc_slice=3
for arg in "$@"; do
  case "$arg" in
    --incremental-gc) gc_slice=3 ;;
    --no-incremental-gc) gc_slice="" ;;
    --incremental-gc=*) gc_slice="${arg#*=}" ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done
[[ -z "$gc_slice" || "$gc_slice" =~ ^[0-9]+$ ]] || { echo "--incremental-gc wants whole milliseconds" >&2; exit 2; }

rc=0
pgrep -qx 'Pathfinder Wrath Of The Righteous' || rc=$?
case $rc in
  0) echo "quit Wrath first" >&2; exit 1 ;;
  1) ;;
  *) echo "pgrep failed (exit $rc); can't tell whether Wrath is running" >&2; exit 1 ;;
esac
[[ -d "$src" ]] || { echo "no Steam install at $src" >&2; exit 1; }
[[ -d "$template" ]] || { echo "no Unity player template at $template" >&2; exit 1; }

steam_libs=("$root/native/steamshim/build/libsteam_api.dylib" "$root/native/steamshim/build/libsteam_api_modern.dylib")
for f in "${steam_libs[@]}"; do
  [[ -f "$f" ]] || { echo "missing $f (build it first)" >&2; exit 1; }
done

# Tripwire: nothing under the Steam install may change while this script runs. Checked on
# every exit, including failures. (A build once patched the Steam install by mistake;
# see tools/install-umm.exp.)
# Two checks: content hashes of every code-bearing file (Managed DLLs, native binaries,
# Info.plist; a few hundred files, under a second), and modification times for
# everything else (46 GB of data is too much to hash on every build).
marker="$(mktemp)"
code_manifest() {
  (cd "$src/Contents" && find Resources/Data/Managed Frameworks MacOS PlugIns Info.plist -type f -print0 |
    sort -z | xargs -0 shasum -a 256)
}
before="$(code_manifest)"
verify_steam_untouched() {
  local status=$? changed after
  changed="$(find "$src" -newer "$marker" -print 2>/dev/null || echo "(scan failed)")"
  after="$(code_manifest 2>/dev/null || echo "(manifest failed)")"
  rm -f "$marker"
  if [[ -n "$changed" || "$before" != "$after" ]]; then
    echo "STEAM INSTALL CHANGED during this build:" >&2
    [[ -n "$changed" ]] && printf '%s\n' "$changed" | sed -n '1,5p' >&2
    [[ "$before" != "$after" ]] && echo "  (code files differ from the start of the build)" >&2
    echo "If Steam wasn't updating the game just now, restore it: Steam > Verify integrity of game files." >&2
    exit 1
  fi
  exit "$status"
}
trap verify_steam_untouched EXIT

# Only ever delete or write inside this repo's builds/arm64, resolved physically, so a
# symlinked builds/ or Wrath.app can't redirect writes into the Steam install.
mkdir -p "$out"
[[ -L "$out" || -L "$app" ]] && { echo "refusing: $out or $app is a symlink" >&2; exit 1; }
[[ "$(cd "$out" && pwd -P)" == "$(cd "$root" && pwd -P)/builds/arm64" ]] ||
  { echo "refusing: $out doesn't resolve to $root/builds/arm64" >&2; exit 1; }

echo "cloning the Steam install (APFS clone, no extra space)..."
rm -rf "$app"
cp -cRp "$src" "$app"
# Every later write goes through paths inside the clone; a symlink could point back out.
no_symlinks() {
  local found
  found="$(find "$@" -type l -print -quit)"
  [[ -z "$found" ]] || { echo "refusing: symlink at $found (writes could escape the build)" >&2; exit 1; }
}
no_symlinks "$app"

echo "swapping in the arm64 player ($variation)..."
for lib in libmonobdwgc-2.0.dylib libMonoPosixHelper.dylib; do
  cp "$template/Frameworks/$lib" "$c/Frameworks/$lib"
done
# UnityPlayer.dylib: Unity's CVE-2025-59489 fix for this exact template (the stock game's
# x86_64 player is already patched; the 2020.3.48f1 arm64 template isn't). Fetched once with
# Unity's Application Patcher 1.3.3:
#   UnityApplicationPatcherCLI -macos -unityPlayerLibrary <copy of the template dylib> \
#     -skipSigning -allowStandaloneLibrary
# (download: security-patches.unity.com/bc0977e0-21a9-4f6e-9414-4f44b242110a/unity-patcher/
#  UnityApplicationPatcher-1.3.3-macOS-x64.zip), saved as $patched_player.
template_player_sha=bad34837b3e1f08e04bd75027e8effb3df30603282ea56cba2f9c4d2eff4ba9b
patched_player_sha=3a0764d731a8c914490eecf3edb2d12adfb180fc5b12a12740b48c199b10dfef
patched_player="$root/builds/tools/unity-cve-2025-59489/UnityPlayer-2020.3.48f1-arm64-patched.dylib"
[[ "$(shasum -a 256 "$template/Frameworks/UnityPlayer.dylib" | cut -d' ' -f1)" == "$template_player_sha" ]] ||
  { echo "the Unity template's UnityPlayer.dylib isn't the one the CVE patch was made for" >&2; exit 1; }
[[ -f "$patched_player" && "$(shasum -a 256 "$patched_player" | cut -d' ' -f1)" == "$patched_player_sha" ]] ||
  { echo "missing or wrong $patched_player; re-fetch it with Unity's patcher (see comment above)" >&2; exit 1; }
cp "$patched_player" "$c/Frameworks/UnityPlayer.dylib"
# The base launcher and the three DLC launchers are all Unity's stub executable.
for exe in "$c/MacOS"/*; do
  cp "$template/MacOS/UnityPlayer" "$exe"
done

if [[ -n "$gc_slice" ]]; then
  # The player turns this into Mono's --gc-params=incremental=1 (verified: WrathStats then
  # reports GarbageCollector.isIncremental = true).
  echo "enabling incremental GC (${gc_slice} ms slice)..."
  printf 'gc-max-time-slice=%s\n' "$gc_slice" >> "$c/Resources/Data/boot.config"
fi

echo "native plugins..."
# Unused on Steam or not loaded at runtime; all x86_64-only.
#   CSteamworks: old Steamworks.NET C wrapper, no managed code imports it.
#   Galaxy: GOG only. FBX SDK: editor-time import. lib_burst_generated: x86_64 Burst
#   AOT code; without it Burst jobs run as ordinary managed code (slower than Burst, but correct).
rm -rf "$c/PlugIns/CSteamworks.bundle" "$c/PlugIns/libGalaxy64.dylib" "$c/PlugIns/libGalaxyCSharpGlue.dylib" \
       "$c/PlugIns/UnityFbxSdkNative.bundle" "$c/PlugIns/lib_burst_generated.bundle" \
       "$c/Frameworks/MonoEmbedRuntime/osx/libGalaxyCSharpGlue.dylib"
rm -f "$c/PlugIns/steam_api.bundle/Contents/MacOS/"*
cp "${steam_libs[@]}" "$c/PlugIns/steam_api.bundle/Contents/MacOS/"

echo "checking architectures..."
bad=0
while IFS= read -r -d '' f; do
  if file -b "$f" | grep -q 'Mach-O'; then
    if ! lipo -archs "$f" | grep -qw arm64; then
      echo "  not arm64: ${f#"$c/"}"
      bad=1
    fi
  fi
done < <(find "$c/MacOS" "$c/Frameworks" "$c/PlugIns" -type f -print0)
[[ $bad == 0 ]] || { echo "x86_64-only binaries remain; fix the list above" >&2; exit 1; }

echo "installing Unity Mod Manager (copying the x64 build's assembly patch)..."
# UMM's assembly patch is managed code, so the x64 build's patched UIModule works as is.
# Copying it avoids running UMM's interactive installer, which once patched the Steam
# install by mistake (see tools/install-umm.exp). Only copy if the patch was made from
# the same original DLL the game ships now.
x64_managed="$root/builds/x64-modded/Wrath.app/Contents/Resources/Data/Managed"
managed="$c/Resources/Data/Managed"
[[ -f "$x64_managed/UnityEngine.UIModule.dll.original_" && -d "$x64_managed/UnityModManager" ]] ||
  { echo "no UMM install in $x64_managed to copy from" >&2; exit 1; }
if ! cmp -s "$x64_managed/UnityEngine.UIModule.dll.original_" "$managed/UnityEngine.UIModule.dll"; then
  echo "the game's UnityEngine.UIModule.dll changed since the x64 build was patched;" >&2
  echo "reinstall UMM on builds/x64-modded first (tools/install-umm.exp), then rerun" >&2
  exit 1
fi
cp "$managed/UnityEngine.UIModule.dll" "$managed/UnityEngine.UIModule.dll.original_"
cp "$x64_managed/UnityEngine.UIModule.dll" "$managed/UnityEngine.UIModule.dll"
no_symlinks "$x64_managed/UnityModManager"
cp -R "$x64_managed/UnityModManager" "$managed/"
no_symlinks "$app"
# UMM 0.32.4 bundles Harmony 2.3.6, whose MonoMod backend can't patch code on Apple
# Silicon (mprotect RWX fails under W^X, errno 13). Harmony 2.4.0+ supports arm64
# including Apple Silicon. Mods compiled against Harmony 2.x bind to it unchanged.
harmony="$root/builds/tools/harmony-2.4.2/0Harmony.dll"
[[ -f "$harmony" ]] || { echo "missing $harmony (Harmony 2.4.2 Fat net48 from github.com/pardeike/Harmony releases)" >&2; exit 1; }
cp "$harmony" "$managed/UnityModManager/0Harmony.dll"
# The game ships its own Managed/0Harmony.dll (2.0.4, for Owlcat's built-in mod support)
# and Mono resolves "0Harmony" by name, so that copy loads first and UMM gets it. Replace
# it too; Harmony 2.4 is API-compatible within 2.x.
cp "$harmony" "$managed/0Harmony.dll"

# Mods live next to the .app. Seed from the x64 build's on first run; never overwrite.
if [[ ! -d "$out/Mods" ]]; then
  mkdir -p "$out/Mods"
  [[ -d "$root/builds/x64-modded/Mods" ]] && cp -R "$root/builds/x64-modded/Mods/." "$out/Mods/"
fi

# No re-signing: every binary copied in (Unity's template player and launcher, the
# Steam libraries, the universal plugins) already carries a valid ad-hoc signature, and
# the bundle as a whole can't be sealed (Owlcat keeps Bundles/ and blueprints.zip at the
# bundle root; the stock app is unsealed too). Just clear quarantine attributes.
xattr -cr "$app"

buildid="$(awk -F'"' '/"buildid"/{print $4; exit}' "$steam_common/appmanifest_1184370.acf" 2>/dev/null || true)"
rm -f "$out/BUILD_INFO"
{
  echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "steam_buildid=${buildid:-unknown}"
  echo "unity_variation=2020.3.48f1/$variation"
  echo "unity_player=CVE-2025-59489 patched (Unity Application Patcher 1.3.3)"
  echo "steam=shim"
  echo "incremental_gc=${gc_slice:-off}"
} > "$out/BUILD_INFO"
cat "$out/BUILD_INFO"
echo "done: $app"
echo "WrathPerf: dotnet build -c Release mods/WrathPerf/WrathPerf.csproj"
