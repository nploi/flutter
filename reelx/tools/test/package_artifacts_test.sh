#!/usr/bin/env bash
# Tests for reelx/tools/package_artifacts.sh against a fake engine out tree.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
script="$here/../package_artifacts.sh"
source "$here/lib.sh"

# A fake engine `out/` with the pieces package_artifacts.sh reads.
make_fake_out() {
  local out="$1"
  for mode in release profile; do
    local fw="$out/mac_${mode}_universal/FlutterMacOS.framework"
    mkdir -p "$fw/Versions/A/Resources"
    echo "binary-$mode" >"$fw/Versions/A/FlutterMacOS"
    ln -s A "$fw/Versions/Current"
    ln -s Versions/Current/FlutterMacOS "$fw/FlutterMacOS"
  done
  local host="$out/host_debug"
  mkdir -p "$host/flutter_patched_sdk/lib" "$host/gen" "$host/dart-sdk/bin"
  echo tester >"$host/flutter_tester"
  chmod +x "$host/flutter_tester"
  echo icu >"$host/icudtl.dat"
  echo dill >"$host/flutter_patched_sdk/platform_strong.dill"
  echo snap >"$host/gen/const_finder.dart.snapshot"
  echo dart >"$host/dart-sdk/bin/dart"
}

package() {
  local out="$1" dest="$2"
  "$script" --out-dir "$out" --dest "$dest" \
    --flutter-version 3.47.7 \
    --engine-revision deb287481e3ce9468f3434937ced4240a70539ca \
    --content-hash d8a2d6c768c334d53e16277f837970ffa1e2de81 \
    --fork-commit 0123456789abcdef \
    --patch-set reelx.0 --patches "P0"
}

setup_tree() {
  tmp="$(mktemp -d)"
  make_fake_out "$tmp/out"
  package "$tmp/out" "$tmp/artifacts" >/dev/null
}

test_writes_every_asset() {
  setup_tree
  for f in FlutterMacOS-release.framework.zip FlutterMacOS-profile.framework.zip \
    engine-folder.zip manifest.json SHA256SUMS; do
    assert_file "$tmp/artifacts/$f"
  done
}

test_manifest_identity_fields() {
  setup_tree
  local m="$tmp/artifacts/manifest.json"
  assert_eq "$(jq -r .flutterVersion "$m")" 3.47.7
  assert_eq "$(jq -r .engineRevision "$m")" deb287481e3ce9468f3434937ced4240a70539ca
  assert_eq "$(jq -r .engineContentHash "$m")" d8a2d6c768c334d53e16277f837970ffa1e2de81
  assert_eq "$(jq -r .forkCommit "$m")" 0123456789abcdef
  assert_eq "$(jq -r .patchSet "$m")" reelx.0
  assert_eq "$(jq -c .patches "$m")" '["P0"]'
  assert_eq "$(jq '.artifacts | length' "$m")" 3
}

test_manifest_hashes_match_files() {
  setup_tree
  local m="$tmp/artifacts/manifest.json" name sha bytes
  assert_file "$m"
  assert_eq "$(jq '.artifacts | length' "$m")" 3
  while read -r name sha bytes; do
    assert_eq "$(sha256sum "$tmp/artifacts/$name" | cut -d' ' -f1)" "$sha" "$name"
    assert_eq "$(wc -c <"$tmp/artifacts/$name" | tr -d ' ')" "$bytes" "$name"
  done < <(jq -r '.artifacts[] | "\(.name) \(.sha256) \(.bytes)"' "$m")
}

test_sha256sums_cover_every_asset() {
  setup_tree
  (cd "$tmp/artifacts" && sha256sum --quiet -c SHA256SUMS)
  for f in FlutterMacOS-release.framework.zip FlutterMacOS-profile.framework.zip \
    engine-folder.zip manifest.json; do
    assert_contains "$(cat "$tmp/artifacts/SHA256SUMS")" "$f"
  done
}

test_engine_folder_layout_without_dart_sdk() {
  setup_tree
  local list
  list="$(unzip -Z1 "$tmp/artifacts/engine-folder.zip")"
  assert_contains "$list" out/host_debug/flutter_tester
  assert_contains "$list" out/host_debug/icudtl.dat
  assert_contains "$list" out/host_debug/flutter_patched_sdk/platform_strong.dill
  assert_contains "$list" out/host_debug/gen/const_finder.dart.snapshot
  if grep -q dart-sdk <<<"$list"; then fail "dart-sdk must not be shipped"; fi
}

test_framework_zip_keeps_symlinks() {
  setup_tree
  local x="$tmp/x"
  mkdir -p "$x" && (cd "$x" && unzip -q "$tmp/artifacts/FlutterMacOS-release.framework.zip")
  [[ -L "$x/FlutterMacOS.framework/FlutterMacOS" ]] || fail "symlink flattened"
  assert_eq "$(cat "$x/FlutterMacOS.framework/FlutterMacOS")" binary-release
}

test_fails_without_tester() {
  tmp="$(mktemp -d)"
  make_fake_out "$tmp/out"
  rm "$tmp/out/host_debug/flutter_tester"
  if package "$tmp/out" "$tmp/artifacts" >/dev/null 2>&1; then
    fail "packaging must fail without flutter_tester"
  fi
}

test_refuses_to_overwrite_existing_assets() {
  setup_tree
  if package "$tmp/out" "$tmp/artifacts" >/dev/null 2>&1; then
    fail "assets must never be replaced"
  fi
}

run_tests
