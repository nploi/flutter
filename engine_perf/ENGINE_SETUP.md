# Using the patched engine on a machine

How to build this fork's Flutter engine and use it on your own computer or any other one: for `flutter test` and inside an app build. What each patch does is in [ENGINE_PERF_PATCHES.md](../ENGINE_PERF_PATCHES.md).

Everything here keeps the normal Flutter SDK. The patched engine is used next to it, and removing it brings back the stock engine. An app finds the engine's extra C functions at run time (`dart:ffi`); with a stock engine it keeps its default paths.

## What you need

| | macOS | Linux | Windows |
|---|---|---|---|
| Flutter SDK | **3.47.9** exactly (the engine must match the SDK) | same | same |
| Build tools | Xcode (command line tools selected) | `python3`, `git`, `curl`, `pkg-config`, `libgtk-3-dev` (for the app library) | Visual Studio 2022 with "Desktop development with C++" and **Windows SDK 10.0.22621.0** (the engine asks for this version) |
| depot_tools | [Chromium's depot_tools](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html) on `PATH` | same | same, and `DEPOT_TOOLS_WIN_TOOLCHAIN=0` |
| Disk | ~40 GB for the checkout, 10–20 GB per build folder | same | same |
| Time | first sync 20–40 min; a build 30 min–3 h depending on the machine | same | same |

## 1. Get the source and sync

```sh
git clone --branch engine-perf/3.47.9-wip https://github.com/<owner>/flutter.git flutter-engine
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
engine_perf/tools/build_engine.sh            # add --sync to sync first
```

**Any OS, the tester only** (what `flutter test` needs):

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
| macOS | `engine_perf/tools/build_engine.sh` | (all) | `out/mac_release_universal/FlutterMacOS.framework` |
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

### An engine folder

`flutter test --local-engine` needs only an engine folder: `out/host_debug/flutter_tester` (`flutter_tester.exe` on Windows), its `flutter_patched_sdk`, `icudtl.dat`, `gen/` and a `dart-sdk`. The checkout's `engine/src` is already such a folder once `out/host_debug` exists; CI and releases ship the same layout (`engine-folder-<os>-<cpu>.zip`):

```sh
# Apple silicon or an --unoptimized build: name the folder host_debug.
ln -s host_debug_arm64 engine/src/out/host_debug       # or host_debug_unopt
```

### `flutter test`

Pass the engine to any Flutter command:

```sh
E=/path/to/flutter-engine/engine/src
flutter test --local-engine-src-path=$E --local-engine=host_debug --local-engine-host=host_debug
#   + --enable-impeller to test Impeller (Metal on macOS, Vulkan or OpenGL ES on Linux)
```

### Inside an app build

The app is built with the stock SDK, then its engine is replaced:

| OS | After | Replace |
|---|---|---|
| macOS | `flutter build macos --release` | `build/macos/Build/Products/Release*/<App>.app/Contents/Frameworks/FlutterMacOS.framework` with the universal release framework, then sign the app again |
| Linux | `flutter build linux --release` | `build/linux/x64/release/bundle/lib/libflutter_linux_gtk.so` with `out/host_release/libflutter_linux_gtk.so` |
| Windows | `flutter build windows --release` | `build\windows\x64\runner\Release\flutter_windows.dll` with `out\host_release\flutter_windows.dll` |

Status: these swaps are not yet checked by hand. An app built with another Flutter version than the engine's will not start.

### On a phone (Android and iOS)

Build the app with the stock Flutter 3.47.9 in the engine's mode (release or profile), then put the engine in. Neither way is checked on a phone yet.

**Android**: replace the library inside the APK, stored uncompressed and page-aligned as Android loads it, and sign the APK again:

```sh
flutter build apk --release --target-platform android-arm64
unzip libflutter-release-android-arm64.zip           # lib/arm64-v8a/libflutter.so
cp build/app/outputs/flutter-apk/app-release.apk app.apk
zip -0 app.apk lib/arm64-v8a/libflutter.so           # replaces the stock one
BT=$ANDROID_HOME/build-tools/<version>               # 35 or later for -P 16
$BT/zipalign -f -P 16 4 app.apk app-aligned.apk
$BT/apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android --out app-engine.apk app-aligned.apk
adb install -r app-engine.apk
```

Impeller picks Vulkan where the phone has it. To run the OpenGL ES paths there, add to `<application>` in `android/app/src/main/AndroidManifest.xml`:

```xml
<meta-data android:name="io.flutter.embedding.android.ImpellerBackend" android:value="opengles" />
```

**iOS**: put the engine in the SDK's cache before building, so Xcode signs the app as usual:

```sh
cache="$(dirname "$(which flutter)")/cache/artifacts/engine/ios-release"   # ios-profile for profile
mv "$cache/Flutter.xcframework" "$cache/Flutter.xcframework.stock"
unzip -q Flutter-release.xcframework.zip -d "$cache"
flutter run --release                                 # or flutter build ipa
```

The engine has only the device slice: building that mode for a simulator fails until the stock one is back (`rm -rf "$cache/Flutter.xcframework" && mv "$cache/Flutter.xcframework.stock" "$cache/Flutter.xcframework"`).

### Builds from CI

The "Engine" workflow (`.github/workflows/engine.yml`) builds every push to `engine-perf/*-wip` and every pull request into it, and keeps the builds 14 days as run artifacts, one zip each:

| Artifact | Contents |
|---|---|
| `engine-folder-linux-x64`, `engine-folder-macos-arm64`, `engine-folder-windows-x64` | the engine folder (`out/host_debug`, from an unoptimized build) for `flutter test` |
| `libflutter-release-android-arm64`, `libflutter-profile-android-arm64` | `lib/arm64-v8a/libflutter.so` |
| `Flutter-release.xcframework`, `Flutter-profile.xcframework` | `Flutter.xcframework` for iOS devices |

```sh
repo=<owner>/flutter
run=$(gh run list --repo $repo --workflow engine.yml --branch engine-perf/3.47.9-wip \
  --limit 1 --json databaseId --jq '.[0].databaseId')     # a pull request's run: its branch
gh run download "$run" --repo $repo --name libflutter-release-android-arm64
```

To build only some platforms or phone modes, run it by hand: `gh workflow run engine.yml --repo $repo --ref <branch> -f linux=false -f macos=false -f windows=false -f mobile_modes=release` (or "Run workflow" in the Actions tab).

## 4. Check that it is the patched engine

Exported functions (seven, all starting with `EnginePerf`):

```sh
nm -D --defined-only engine/src/out/host_debug/flutter_tester | grep EnginePerf             # Linux
nm -gU engine/src/out/host_debug_arm64/flutter_tester | grep EnginePerf                     # macOS
# Windows: engine\src\flutter\buildtools\windows-x64\clang\bin\llvm-readobj.exe --coff-exports flutter_windows.dll
engine/src/flutter/buildtools/linux-x64/clang/bin/llvm-nm -D --defined-only lib/arm64-v8a/libflutter.so | grep EnginePerf   # Android
nm -gU Flutter.xcframework/ios-arm64/Flutter.framework/Flutter | grep EnginePerf              # iOS
```

From Dart, in a test run on the engine (a stock engine has no such symbol):

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';

final info = DynamicLibrary.process()
    .lookupFunction<Pointer<Utf8> Function(), Pointer<Utf8> Function()>('EnginePerfInfo');
print(info().toDartString()); // {"patchSet":...,"features":{...}}
```

## 5. Turning it off

- Use the stock engine again: drop `--local-engine`, or put the stock engine library back into the app.
- At run time: `EnginePerfSetFeature(name, 0)` switches a single feature off; `EnginePerfInfo` lists them.

## 6. Releasing an engine

A release is a GitHub release of this repository whose tag names the Flutter version and the patch set: `engine-3.47.9-ext.1`. Pushing the tag starts the "Engine release" workflow:

```sh
git tag engine-3.47.9-ext.1 <commit on engine-perf/3.47.9 or -wip>
git push origin engine-3.47.9-ext.1
```

1. **prepare** checks the tag, reads the stock Flutter's engine revision and content hash (what an app's `flutter --version` reports), refuses a tag whose engine revision differs, and creates a **draft** release.
2. **build** jobs, one per platform part on GitHub's runners: the Linux library, the macOS framework per CPU, the Windows DLL, and the engine folders (Linux x64, macOS arm64 and x64, Windows x64). Each writes the build identity (`engine_perf/tools/write_build_info.sh`, so `EnginePerfInfo` reports the patch set), builds, packages (`engine_perf/tools/package_artifacts.py`) and uploads to the draft.
3. **macos-universal** merges the two CPUs' frameworks into the universal one.
4. **finalize** writes `manifest.json` and `SHA256SUMS` over every asset.

Nothing is public until you publish the draft: `gh release edit engine-3.47.9-ext.1 --repo <owner>/flutter --draft=false`. Assets are never replaced; to rebuild, delete the draft and push the tag again. Android and iOS engines are not release assets yet; the "Engine" workflow's run artifacts have them (see "Builds from CI").

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `Path "...Windows Kits\10\include\10.0.22621.0\um" ... does not exist` | Install Windows SDK 10.0.22621.0 (Visual Studio Installer → Individual components). |
| Xcode 27: `ld64.lld: error: could not load TAPI file at .../MacOSX27.2.sdk/usr/lib/libSystem.tbd: malformed file` (`unknown architecture`), and before that `'stack_protector_ignore' attribute ignored` in ANGLE | The engine's LLVM can't read the macOS 27 SDK (its `.tbd` files list `arm64e.x1`, and its `os_log` macros warn under `-fstack-protector-all`). Build with a macOS 26 SDK: the Command Line Tools keep one. Replace the sync's link, then run `gn` again: `cd engine/src/flutter/prebuilts/SDKs && rm MacOSX27*.sdk && ln -s /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk MacOSX26.5.sdk` (`gclient sync` puts the Xcode SDK back). |
| Xcode 27: `cannot execute tool 'metal' due to missing Metal Toolchain` | `xcodebuild -downloadComponent MetalToolchain` (about 840 MB). |
| A Mac swaps hard while linking (16–24 GB) | Each LTO link takes a few GB; link the many test binaries at `-j 2` (`ninja -j 2 ...` after the objects are built). |
| `/usr/bin/env: 'vpython3': No such file` on `git rebase`/`push` | The checkout's git hooks need depot_tools on `PATH`. |
| `flutter test --local-engine` can't find the Dart SDK | Link `dart-sdk` into the build folder (step 2). |
| Impeller tests hang or abort on a Linux machine without a GPU | Use Skia or OpenGL ES; Vulkan needs a real driver. |
| The app crashes at start after a swap | The engine and the SDK that built the app differ: both must be 3.47.9. |
| `EnginePerfInfo` is not found | The engine isn't the patched one, or the lookup used the wrong library: `DynamicLibrary.process()` on macOS, iOS and Linux, `DynamicLibrary.open('libflutter.so')` on Android, `flutter_windows.dll` on Windows. |

CI builds and tests the patches on every push to `engine-perf/*-wip` and every pull request into it: the "Engine" workflow in this repository's Actions tab, one job per platform (see "Builds from CI").
