# Using the patched engine on a machine

How to build this fork's Flutter engine and use it on your own computer or any other one: for `reelx render`, for `flutter test` while working on Reelx, and inside an app build. What each patch does is in [REELX_PATCHES.md](../REELX_PATCHES.md).

Everything here keeps the normal Flutter SDK. The patched engine is used next to it, and removing it brings back the stock engine. Reelx finds the engine's extra C functions at run time; with a stock engine it runs exactly as before.

## What you need

| | macOS | Linux | Windows |
|---|---|---|---|
| Flutter SDK | **3.47.9** exactly (the engine must match the SDK) | same | same |
| Build tools | Xcode (command line tools selected) | `python3`, `git`, `curl`, `pkg-config`, `libgtk-3-dev` (for the app library) | Visual Studio 2022 with "Desktop development with C++" and **Windows SDK 10.0.22621.0** (the engine asks for this version) |
| depot_tools | [Chromium's depot_tools](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html) on `PATH` | same | same, and `DEPOT_TOOLS_WIN_TOOLCHAIN=0` |
| Disk | ~40 GB for the checkout, 10–20 GB per build folder | same | same |
| Time | first sync 20–40 min; a build 30 min–3 h depending on the machine | same | same |

FFmpeg on `PATH` is needed for renders and export tests, as without the patched engine.

## 1. Get the source and sync

```sh
git clone --branch engine-perf/3.47.9-wip https://github.com/nploi/flutter.git flutter-engine
cd flutter-engine
cat > .gclient <<'EOF'
solutions = [
  {
    "custom_deps": {},
    "deps_file": "DEPS",
    "managed": False,
    "name": ".",
    "safesync_url": "",
    "url": "https://github.com/flutter/flutter.git",
    "custom_vars": {
      "download_android_deps": False,
      "download_fuchsia_deps": False,
      "download_jdk": False,
      "download_esbuild": False,
    },
  },
]
EOF
gclient sync -D --no-history --shallow
```

`engine-perf/3.47.9-wip` has every patch; `engine-perf/3.47.9` has only those already checked on every platform. For Android engine builds set `"download_android_deps": True` before syncing.

On Windows run the same in PowerShell (write `.gclient` with any editor) after `$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"`.

## 2. Build

All commands run from the checkout's root. `gn` writes a build folder under `engine/src/out/`; on Apple silicon its name ends in `_arm64` (for example `host_debug_arm64`).

**macOS, everything at once** (release and profile `FlutterMacOS.framework` for both CPUs, merged into universal ones, plus the tester):

```sh
reelx/tools/build_engine.sh            # add --sync to sync first
```

**Any OS, the tester only** (what `reelx render` and `flutter test` need):

```sh
python3 engine/src/flutter/tools/gn --no-rbe --no-goma          # → out/host_debug(_arm64)
third_party/ninja/ninja -C engine/src/out/host_debug \
  flutter/shell/testing:testing flutter/lib/snapshot:kernel_platform_files \
  flutter/flutter_frontend_server:frontend_server
```

Add `--unoptimized` to `gn` for a faster build that runs slower (folder `host_debug_unopt`). On Windows use `python` and `third_party\ninja\ninja.exe`.

**The app's engine library** (release mode, for an app build):

| OS | Configure | Target | Result |
|---|---|---|---|
| macOS | `reelx/tools/build_engine.sh` | (all) | `out/mac_release_universal/FlutterMacOS.framework` |
| Linux | `gn --runtime-mode release --no-rbe --no-goma` | `flutter/shell/platform/linux:flutter_linux_gtk` | `out/host_release/libflutter_linux_gtk.so` |
| Windows | `gn --runtime-mode release --no-rbe --no-goma` | `flutter/shell/platform/windows:flutter_windows` | `out\host_release\flutter_windows.dll` |

Finally, `flutter test --local-engine` looks for a `dart-sdk` inside the build folder. Link the one the sync downloaded:

```sh
# macOS (use macos-x64 on an Intel Mac), Linux (linux-x64)
ln -s ../../flutter/prebuilts/macos-arm64/dart-sdk engine/src/out/host_debug_arm64/dart-sdk
# Windows (PowerShell)
New-Item -ItemType Junction -Path engine\src\out\host_debug\dart-sdk -Target engine\src\flutter\prebuilts\windows-x64\dart-sdk
```

## 3. Use it

### `reelx render` (an engine folder)

The CLI uses a local engine when it finds an engine folder: `REELX_ENGINE_FOLDER`, else `~/.reelx/engine` (`%USERPROFILE%\.reelx\engine` on Windows). The folder must hold `out/host_debug/flutter_tester` (`flutter_tester.exe` on Windows), its `flutter_patched_sdk`, `icudtl.dat`, `gen/` and a `dart-sdk`. The checkout's `engine/src` is already such a folder once `out/host_debug` exists:

```sh
# Apple silicon or an --unoptimized build: name the folder host_debug.
ln -s host_debug_arm64 engine/src/out/host_debug       # or host_debug_unopt
export REELX_ENGINE_FOLDER="$PWD/engine/src"
reelx render
```

What the CLI does with it:

- macOS: renders with Impeller on Metal.
- Linux: renders with Skia (Impeller's shaders stall on software GL); `REELX_ENGINE_IMPELLER=1` asks for Impeller on a machine with a real GPU.
- Windows: renders with Skia.

Without the variable and without `~/.reelx/engine`, renders use the stock engine as before. Checked on Linux: a render through the engine folder ran the folder's tester and gave an MP4 byte-identical to the stock render.

Once an engine release exists (`engine-3.47.9-ext.<n>`), the editor's `tool/release/engine/fetch_engine.sh --engine-folder <tag> ~/.reelx/engine` downloads and checks the same folder instead of building it (set `REELX_ENGINE_REPO=nploi/flutter`).

### `flutter test` while working on Reelx

Pass the engine to any Flutter command:

```sh
E=/path/to/flutter-engine/engine/src
flutter test --local-engine-src-path=$E --local-engine=host_debug --local-engine-host=host_debug
#   + --enable-impeller to test Impeller (Metal on macOS, Vulkan or OpenGL ES on Linux)
```

Reelx's gated export tests run the same way (`--dart-define=REELX_RUN_EXPORT_TESTS=true`).

### Inside an app build

The app is built with the stock SDK, then its engine is replaced. The release lanes will do this from an engine release (macOS today on reelx_editor's `macos-engine-lane`; Linux, Windows, Android and iOS are in progress). By hand:

| OS | After | Replace |
|---|---|---|
| macOS | `flutter build macos --release` | `build/macos/Build/Products/Release*/<App>.app/Contents/Frameworks/FlutterMacOS.framework` with the universal release framework, then sign the app again |
| Linux | `flutter build linux --release` | `build/linux/x64/release/bundle/lib/libflutter_linux_gtk.so` with `out/host_release/libflutter_linux_gtk.so` |
| Windows | `flutter build windows --release` | `build\windows\x64\runner\Release\flutter_windows.dll` with `out\host_release\flutter_windows.dll` |

Status: the macOS swap is covered by the lane's tests but not yet run on a Mac; the Linux and Windows swaps by hand are not yet checked. An app built with another Flutter version than the engine's will not start.

## 4. Check that it is the patched engine

Exported functions (seven, all starting with `Reelx`):

```sh
nm -D --defined-only engine/src/out/host_debug/flutter_tester | grep Reelx             # Linux
nm -gU engine/src/out/host_debug_arm64/flutter_tester | grep Reelx                     # macOS
# Windows: engine\src\flutter\buildtools\windows-x64\clang\bin\llvm-readobj.exe --coff-exports flutter_windows.dll
```

From Dart, in a test run on the engine:

```dart
import 'package:reelx/reelx.dart';
import 'package:reelx_export/reelx_export.dart';

print(ReelxEngine.current.infoLine); // REELX_ENGINE {"patchSet":...,"features":{...}}
print(ShaderBytes.isSupported);      // true
```

Every export also logs one `REELX_ENGINE {...}` line: `{"stock":true,...}` on the stock engine.

## 5. Turning it off

- Renders: unset `REELX_ENGINE_FOLDER` and remove `~/.reelx/engine`.
- One export: `ExportConfig(engineExtensions: EngineExtensions.off)`, or `EngineExtensions.parse('without:fdHandoff')` to drop single features. The engine's previous switches come back after the export.

## 6. Releasing an engine

A release is a GitHub release of this repository whose tag names the Flutter version and the patch set: `engine-3.47.9-ext.1`. Pushing the tag starts the "Engine release" workflow:

```sh
git tag engine-3.47.9-ext.1 <commit on engine-perf/3.47.9 or -wip>
git push origin engine-3.47.9-ext.1
```

1. **prepare** checks the tag, reads the stock Flutter's engine revision and content hash (what an app's `flutter --version` reports), refuses a tag whose engine revision differs, and creates a **draft** release.
2. **build** jobs, one per platform part on GitHub's runners: the Linux library, the macOS framework per CPU, the Windows DLL, and the engine folders (Linux x64, macOS arm64 and x64, Windows x64). Each writes the build identity (`reelx/tools/write_build_info.sh`, so `ReelxEngineInfo` reports the patch set), builds, packages (`reelx/tools/package_artifacts.py`) and uploads to the draft.
3. **macos-universal** merges the two CPUs' frameworks into the universal one.
4. **finalize** writes `manifest.json` and `SHA256SUMS` over every asset.

Nothing is public until you publish the draft: `gh release edit engine-3.47.9-ext.1 --repo nploi/flutter --draft=false`. Assets are never replaced; to rebuild, delete the draft and push the tag again. Android and iOS assets are not built yet.

Then pin the release in the editor (`tool/release/engine/ENGINE_VERSION`) in its own PR, and on a machine install the engine folder with `fetch_engine.sh --engine-folder`.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `Path "...Windows Kits\10\include\10.0.22621.0\um" ... does not exist` | Install Windows SDK 10.0.22621.0 (Visual Studio Installer → Individual components). |
| `/usr/bin/env: 'vpython3': No such file` on `git rebase`/`push` | The checkout's git hooks need depot_tools on `PATH`. |
| `flutter test --local-engine` can't find the Dart SDK | Link `dart-sdk` into the build folder (step 2). |
| Impeller tests hang or abort on a Linux machine without a GPU | Use Skia (the CLI's default on Linux) or OpenGL ES; Vulkan needs a real driver. |
| The app crashes at start after a swap | The engine and the SDK that built the app differ: both must be 3.47.9. |
| `ShaderBytes.isSupported` is false | The engine isn't the patched one, or on Android/Windows an older Reelx that looked in the wrong library (fixed in reelx `engineLibrary()`). |

CI builds and tests the patches on every push to `engine-perf/*-wip`: see the "Engine (macOS)", "Engine (Linux)" and "Engine (Windows)" workflows in this repository's Actions tab.
