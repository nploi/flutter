# Reelx engine patches

Private patch set on top of Flutter `3.47.7` (engine revision `deb287481e3ce9468f3434937ced4240a70539ca`) for Reelx's macOS export path. One commit per patch on branch `reelx/3.47.7`, in the order below. Nothing here is sent upstream.

Every patch is behind a feature flag that `ReelxSetFeature` can turn off at run time and that `ReelxEngineInfo` reports.

## Upgrading to a new Flutter release

```sh
git fetch https://github.com/flutter/flutter.git --tags
git switch -c reelx/<new> reelx/3.47.7
git rebase --onto <new> 3.47.7
```

Then follow each patch's **Verify** steps, rebuild (`reelx/tools/build_engine.sh`), package (`reelx/tools/package_artifacts.sh`), and re-run the export benchmark before releasing `engine-<new>-reelx.<n>`.

## Entry format

Each patch has: **Id**, **Title**, **Why** (what an export pays for without it), **Files**, **Tests**, **Verify** (after a rebase), **Feature** (the `ReelxEngineInfo.features` key).

---

## P0 — Engine info and feature switches

- **Why**: Reelx must know whether it runs on this engine and which features to use, and must be able to switch any of them off per export without a new release.
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: none (adds `ReelxEngineInfo`, `ReelxSetFeature`)

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
- **Files**: _pending_
- **Tests**: _pending_
- **Verify**: _pending_
- **Feature**: `testerMetal`
