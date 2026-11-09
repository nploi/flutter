# Reelx engine patches

Patch set on top of Flutter `3.47.7` (engine revision `deb287481e3ce9468f3434937ced4240a70539ca`) for Reelx's macOS export path. One commit per patch on branch `engine-perf/3.47.7`, in the order below; patches written without an engine build wait on `engine-perf/3.47.7-wip` until they are built and tested. A patch that is useful beyond Reelx may be offered upstream.

Every patch is behind a feature flag that `ReelxSetFeature` can turn off at run time and that `ReelxEngineInfo` reports.

## Upgrading to a new Flutter release

```sh
git fetch https://github.com/flutter/flutter.git --tags
git switch -c engine-perf/<new> engine-perf/3.47.7
git rebase --onto <new> 3.47.7
```

Then follow each patch's **Verify** steps, rebuild (`reelx/tools/build_engine.sh`), package (`reelx/tools/package_artifacts.sh`), and re-run the export benchmark before releasing `engine-<new>-ext.<n>`.

## Entry format

Each patch has: **Id**, **Title**, **Why** (what an export pays for without it), **Files**, **Tests**, **Verify** (after a rebase), **Feature** (the `ReelxEngineInfo.features` key).

---

## P0 — Engine info and feature switches

- **Why**: Reelx must know whether it runs on this engine and which features to use, and must be able to switch any of them off per export without a new release.
- **Files**: `engine/src/flutter/lib/ui/reelx/reelx_ffi.{h,cc}` (C API + flags + counters), `lib/ui/BUILD.gn` (sources, test), `common/exported{,_test}_symbols{,_mac}.sym` (`Reelx*` exported like `InternalFlutterGpu*`), `.gitignore` (generated `reelx_build_info.h`), `reelx/tools/build_engine.sh` (writes it).
- **Tests**: `lib/ui/reelx/reelx_ffi_unittests.cc` (`ReelxFfiTest.*` in `ui_unittests`); `nm -gU` shows `_ReelxEngineInfo` and `_ReelxSetFeature` in `FlutterMacOS.framework` and `flutter_tester`.
- **Verify**: `ui_unittests --gtest_filter='ReelxFfiTest.*'`; `nm -gU out/host_debug_arm64/flutter_tester | grep Reelx`; the editor's export log shows `REELX_ENGINE {"patchSet":...}`.
- **Feature**: none (adds `ReelxEngineInfo`, `ReelxSetFeature`; `-2` means the feature isn't in this build)
- **Note**: patching `engine/src/flutter` changes the engine content hash, so this checkout can't serve as a Flutter SDK (it would look for artifacts that don't exist). Apps build with the stock SDK and get this engine by framework swap.

## P1 — Lazy mipmaps for snapshots

- **Status**: written on `engine-perf/3.47.7-wip`, not yet built.
- **Why**: every `toImage` snapshot builds a full mip chain an export never reads.
- **Files**: `shell/common/snapshot_controller_impeller.cc` (`generate_mips` off while `lazyMips` is on, the texture marked `SetWantsLazyMipmaps`), `impeller/core/texture.{h,cc}` (the mark and the kept copy), `impeller/display_list/lazy_mipmaps.{h,cc}` (`TextureForSampling`: a mipmapped copy made once by blit copy + `GenerateMipmap`, only for samplers with `MipFilter::kLinear`), `impeller/display_list/dl_dispatcher.cc` (`drawImageRect`, and `drawImage` through it) and `paint.cc` (image shaders) call it.
- **Tests**: `impeller/display_list/reelx_snapshot_unittests.cc` (`AiksTest.ReelxSnapshotWithoutMipsHasOneLevel`, `ReelxLazyMipmapsOnlyForMipmapSampling`, `ReelxLazyMipmapsNotForOneLevelSizes`).
- **Verify**: `impeller_unittests --gtest_filter='*Reelx*'`; a scaled-down `Image` of a snapshot with `FilterQuality.medium` looks as on the stock engine; the export benchmark's `toImage` stage drops.
- **Feature**: `lazyMips`

## P2 — Pooled targets and one-copy readback off the IO thread

