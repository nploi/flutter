# Reelx engine patches

Patch set on top of Flutter `3.47.9` (engine revision `89c39edfdfae2949f6806e4a315c0c08522238b7`, content hash `9e025265bef30b838c0c1b68cad19decf97048f2`) for Reelx's export path on every platform the editor ships. One commit per patch on branch `engine-perf/3.47.9`, in the order below; patches not yet built and tested on every platform wait on `engine-perf/3.47.9-wip`.

Moved from 3.47.7 on 2026-11-10 to match the Flutter version the editor builds with (a swapped engine must match the app's Flutter exactly); the rebase had no conflicts. `engine-perf/3.47.7` and `-wip` stay as they were. Status lines dated before 2026-11-10 were measured on 3.47.7. A patch that is useful beyond Reelx may be offered upstream.

To build the engine and use it on a machine (CLI renders, `flutter test`, app builds), see [reelx/ENGINE_SETUP.md](reelx/ENGINE_SETUP.md).

Every patch is behind a feature flag that `ReelxSetFeature` can turn off at run time and that `ReelxEngineInfo` reports.

## Upgrading to a new Flutter release

```sh
git fetch https://github.com/flutter/flutter.git --tags
git switch -c engine-perf/<new> engine-perf/3.47.9
git rebase --onto <new> 3.47.9
```

Then follow each patch's **Verify** steps, rebuild (`reelx/tools/build_engine.sh`), package (`reelx/tools/package_artifacts.py`; the `Engine release` workflow does all platforms), and re-run the export benchmark before releasing `engine-<new>-ext.<n>`.

## Entry format

Each patch has: **Id**, **Title**, **Why** (what an export pays for without it), **Files**, **Tests**, **Verify** (after a rebase), **Feature** (the `ReelxEngineInfo.features` key).

---

## P0 — Engine info and feature switches

- **Why**: Reelx must know whether it runs on this engine and which features to use, and must be able to switch any of them off per export without a new release.
- **Files**: `engine/src/flutter/lib/ui/reelx/reelx_ffi.{h,cc}` (C API + flags + counters), `lib/ui/BUILD.gn` (sources, test), `common/exported{,_test}_symbols{,_mac}.sym` (`Reelx*` exported like `InternalFlutterGpu*`), `shell/platform/android/android_exports.lst` (`Reelx*`: `libflutter.so` exports only what this version script lists; added 2026-11-10 for the cross-platform builds), `.gitignore` (generated `reelx_build_info.h`), `reelx/tools/build_engine.sh` (writes it).
- **Tests**: `lib/ui/reelx/reelx_ffi_unittests.cc` (`ReelxFfiTest.*` in `ui_unittests`); `nm -gU` shows `_ReelxEngineInfo` and `_ReelxSetFeature` in `FlutterMacOS.framework` and `flutter_tester`.
- **Verify**: `ui_unittests --gtest_filter='ReelxFfiTest.*'`; `nm -gU out/host_debug_arm64/flutter_tester | grep Reelx`; the editor's export log shows `REELX_ENGINE {"patchSet":...}`.
- **Feature**: none (adds `ReelxEngineInfo`, `ReelxSetFeature`; `-2` means the feature isn't in this build)
- **Note**: patching `engine/src/flutter` changes the engine content hash, so this checkout can't serve as a Flutter SDK (it would look for artifacts that don't exist). Apps build with the stock SDK and get this engine by framework swap.

## P1 — Lazy mipmaps for snapshots

- **Status**: on `engine-perf/3.47.7-wip`; built and tested on Linux (host_debug_unopt, 2026-11-09): `Play/AiksTest.Reelx*` pass on OpenGLES (Vulkan can't start on that machine, Metal needs the Mac).
- **Why**: every `toImage` snapshot builds a full mip chain an export never reads.
- **Files**: `shell/common/snapshot_controller_impeller.cc` (`generate_mips` off while `lazyMips` is on, the texture marked `SetWantsLazyMipmaps`), `impeller/core/texture.{h,cc}` (the mark and the kept copy), `impeller/display_list/lazy_mipmaps.{h,cc}` (`TextureForSampling`: a mipmapped copy made once by blit copy + `GenerateMipmap`, only for samplers with `MipFilter::kLinear`), `impeller/display_list/dl_dispatcher.cc` (`drawImageRect`, and `drawImage` through it) and `paint.cc` (image shaders) call it.
- **Tests**: `impeller/display_list/reelx_snapshot_unittests.cc` (`AiksTest.ReelxSnapshotWithoutMipsHasOneLevel`, `ReelxLazyMipmapsOnlyForMipmapSampling`, `ReelxLazyMipmapsNotForOneLevelSizes`).
- **Verify**: `impeller_unittests --gtest_filter='*Reelx*'`; a scaled-down `Image` of a snapshot with `FilterQuality.medium` looks as on the stock engine; the export benchmark's `toImage` stage drops.
- **Feature**: `lazyMips`

## P2 — Pooled targets and one-copy readback off the IO thread

- **Status**: on `engine-perf/3.47.7-wip`; built and tested on Linux (host_debug_unopt, 2026-11-09): the P2a tests pass on OpenGLES, `ImageEncodingReelxTest.*` pass, and a Reelx export with `oneCopyReadback` on is byte-identical to the stock engine's. Pooled readback buffers (P2b's buffer half) are not written: the copy they would save is gone with the zero-copy hand-over; the stats keys stay for them.
- **Why**: every snapshot allocates a new resolve texture, every readback a new buffer, and the readback copies the frame again on the IO thread.
- **Files**: P2a (`targetPool`) `impeller/display_list/dl_dispatcher.{h,cc}` (`DisplayListToTexture(..., pooled_target, pool_hit)`: one-level targets from a pool of three on the `ContentContext`, reused once nothing else holds them, lazy-mip state cleared), `impeller/entity/contents/content_context.h` (`GetSnapshotTargetPool`), `shell/common/snapshot_controller_impeller.cc` (pooled while `lazyMips` and `targetPool` are on; counts `targetPoolHits`/`Misses`). P2b/P2c (`oneCopyReadback`) `lib/ui/painting/image_encoding.cc`: `rawUnmodified` hands Dart the downloaded buffer itself (`SkData::MakeWithProc` holding the raster image), other raw formats swizzle in one pass straight into the returned data (`SkPixmap::readPixels`), and the copy runs on the concurrent worker runner instead of the IO thread.
- **Fix (2026-11-10)**: a reused target keeps the framebuffer its backend cached for it (Vulkan `TextureSourceVK` frame data, GLES `TextureGLES` cached FBO), and that cache also holds the previous render target's MSAA and stencil textures, which are gone. On Vulkan (SwiftShader, `flutter test --enable-impeller`) the second snapshot aborted on a freed image view; on GLES the old stencil stayed attached. `TargetFromPool` now calls `Texture::ForgetRenderTargetCaches()` (a new virtual, no-op by default; `TextureVK` clears its frame data, `TextureGLES` drops its FBO) on reuse, so a reused target costs one new framebuffer, as a stock snapshot does. Metal caches nothing on the texture. Files: `impeller/core/texture.h`, `impeller/renderer/backend/vulkan/texture{,_source}_vk.{h,cc}`, `impeller/renderer/backend/gles/texture_gles.{h,cc}`.
- **Tests**: `AiksTest.ReelxPooledTargetIsReusedOnceReleased`, `ReelxMipmappedTargetsAreNotPooled`, `ReelxReusedTargetDrawsLikeAFreshOne`, `RenderPassVK.ForgottenRenderTargetCachesMakeANewFramebuffer` (mock Vulkan), `TextureGLESTest.ForgetRenderTargetCachesDropsTheCachedFBO` (`impeller_unittests`); `ImageEncodingReelxTest.OneCopyKeepsTheBytes`, `OneCopyUnmodifiedIsTheImagesOwnMemory` (`ui_unittests`).
- **Verify**: the tests above; Reelx's gated determinism suite byte-identical with `oneCopyReadback` on and off; the export log's pool hit rate near 100 % after the first frames.
- **Feature**: `targetPool`, `oneCopyReadback`

## P3 — Exact (lossless) snapshots

- **Status**: on `engine-perf/3.47.7-wip`; built and tested on Linux (host_debug_unopt, 2026-11-09): `ReelxLosslessSnapshotTarget` passes on OpenGLES; the lossy case only exists on Apple GPUs.
- **Why**: on Apple GPU family 8+ snapshots are stored with lossy compression, so exported pixels are not exact.
- **Files**: `impeller/display_list/dl_dispatcher.{h,cc}` (`DisplayListToTexture(..., lossless_target)`: the MSAA resolve texture is made lossless and passed as `existing_color_resolve_texture`, so `CreateOffscreenMSAA` and the render target cache are unchanged), `shell/common/snapshot_controller_impeller.cc` (passes `exactSnapshots`).
- **Tests**: `AiksTest.ReelxLosslessSnapshotTarget` in `reelx_snapshot_unittests.cc`.
- **Verify**: `impeller_unittests --gtest_filter='*Reelx*'`; on an M2 or later, Reelx's direct yuv420p self-test passes (it fails on lossy targets).
- **Feature**: `exactSnapshots`

## P4 — Native frame hand-off to a file descriptor

- **Status**: on `engine-perf/3.47.7-wip`; built and tested on Linux (host_debug_unopt, 2026-11-09): `ReelxFfiTest.*` pass, `flutter_tester` exports the three functions, and a Reelx export through `ReelxImageReadbackToFd` (Skia path) is byte-identical to the stock engine's; the writer logic was also checked off-tree under ThreadSanitizer.
- **Why**: handing frames to FFmpeg from Dart runs on the UI thread in hundreds of small writes per frame.
- **Platforms**: POSIX only (macOS, Linux, Android, iOS). On Windows the three C functions are built as stubs (`ReelxImageReadbackToFd`/`ReelxRepeatLastToFd` return `-1`, `ReelxReleaseFd` `0`) and `fdHandoff` is never reported available; Reelx writes to FFmpeg's stdin there. `reelx_handoff_unittests.cc` is left out of Windows builds. Checked on Linux by a `-D_WIN32 -fms-extensions` syntax pass; a real Windows build is still to come.
- **Files**: `engine/src/flutter/lib/ui/reelx/reelx_handoff.cc` (`ReelxImageReadbackToFd`, `ReelxRepeatLastToFd`, `ReelxReleaseFd`: the readback is `toByteData`'s own, `ConvertImageToRaster` then `EncodeImage`, so the bytes are the same; one writer thread per descriptor writes in call order, keeps the last frame, posts `[token, status, bytes, digest]` with a 64-bit digest of its own), `reelx_ffi.h` (declarations), `lib/ui/BUILD.gn`. Exported by the existing `Reelx*` symbol patterns.
- **Tests**: `ReelxFfiTest.Handoff*` in `ui_unittests` (refusals before any image); the readback path is Reelx's `frame_handoff_export_test.dart` against the real engine.
- **Verify**: `ui_unittests --gtest_filter='ReelxFfiTest.*'`; Reelx's gated export suite with `fdHandoff` on and off gives byte-identical files.
- **Feature**: `fdHandoff`

## P5 — BT.709 for native video textures

- **Status**: on `engine-perf/3.47.7-wip`; built and tested on Linux (host_debug_unopt, 2026-11-09) for the Impeller part (`EntityTest.YUV*` pass); the macOS `.mm` files and the Metal shader path need the Mac.
- **Why**: NV12 external textures are always converted with BT.601.
- **Files**: `impeller/geometry/color.h` (`kBT709LimitedRange`, `kBT709FullRange` after the existing values), `impeller/entity/contents/filters/yuv_to_rgb_filter_contents.{h,cc}` (BT.709 matrices; `MatrixFor`, `IsLimitedRange`; the shader is unchanged: it gets 0 for limited range), `shell/platform/embedder/embedder.h` and `embedder_external_texture_metal.mm` (the new spaces, Impeller and Skia), `shell/platform/darwin/macos/framework/Source/FlutterExternalTexture.mm` (a buffer tagged ITU-R 709 gets BT.709 while `bt709Textures` is on, macOS 12+), `shell/platform/darwin/macos/BUILD.gn` (`//flutter/lib/ui`).
- **Tests**: `EntityTest.YUVToRGBMatricesInvertTheirSpaces`, `EntityTest.YUVColorSpaceRanges` in `impeller/entity/entity_unittests.cc` (the BT.709 matrices checked off-tree to 3e-6).
- **Verify**: `impeller_unittests --gtest_filter='EntityTest.YUV*'`; a BT.709 clip played through the macOS export decoder matches FFmpeg's colours.
- **Feature**: `bt709Textures`

## P6 — Metal by default in `flutter_tester` on macOS

- **Why**: `flutter test --enable-impeller` on macOS can't create a tester context (no `--impeller-backend`, no Vulkan), so headless renders fall back to Skia software.
- **Files**: `engine/src/flutter/shell/testing/tester_main.cc` (`UsesMetal`: no backend asked for → Metal while `testerMetal` is on; the feature is marked available in `main` when Metal is compiled in), `shell/testing/BUILD.gn` (`//flutter/lib/ui` for the switch).
- **Tests**: `reelx/tools/test/tester_metal_test.sh` (macOS: `--enable-impeller` with and without `--impeller-backend=metal` gets past "Could not create tester context"); the decision itself compiled and checked against `reelx_ffi` off-tree.
- **Verify**: after a rebase, `REELX_ENGINE_OUT=engine/src/out reelx/tools/test/tester_metal_test.sh`; a CLI render with the engine folder logs the engine and renders.
- **Feature**: `testerMetal`

## P7 — Fragment shaders from bytes

- **Status**: on `engine-perf/3.47.7-wip`; built and tested on Linux (host_debug_unopt, 2026-11-10).
- **Why**: a shader effect or transition otherwise has to ship as an asset in the app bundle; with bytes, effects can come from code or downloaded packs.
- **What**: `ReelxRegisterShaderBytes(key, bytes)` (FFI `Handle` arguments: a `String` and a `Uint8List`, so no `malloc` on the Dart side) keeps a copy of an impellerc bundle (`impellerc --iplr`, the file the flutter tool builds for a `.frag` in pubspec's `shaders`) under `key` for the whole process; `FragmentProgram.fromAsset(key)` finds it before the asset manager. `dart:ui` is unchanged, so the stock API loads it, and a stock engine simply lacks the symbol. Returns `0`, `-1` bad argument, `-3` not a shader bundle (checked with `RuntimeStage::DecodeRuntimeStages`); `ReelxUnregisterShaderBytes(key)` returns `0` or `-4`. `dart:ui` keeps the program it loaded for a key (`_shaderRegistry`) and encodes keys like asset keys (`Uri.encodeFull`), so use plain keys and a new key for changed bytes (a content hash). The bytes must hold a stage for the backend the app runs on (`--runtime-stage-metal` for macOS/iOS Impeller, `--sksl` for Skia, GLES/Vulkan for Linux/Android).
- **Files**: `lib/ui/reelx/reelx_shader_bytes.{h,cc}` (store + C API), `lib/ui/painting/fragment_program.cc` (`initFromAsset` asks the store first), `lib/ui/BUILD.gn`.
- **Tests**: `lib/ui/reelx/reelx_shader_bytes_unittests.cc` (`ReelxShaderBytesTest.*` in `ui_unittests`: round trip, decodes like the asset, junk and cut bundles refused, replace, unregister keeps live mappings). End to end (a Flutter test, outside the tree): register an impellerc bundle over FFI, `FragmentProgram.fromAsset`, draw, check pixels; passes with Skia and with `--enable-impeller` (Vulkan/SwiftShader), skipped on the stock engine.
- **Verify**: `ui_unittests --gtest_filter='ReelxShaderBytes*'`; `nm` shows `ReelxRegisterShaderBytes` / `ReelxUnregisterShaderBytes` in `flutter_tester` and `FlutterMacOS.framework`.
- **Feature**: none (the symbol's presence is the capability)
