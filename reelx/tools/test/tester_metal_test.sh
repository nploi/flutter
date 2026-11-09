#!/usr/bin/env bash
# P6: on macOS, flutter_tester with --enable-impeller and no
# --impeller-backend opens a Metal context. The stock tester stops with
# "Could not create tester context" (no Vulkan in a mac host build).
#
# Needs a build: REELX_ENGINE_OUT=<engine/src/out> (build_engine.sh sets it).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/lib.sh"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "skipped: Metal exists only on a Mac"
  exit 0
fi
out="${REELX_ENGINE_OUT:?set REELX_ENGINE_OUT to engine/src/out}"
cpu="$([[ "$(uname -m)" == arm64 ]] && echo _arm64 || true)"
host="$out/host_debug$cpu"

run_tester() {
  "$host/flutter_tester" --icu-data-file-path="$host/icudtl.dat" "$@" \
    /nonexistent/kernel.dill 2>&1 || true
}

test_impeller_without_backend_opens_metal() {
  local log
  log="$(run_tester --enable-impeller)"
  if grep -q "Could not create tester context" <<<"$log"; then
    fail "no tester context: $log"
  fi
}

test_explicit_metal_still_works() {
  local log
  log="$(run_tester --enable-impeller --impeller-backend=metal)"
  if grep -q "Could not create tester context" <<<"$log"; then
    fail "no tester context: $log"
  fi
}

run_tests
