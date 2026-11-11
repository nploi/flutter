# Engine performance patches

Patch set on top of Flutter `3.47.9` that speeds up drawing frames off-screen and reading them back (as a video export does). This branch, `engine-perf/3.47.9`, holds the patches already checked on every platform; `engine-perf/3.47.9-wip` holds every patch and the current version of this list.

Every patch is behind a feature flag that `EnginePerfSetFeature` can turn off at run time and that `EnginePerfInfo` reports.

## Upgrading to a new Flutter release

```sh
git fetch https://github.com/flutter/flutter.git --tags
git switch -c engine-perf/<new> engine-perf/3.47.9
git rebase --onto <new> 3.47.9
```

Then follow each patch's **Verify** steps, rebuild (`engine_perf/tools/build_engine.sh`), package (`engine_perf/tools/package_artifacts.sh`), and re-run the export benchmark before releasing `engine-<new>-engine_perf.<n>`.

## Entry format

Each patch has: **Id**, **Title**, **Why** (what an export pays for without it), **Files**, **Tests**, **Verify** (after a rebase), **Feature** (the `EnginePerfInfo.features` key).

---

## P0 — Engine info and feature switches

- **Why**: an app must know whether it runs on this engine and which features to use, and must be able to switch any of them off per export without a new release.
- **Files**: `engine/src/flutter/lib/ui/engine_perf/engine_perf_ffi.{h,cc}` (C API + flags + counters), `lib/ui/BUILD.gn` (sources, test), `common/exported{,_test}_symbols{,_mac}.sym` (`EnginePerf*` exported like `InternalFlutterGpu*`), `.gitignore` (generated `engine_perf_build_info.h`), `engine_perf/tools/build_engine.sh` (writes it).
- **Tests**: `lib/ui/engine_perf/engine_perf_ffi_unittests.cc` (`EnginePerfFfiTest.*` in `ui_unittests`); `nm -gU` shows `_EnginePerfInfo` and `_EnginePerfSetFeature` in `FlutterMacOS.framework` and `flutter_tester`.
- **Verify**: `ui_unittests --gtest_filter='EnginePerfFfiTest.*'`; `nm -gU out/host_debug_arm64/flutter_tester | grep EnginePerf`; `EnginePerfInfo()` returns `{"patchSet":...}`.
- **Feature**: none (adds `EnginePerfInfo`, `EnginePerfSetFeature`; `-2` means the feature isn't in this build)
- **Note**: patching `engine/src/flutter` changes the engine content hash, so this checkout can't serve as a Flutter SDK (it would look for artifacts that don't exist). Apps build with the stock SDK and get this engine by framework swap.

## P1 — Lazy mipmaps for snapshots

- **Why**: every `toImage` snapshot builds a full mip chain an export never reads.
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: `lazyMips`

## P2 — Pooled targets and one-copy readback off the IO thread

- **Why**: every snapshot allocates a new resolve texture, every readback a new buffer, and the readback copies the frame again on the IO thread.
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: `targetPool`, `oneCopyReadback`

## P3 — Exact (lossless) snapshots

- **Why**: on Apple GPU family 8+ snapshots are stored with lossy compression, so exported pixels are not exact.
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: `exactSnapshots`

## P4 — Native frame hand-off to a file descriptor

- **Why**: handing frames to FFmpeg from Dart runs on the UI thread in hundreds of small writes per frame.
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: `fdHandoff`

## P5 — BT.709 for native video textures

- **Why**: NV12 external textures are always converted with BT.601.
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: `bt709Textures`

## P6 — Metal by default in `flutter_tester` on macOS

- **Why**: `flutter test --enable-impeller` on macOS can't create a tester context (no `--impeller-backend`, no Vulkan), so headless renders fall back to Skia software.
- **Files**: `engine/src/flutter/shell/testing/tester_main.cc` (`UsesMetal`: no backend asked for → Metal while `testerMetal` is on; the feature is marked available in `main` when Metal is compiled in), `shell/testing/BUILD.gn` (`//flutter/lib/ui` for the switch).
- **Tests**: `engine_perf/tools/test/tester_metal_test.sh` (macOS: `--enable-impeller` with and without `--impeller-backend=metal` gets past "Could not create tester context"); the decision itself compiled and checked against `engine_perf_ffi` off-tree.
- **Verify**: after a rebase, `ENGINE_PERF_ENGINE_OUT=engine/src/out engine_perf/tools/test/tester_metal_test.sh`; `flutter test --enable-impeller` with the engine folder renders on Metal.
- **Feature**: `testerMetal`
