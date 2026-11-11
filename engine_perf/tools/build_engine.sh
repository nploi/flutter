#!/usr/bin/env bash
# Builds the patched engine on a Mac: universal release and profile
# FlutterMacOS.framework, and flutter_tester for local-engine tests.
#
# Usage (from anywhere inside this checkout, depot_tools on PATH):
#   engine_perf/tools/build_engine.sh [--sync] [--skip-tests] [--jobs N]
#
# Outputs under engine/src/out/:
#   host_{release,profile,debug}{,_arm64}/      per-cpu builds
#   mac_release_universal/FlutterMacOS.framework
#   mac_profile_universal/FlutterMacOS.framework
# Then run engine_perf/tools/package_artifacts.py assets --platform macos --out-dir engine/src/out ...
set -euo pipefail

die() { echo "build_engine: $*" >&2; exit 1; }

sync=0 tests=1 jobs=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sync) sync=1; shift ;;
    --skip-tests) tests=0; shift ;;
    --jobs) jobs="-j $2"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done

[[ "$(uname -s)" == Darwin ]] || die "the macOS engine builds on a Mac"
command -v gclient >/dev/null || die "depot_tools (gclient) is not on PATH"
command -v ninja >/dev/null || die "ninja (from depot_tools) is not on PATH"
# Xcode 26 ships without the Metal compiler; ANGLE's and Impeller's shaders
# need it, and the build would otherwise stop well into the first config.
xcrun -sdk macosx metal --version >/dev/null 2>&1 ||
  die "no Metal toolchain: run xcodebuild -downloadComponent MetalToolchain"

root="$(git rev-parse --show-toplevel)"
src="$root/engine/src"
cd "$root"

# The monorepo is its own gclient solution ("." in standard.gclient); the
# url only matters for a fresh checkout, so this checkout is used as is.
if [[ ! -f .gclient ]]; then
  cp engine/scripts/standard.gclient .gclient
fi
if [[ $sync -eq 1 || ! -d "$src/flutter/third_party/dart" ]]; then
  gclient sync -D
fi

# The engine's LLVM can't link against the macOS 27 SDK (Xcode 27): its .tbd
# files list arm64e.x1. A macOS 26 SDK (the Command Line Tools keep one) works.
if compgen -G "$src/flutter/prebuilts/SDKs/MacOSX27*.sdk" >/dev/null; then
  die "the sync linked the macOS 27 SDK, which this engine can't link with;" \
    "see engine_perf/ENGINE_SETUP.md › Troubleshooting (link a MacOSX26 SDK instead)"
fi

# Identity reported by EnginePerfInfo (lib/ui/engine_perf/engine_perf_ffi.cc).
"$root/engine_perf/tools/write_build_info.sh" "${ENGINE_PERF_PATCH_SET:-dev}"

gn="$src/flutter/tools/gn"
# build <out-dir-name> <gn flags...>: configure, then build ${targets[@]}.
build() {
  local out="$1"; shift
  "$gn" "$@"
  # shellcheck disable=SC2086
  ninja -C "$src/out/$out" $jobs "${targets[@]}"
}

for mode in release profile; do
  targets=(flutter/shell/platform/darwin/macos:flutter_framework)
  build "host_$mode" --runtime-mode "$mode" --mac-cpu x64
  build "host_${mode}_arm64" --runtime-mode "$mode" --mac-cpu arm64
  python3 "$src/flutter/sky/tools/create_macos_framework.py" \
    --dst "$src/out/mac_${mode}_universal" \
    --x64-out-dir "$src/out/host_$mode" \
    --arm64-out-dir "$src/out/host_${mode}_arm64" \
    --dsym --strip
done

# flutter_tester (and its patched SDK) for both cpus.
targets=(flutter/shell/testing:testing flutter/lib/snapshot:kernel_platform_files
  flutter:unittests)
build host_debug --runtime-mode debug --mac-cpu x64
build host_debug_arm64 --runtime-mode debug --mac-cpu arm64

if [[ $tests -eq 1 ]]; then
  out="$src/out/host_debug_arm64"
  [[ "$(uname -m)" == arm64 ]] || out="$src/out/host_debug"
  "$out/ui_unittests" --gtest_filter='EnginePerf*:ImageEncoding*'
  "$out/impeller_unittests" --gtest_filter='*EnginePerf*:*Snapshot*'
  ENGINE_PERF_ENGINE_OUT="$src/out" "$root/engine_perf/tools/test/run_all.sh"
fi

echo "built: $src/out"