- **Status**: written on `engine-perf/3.47.7-wip`, not yet built. Pooled readback buffers (P2b's buffer half) are not written: the copy they would save is gone with the zero-copy hand-over; the stats keys stay for them.
- **Why**: every snapshot allocates a new resolve texture, every readback a new buffer, and the readback copies the frame again on the IO thread.
- **Files**: P2a (`targetPool`) `impeller/display_list/dl_dispatcher.{h,cc}` (`DisplayListToTexture(..., pooled_target, pool_hit)`: one-level targets from a pool of three on the `ContentContext`, reused once nothing else holds them, lazy-mip state cleared), `impeller/entity/contents/content_context.h` (`GetSnapshotTargetPool`), `shell/common/snapshot_controller_impeller.cc` (pooled while `lazyMips` and `targetPool` are on; counts `targetPoolHits`/`Misses`). P2b/P2c (`oneCopyReadback`) `lib/ui/painting/image_encoding.cc`: `rawUnmodified` hands Dart the downloaded buffer itself (`SkData::MakeWithProc` holding the raster image), other raw formats swizzle in one pass straight into the returned data (`SkPixmap::readPixels`), and the copy runs on the concurrent worker runner instead of the IO thread.
- **Tests**: `AiksTest.ReelxPooledTargetIsReusedOnceReleased`, `ReelxMipmappedTargetsAreNotPooled` (`impeller_unittests`); `ImageEncodingReelxTest.OneCopyKeepsTheBytes`, `OneCopyUnmodifiedIsTheImagesOwnMemory` (`ui_unittests`).
- **Verify**: the tests above; Reelx's gated determinism suite byte-identical with `oneCopyReadback` on and off; the export log's pool hit rate near 100 % after the first frames.
- **Feature**: `targetPool`, `oneCopyReadback`

## P3 — Exact (lossless) snapshots

- **Status**: written on `engine-perf/3.47.7-wip`, not yet built.
- **Why**: on Apple GPU family 8+ snapshots are stored with lossy compression, so exported pixels are not exact.
- **Files**: `impeller/display_list/dl_dispatcher.{h,cc}` (`DisplayListToTexture(..., lossless_target)`: the MSAA resolve texture is made lossless and passed as `existing_color_resolve_texture`, so `CreateOffscreenMSAA` and the render target cache are unchanged), `shell/common/snapshot_controller_impeller.cc` (passes `exactSnapshots`).
- **Tests**: `AiksTest.ReelxLosslessSnapshotTarget` in `reelx_snapshot_unittests.cc`.
- **Verify**: `impeller_unittests --gtest_filter='*Reelx*'`; on an M2 or later, Reelx's direct yuv420p self-test passes (it fails on lossy targets).
- **Feature**: `exactSnapshots`

## P4 — Native frame hand-off to a file descriptor

- **Status**: written on `engine-perf/3.47.7-wip`, not yet built; the writer's ordering, repeat, failure and stop logic compiled and checked off-tree (gcc -Werror, clang ThreadSanitizer) with stand-ins for `SkData` and `Dart_PostCObject`.
- **Why**: handing frames to FFmpeg from Dart runs on the UI thread in hundreds of small writes per frame.
- **Files**: `engine/src/flutter/lib/ui/reelx/reelx_handoff.cc` (`ReelxImageReadbackToFd`, `ReelxRepeatLastToFd`, `ReelxReleaseFd`: the readback is `toByteData`'s own, `ConvertImageToRaster` then `EncodeImage`, so the bytes are the same; one writer thread per descriptor writes in call order, keeps the last frame, posts `[token, status, bytes, digest]` with a 64-bit digest of its own), `reelx_ffi.h` (declarations), `lib/ui/BUILD.gn`. Exported by the existing `Reelx*` symbol patterns.
- **Tests**: `ReelxFfiTest.Handoff*` in `ui_unittests` (refusals before any image); the readback path is Reelx's `frame_handoff_export_test.dart` against the real engine.
- **Verify**: `ui_unittests --gtest_filter='ReelxFfiTest.*'`; Reelx's gated export suite with `fdHandoff` on and off gives byte-identical files.
- **Feature**: `fdHandoff`

## P5 — BT.709 for native video textures

- **Status**: written on `engine-perf/3.47.7-wip`, not yet built.
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
