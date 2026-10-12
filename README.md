# pf-wotr-silicon

Run **Pathfinder: Wrath of the Righteous** natively on Apple Silicon.

The Mac version on Steam is an Intel build, so it runs under Rosetta. This repository
builds a native arm64 copy of the game from your own Steam install. The copy is the game's
own data and code, unchanged, running on Unity's arm64 player for the same Unity version
(2020.3.48f1). Steam keeps working, and so do Unity Mod Manager mods. The copy also comes
with **WrathPerf**, an optional mod that makes loads faster.

**No game files are included or distributed.** You need to own the game on Steam. Every
third-party piece (the Unity player, Unity Mod Manager, Harmony, Steamworks.NET's library)
is something you download yourself from its official source. The scripts never modify your
Steam install. They build APFS clones of it under `builds/`. A clone takes almost no extra
disk space, but only when this checkout is on the same APFS volume as the Steam library.
Otherwise `cp -c` falls back to a full copy, about 46 GB per build.

## What you get

Measured on an M2 Max (macOS 27): the same saves and the same scenes, with three runs of
each build.

| | Steam build (Rosetta) | Native arm64 |
|---|---|---|
| Hitches (frames ≥ 50 ms), first play of a cutscene | 36–37 | 5–6 |
| Worst frame, first play | ~520 ms | ~130 ms |
| p99 frame time | 34 ms | 22 ms |
| Area transition | 11.6 s | 8.7 s |
| Manual save | 0.46 s | 0.33 s |

The native first-play hitch count is with incremental GC, which is on by default.

WrathPerf cuts each save load from about 11.5 s to about 6.5 s, and the first load in a
session from about 12.5 s to about 7.5 s. Two changes add up to that:

- **A native DXT compressor.** The game compresses character texture atlases with a C#
  compressor. Without Burst, which is x86_64-only, the arm64 build runs it as plain Mono
  code. `libwrathdxt` is the same algorithm compiled for the CPU, and its output is
  byte-identical. That was checked on about 9.3 million fuzzed blocks and on every atlas of
  six real loads, with zero mismatches.
- **Shorter cleanup waits.** Each load waits 100 frames before each of its two cleanup
  passes. WrathPerf waits 10. The wait frames can be changed in the mod's settings.

Each change has its own toggle in the mod's settings, so you can compare against the game's
own behaviour.

## What's here

| Path | What it is |
|---|---|
| `tools/build-arm64-bundle.sh` | Builds `builds/arm64/Wrath.app`: clones the game, swaps in the arm64 player, the Steam shim, Unity Mod Manager and Harmony 2.4.2, and turns on incremental GC. |
| `tools/install-umm.exp` | Runs Unity Mod Manager's console installer against a given `.app`, never the Steam install. |
| `tools/launch.sh` | Launches a build outside Steam's Play button. Steam must be running. |
| `native/steamshim/` | `libsteam_api.dylib` for arm64. The game was built against Steamworks SDK 1.41, whose library is x86_64-only. The shim implements the 1.41 API on top of a current universal `libsteam_api`. |
| `native/wrathdxt/` | WrathPerf's native DXT compressor, plus `test/verify.sh`, which checks it against the game's own compressor. |
| `mods/WrathPerf/` | The WrathPerf mod (Unity Mod Manager). |

## Requirements

- An Apple Silicon Mac. The game installed from Steam, and Steam running.
- Xcode Command Line Tools (`xcode-select --install`).
- [Unity Hub](https://unity.com/download) with **Unity 2020.3.48f1** and its **Mac Build
  Support (Mono)** module. Only the player template inside the editor install is used.
- The [GitHub CLI](https://cli.github.com) (`gh`), logged in. `native/steamshim/build.sh`
  uses it to fetch the Steamworks SDK 1.41 headers and the Steamworks.NET release it builds
  against.
- Mono (`brew install mono`), to run Unity Mod Manager's console installer.
- The [.NET SDK](https://dotnet.microsoft.com/download), to build WrathPerf.

## Building

Run everything from the repository root.

### 1. Build the Steam shim

```sh
native/steamshim/build.sh
```

### 2. Make a Unity Mod Manager build of the Intel game

Unity Mod Manager's patch is managed code, so the arm64 build copies it from an Intel build
instead of running the installer on the arm64 build. Clone the game and install UMM into the
clone:

```sh
mkdir -p builds/x64-modded builds/tools
cp -cRp ~/"Library/Application Support/Steam/steamapps/common/Pathfinder Second Adventure/Wrath.app" builds/x64-modded/
```

Download Unity Mod Manager from its official source (the
[project page](https://github.com/newman55/unity-mod-manager) links to it), and unzip it to
`builds/tools/UnityModManagerInstaller/`. Version 0.32.4 was tested. Then:

```sh
tools/install-umm.exp builds/tools/UnityModManagerInstaller builds/x64-modded/Wrath.app
```

The script answers the installer's prompts. It refuses to continue unless the installer is
patching the clone, because the installer defaults to the Steam install it detects.

### 3. Fetch the patched Unity player and Harmony

**The Unity player.** Unity's arm64 player for 2020.3.48f1 predates the fix for
CVE-2025-59489, so the build uses a copy patched with Unity's own Application Patcher.

1. Download Unity's patcher and unzip it. It's an Intel app, so it needs Rosetta 2:

   ```sh
   curl -fLo builds/tools/unity-patcher.zip https://security-patches.unity.com/bc0977e0-21a9-4f6e-9414-4f44b242110a/unity-patcher/UnityApplicationPatcher-1.3.3-macOS-x64.zip
   unzip -q builds/tools/unity-patcher.zip -d builds/tools/unity-patcher
   ```

2. Copy the template's player library:

   ```sh
   template="/Applications/Unity/Hub/Editor/2020.3.48f1/Unity.app/Contents/PlaybackEngines/MacStandaloneSupport/Variations/macos_arm64_nondevelopment_mono/UnityPlayer.app/Contents"
   mkdir -p builds/tools/unity-cve-2025-59489
   cp "$template/Frameworks/UnityPlayer.dylib" builds/tools/unity-cve-2025-59489/UnityPlayer-2020.3.48f1-arm64-patched.dylib
   ```

3. Patch the copy:

   ```sh
   builds/tools/unity-patcher/UnityApplicationPatcher.app/Contents/MacOS/UnityApplicationPatcherCLI \
     -macos -unityPlayerLibrary builds/tools/unity-cve-2025-59489/UnityPlayer-2020.3.48f1-arm64-patched.dylib \
     -skipSigning -allowStandaloneLibrary
   ```

The build script checks the SHA-256 of both the template and the patched library. It stops
if either isn't the one this repository was tested with.

**Harmony.** Get Harmony 2.4.2 from
[its releases](https://github.com/pardeike/Harmony/releases). The Harmony version Unity Mod
Manager bundles can't patch code on Apple Silicon, so the build swaps in 2.4.2. From the
**Fat** zip, put `net48/0Harmony.dll` at `builds/tools/harmony-2.4.2/0Harmony.dll`.

### 4. Build the arm64 game

```sh
tools/build-arm64-bundle.sh
```

The script:

- clones the Steam install to `builds/arm64/Wrath.app`;
- swaps in the arm64 player, the Steam shim, Unity Mod Manager and Harmony;
- removes x86_64-only plugins the game doesn't need on Steam (GOG Galaxy, FBX, Burst's AOT
  library);
- checks that every remaining binary is arm64;
- verifies, on exit, that nothing in the Steam install changed.

The first time, it copies any mods from the Intel build's `Mods/` folder.

### 5. Build WrathPerf (optional)

```sh
dotnet build -c Release mods/WrathPerf/WrathPerf.csproj
```

This builds `libwrathdxt` too, and installs the mod in `builds/arm64/Mods/WrathPerf/`.

To check the compressor against the game's own, on the game's own Mono, run
`native/wrathdxt/test/verify.sh`.

## Playing

With Steam running:

```sh
tools/launch.sh
```

The native build uses the same saves and settings as the Steam build. Unity Mod Manager's
window opens with Ctrl+F10 as usual, and WrathPerf's toggles are there.

**After a game update,** rerun `tools/build-arm64-bundle.sh`. If the update changed
`UnityEngine.UIModule.dll`, the script stops and asks you to redo step 2 first.

**To remove the builds,** delete `builds/`. That covers the game copies and the downloaded
tools. What's left is small: build outputs under `native/*/build/`, `native/steamshim/sdk-1.41/`
and `mods/*/bin|obj`, plus Unity Mod Manager's settings in `~/.local/share/UnityModManagerNet/`.
The Steam install was never touched.

## Credits

WrathPerf's compressor is stb_dxt by Fabian Giesen and Sean Barrett, by way of Roman
Shapiro's [StbDxtSharp](https://github.com/StbSharp/StbDxtSharp). Both are public domain.
The Steam shim is built against the Steamworks SDK 1.41 headers from
[Steamworks.NET-CodeGen](https://github.com/rlabrecque/Steamworks.NET-CodeGen), and it
forwards to the `libsteam_api` shipped by [Steamworks.NET](https://github.com/rlabrecque/Steamworks.NET).
The build script downloads both.

## License

The code in this repository is MIT-licensed (see [LICENSE](LICENSE)). The game, Unity,
Unity Mod Manager, Harmony and the Steamworks libraries belong to their owners, are under
their own licenses, and aren't part of this repository. This project isn't affiliated with
Owlcat Games, Paizo, Unity or Valve.
