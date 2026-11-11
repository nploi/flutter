#!/usr/bin/env bash
# Packages a patched engine build into release assets:
#   FlutterMacOS-release.framework.zip  universal release framework
#   FlutterMacOS-profile.framework.zip  universal profile framework (benchmarks)
#   engine-folder.zip                   local-engine layout for `flutter test`
#                                       (out/host_debug without dart-sdk; the
#                                       installer links the SDK's dart-sdk)
#   manifest.json                       identity + sha256 of every artifact
#   SHA256SUMS                          sha256 of every asset
#
# Usage:
#   package_artifacts.sh --out-dir <engine/src/out> --dest <dir> \
#     --flutter-version 3.47.7 --engine-revision <sha> --content-hash <hash> \
#     --fork-commit <sha> --patch-set engine_perf.<n> --patches "P0 P1 ..."
#
# The out dir must hold mac_release_universal/ and mac_profile_universal/
# (each with FlutterMacOS.framework, made by build_engine.sh) and host_debug/.
# Existing assets in --dest are never replaced.
set -euo pipefail

die() { echo "package_artifacts: $*" >&2; exit 1; }

out_dir="" dest="" flutter_version="" engine_revision="" content_hash=""
fork_commit="" patch_set="" patches="" host_dir="host_debug"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir) out_dir="$2"; shift 2 ;;
    # The tester build to ship (host_debug_arm64 on Apple silicon); it is
    # always staged as out/host_debug so --local-engine flags never change.
    --host-dir) host_dir="$2"; shift 2 ;;
    --dest) dest="$2"; shift 2 ;;
    --flutter-version) flutter_version="$2"; shift 2 ;;
    --engine-revision) engine_revision="$2"; shift 2 ;;
    --content-hash) content_hash="$2"; shift 2 ;;
    --fork-commit) fork_commit="$2"; shift 2 ;;
    --patch-set) patch_set="$2"; shift 2 ;;
    --patches) patches="$2"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done
for v in out_dir dest flutter_version engine_revision content_hash fork_commit patch_set; do
  [[ -n "${!v}" ]] || die "--${v//_/-} is required"
done

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

host="$out_dir/$host_dir"
for f in flutter_tester icudtl.dat flutter_patched_sdk; do
  [[ -e "$host/$f" ]] || die "missing $host/$f"
done
for mode in release profile; do
  [[ -d "$out_dir/mac_${mode}_universal/FlutterMacOS.framework" ]] ||
    die "missing $out_dir/mac_${mode}_universal/FlutterMacOS.framework"
done

assets=(FlutterMacOS-release.framework.zip FlutterMacOS-profile.framework.zip
  engine-folder.zip manifest.json SHA256SUMS)
mkdir -p "$dest"
for a in "${assets[@]}"; do
  [[ ! -e "$dest/$a" ]] || die "$dest/$a exists; assets are never replaced"
done
dest="$(cd "$dest" && pwd)"

# Frameworks: zip -y keeps the framework's internal symlinks.
for mode in release profile; do
  (cd "$out_dir/mac_${mode}_universal" &&
    zip -qry "$dest/FlutterMacOS-$mode.framework.zip" FlutterMacOS.framework)
done

# Engine folder: the layout flutter_tools expects for --local-engine-src-path
# (<src>/out/host_debug). dart-sdk is left out on purpose.
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/out/host_debug"
for f in flutter_tester icudtl.dat flutter_patched_sdk gen; do
  [[ -e "$host/$f" ]] && cp -R "$host/$f" "$stage/out/host_debug/"
done
(cd "$stage" && zip -qry "$dest/engine-folder.zip" out)

artifacts="[]"
for a in FlutterMacOS-release.framework.zip FlutterMacOS-profile.framework.zip engine-folder.zip; do
  artifacts="$(jq -c --arg n "$a" --arg s "$(sha256 "$dest/$a")" \
    --argjson b "$(wc -c <"$dest/$a" | tr -d ' ')" \
    '. + [{name: $n, sha256: $s, bytes: $b}]' <<<"$artifacts")"
done

jq -n \
  --arg flutterVersion "$flutter_version" \
  --arg engineRevision "$engine_revision" \
  --arg engineContentHash "$content_hash" \
  --arg forkCommit "$fork_commit" \
  --arg patchSet "$patch_set" \
  --arg patches "$patches" \
  --argjson artifacts "$artifacts" \
  --arg chip "$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)" \
  --arg os "$(sw_vers -productVersion 2>/dev/null || uname -sr)" \
  --arg xcode "$(xcodebuild -version 2>/dev/null | head -1 || true)" \
  --arg builtAt "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  '{flutterVersion: $flutterVersion, engineRevision: $engineRevision,
    engineContentHash: $engineContentHash, forkCommit: $forkCommit,
    patchSet: $patchSet, patches: ($patches | split(" ") | map(select(. != ""))),
    artifacts: $artifacts,
    builtOn: {chip: $chip, os: $os, xcode: $xcode}, builtAt: $builtAt}' \
  >"$dest/manifest.json"

(cd "$dest" && for a in FlutterMacOS-release.framework.zip FlutterMacOS-profile.framework.zip \
  engine-folder.zip manifest.json; do
  echo "$(sha256 "$a")  $a"
done >SHA256SUMS)

echo "$dest"
