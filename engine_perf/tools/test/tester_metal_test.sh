#!/usr/bin/env bash
# P6: on macOS, flutter_tester with --enable-impeller and no
# --impeller-backend opens a Metal context. The stock tester opens
# SwiftShader's Vulkan (the CPU) instead.
#
# Needs a build: ENGINE_PERF_ENGINE_OUT=<engine/src/out> (build_engine.sh sets it).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/lib.sh"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "skipped: Metal exists only on a Mac"
  exit 0
fi
out="${ENGINE_PERF_ENGINE_OUT:?set ENGINE_PERF_ENGINE_OUT to engine/src/out}"
cpu="$([[ "$(uname -m)" == arm64 ]] && echo _arm64 || true)"
host="$out/host_debug$cpu"

# A tester that got a context keeps running after the missing kernel fails to
# load, so it is stopped after a few seconds; its output so far is the result.
run_tester() {
  local log pid
  log="$(mktemp)"
  "$host/flutter_tester" --icu-data-file-path="$host/icudtl.dat" "$@" \
    /nonexistent/kernel.dill >"$log" 2>&1 &
  pid=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  cat "$log"
  rm -f "$log"
}

# With --verbose-logging the patched tester names its context. (A stock
# tester doesn't, and without a backend it renders with SwiftShader's Vulkan.)
assert_metal() {
  if grep -q "Could not create tester context" <<<"$1"; then
    fail "no tester context: $1"
  fi
  assert_contains "$1" "Tester context: Metal"
}

test_impeller_without_backend_opens_metal() {
  assert_file "$host/flutter_tester"
  assert_metal "$(run_tester --verbose-logging --enable-impeller)"
}

test_explicit_metal_still_works() {
  assert_file "$host/flutter_tester"
  assert_metal "$(run_tester --verbose-logging --enable-impeller \
    --impeller-backend=metal)"
}

run_tests
