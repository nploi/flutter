# Minimal test helpers for the engine_perf/tools scripts (plain bash, no framework).
# Usage: source this file, define functions named test_*, then call run_tests.

TESTS_RUN=0
TESTS_FAILED=0

fail() {
  echo "    FAIL: $*" >&2
  return 1
}

assert_eq() {
  [[ "$1" == "$2" ]] || fail "expected '$2', got '$1'${3:+ ($3)}"
}

assert_file() {
  [[ -e "$1" ]] || fail "missing $1"
}

assert_no_file() {
  [[ ! -e "$1" ]] || fail "unexpected $1"
}

assert_contains() {
  grep -qF -- "$2" <<<"$1" || fail "'$2' not found in output"
}

run_tests() {
  local name
  for name in $(declare -F | awk '{print $3}' | grep '^test_'); do
    TESTS_RUN=$((TESTS_RUN + 1))
    if ( set -e; "$name" ); then
      echo "ok   $name"
    else
      echo "FAIL $name"
      TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
  done
  echo "$TESTS_RUN tests, $TESTS_FAILED failed"
  [[ $TESTS_FAILED -eq 0 ]]
}
