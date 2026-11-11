#!/usr/bin/env bash
# Checks that the engine-perf C functions are exported, so dart:ffi finds them with
# DynamicLibrary.process(): from the macOS framework and from flutter_tester.
#
# Needs a build: ENGINE_PERF_ENGINE_OUT=<engine/src/out> (build_engine.sh sets it).
# Off macOS there is nothing to check and the script says so.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/lib.sh"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "skipped: the macOS binaries only exist on a Mac"
  exit 0
fi
out="${ENGINE_PERF_ENGINE_OUT:?set ENGINE_PERF_ENGINE_OUT to engine/src/out}"
cpu="$([[ "$(uname -m)" == arm64 ]] && echo _arm64 || true)"

exported() {
  nm -gU "$1" | awk '{print $3}'
}

check_binary() {
  local bin="$1" syms
  assert_file "$bin"
  syms="$(exported "$bin")"
  local sym
  for sym in "${engine_perf_symbols[@]}"; do
    assert_contains "$syms" "_$sym"
  done
}

# Every C function the patches add (P0, P4, P7).
engine_perf_symbols=(
  EnginePerfInfo
  EnginePerfSetFeature
  EnginePerfImageReadbackToFd
  EnginePerfRepeatLastToFd
  EnginePerfReleaseFd
  EnginePerfRegisterShaderBytes
  EnginePerfUnregisterShaderBytes
)

test_framework_exports_engine_perf_symbols() {
  check_binary "$out/mac_release_universal/FlutterMacOS.framework/FlutterMacOS"
  check_binary "$out/mac_profile_universal/FlutterMacOS.framework/FlutterMacOS"
}

test_tester_exports_engine_perf_symbols() {
  check_binary "$out/host_debug$cpu/flutter_tester"
}

run_tests
