#!/usr/bin/env bash
# Runs every reelx/tools test script; exits non-zero if any fails.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
status=0
for t in "$here"/*_test.sh; do
  echo "== $(basename "$t")"
  "$t" || status=1
done
exit $status
