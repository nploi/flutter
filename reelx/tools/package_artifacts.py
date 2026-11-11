#!/usr/bin/env python3
"""Packages engine builds into the assets of one engine release.

Every platform zips its own builds (`assets`), on the machine that built
them; one last step writes the release's manifest and checksums over all of
them (`manifest`). Release assets:

  FlutterMacOS-release.framework.zip        macOS app engine (universal)
  FlutterMacOS-profile.framework.zip        the same in profile mode (if built)
  flutter_linux_gtk-release-linux-x64.zip   Linux app engine (libflutter_linux_gtk.so)
  flutter_windows-release-windows-x64.zip   Windows app engine (flutter_windows.dll)
  libflutter-<mode>-android-<cpu>.zip       Android app engine, as lib/<abi>/libflutter.so
                                            (the path inside an APK)
  Flutter-<mode>.xcframework.zip            iOS app engine (device only)
  engine-folder-<os>-<cpu>.zip              the local-engine layout `flutter test`
                                            and `reelx render` use (out/host_debug,
                                            without dart-sdk: the installer links
                                            the SDK's)
  manifest.json                             identity + sha256 of every asset
  SHA256SUMS                                sha256 of every asset and the manifest

Usage:
  package_artifacts.py assets --platform <macos|linux|windows|android|ios> --out-dir <engine/src/out>
      --dest <dir> [--part all|app|engine-folder] [--tester-dir host_debug_arm64] [--cpu arm64|x64]
  package_artifacts.py manifest --dest <dir> --flutter-version 3.47.9
      --engine-revision <sha> --content-hash <hash> --fork-commit <sha>
      --patch-set ext.<n> [--patches "P0 P1 ..."] [--built-by <text>]

Existing assets in --dest are never replaced.
"""

import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import zipfile

ENGINE_FOLDER_FILES = ('flutter_tester', 'flutter_tester.exe', 'icudtl.dat',
                       'flutter_patched_sdk', 'gen')

PLATFORMS = ('macos', 'linux', 'windows', 'android', 'ios')

# Android's name for each CPU: the lib/<abi> folder of an APK.
ANDROID_ABIS = {'arm64': 'arm64-v8a', 'x64': 'x86_64'}


class PackageError(Exception):
  pass


def sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


def _fresh(dest, name):
  path = os.path.join(dest, name)
  if os.path.exists(path):
    raise PackageError(f'{path} exists; assets are never replaced')
  return path


def _zip_tree(zip_path, base, entries, arc_prefix=''):
  """Zips [entries] (files or folders under [base]) with their paths."""
  with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
    for entry in entries:
      full = os.path.join(base, entry)
      if os.path.isdir(full):
        for root, _, files in os.walk(full):
          for name in sorted(files):
            path = os.path.join(root, name)
            z.write(path, os.path.join(arc_prefix, os.path.relpath(path, base)))
      else:
        z.write(full, os.path.join(arc_prefix, entry))


def _zip_framework(zip_path, folder, name='FlutterMacOS.framework'):
  """Zips [folder]/[name] keeping its symlinks (zip -y)."""
  subprocess.run(['zip', '-qry', os.path.abspath(zip_path), name], cwd=folder, check=True)


def _mobile(platform, out_dir, dest, cpu):
  """Zips the app engine of every mode built for [platform] (release,
  profile); at least one must be there."""
  made = []
  for mode in ('release', 'profile'):
    if platform == 'android':
      folder = os.path.join(out_dir, f'android_{mode}_{cpu}')
      library = os.path.join(folder, 'lib.stripped', 'libflutter.so')
      if not os.path.isfile(library):
        continue
      name = f'libflutter-{mode}-android-{cpu}.zip'
      with zipfile.ZipFile(_fresh(dest, name), 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(library, f'lib/{ANDROID_ABIS[cpu]}/libflutter.so')
    else:
      folder = os.path.join(out_dir, f'ios_{mode}')
      if not os.path.isdir(os.path.join(folder, 'Flutter.xcframework')):
        continue
      name = f'Flutter-{mode}.xcframework.zip'
      _zip_framework(_fresh(dest, name), folder, 'Flutter.xcframework')
    made.append(name)
  if not made:
    raise PackageError(f'no {platform} release or profile build in {out_dir}')
  return made


def _engine_folder(out_dir, tester_dir, dest, name):
  host = os.path.join(out_dir, tester_dir)
  present = [f for f in ENGINE_FOLDER_FILES if os.path.exists(os.path.join(host, f))]
  if not any(f.startswith('flutter_tester') for f in present):
    raise PackageError(f'no flutter_tester in {host}')
  for needed in ('icudtl.dat', 'flutter_patched_sdk'):
    if needed not in present:
      raise PackageError(f'missing {os.path.join(host, needed)}')
  # Always out/host_debug, whatever the build folder was called, so the
  # CLI's --local-engine flags never change.
  _zip_tree(_fresh(dest, name), host, present, arc_prefix='out/host_debug')
  return name


def assets(platform, out_dir, dest, tester_dir='host_debug', cpu='x64', part='all'):
  """Zips [platform]'s builds under [out_dir] into [dest]; returns the names.

  [part] is 'app' (the app's engine), 'engine-folder' (the tester layout) or
  'all', so separate build jobs can each package what they built.
  """
  if platform not in PLATFORMS:
    raise PackageError(f'unknown platform {platform}')
  if platform in ('android', 'ios'):
    # A phone's engine has no tester to run on this machine.
    if part == 'engine-folder':
      raise PackageError(f'{platform} has no engine folder')
    if platform == 'android' and cpu not in ANDROID_ABIS:
      raise PackageError(f'unknown Android CPU {cpu}')
    os.makedirs(dest, exist_ok=True)
    return _mobile(platform, out_dir, dest, cpu)
  os.makedirs(dest, exist_ok=True)
  made = []
  if part == 'engine-folder':
    pass
  elif platform == 'macos':
    for mode in ('release', 'profile'):
      folder = os.path.join(out_dir, f'mac_{mode}_universal')
      if not os.path.isdir(os.path.join(folder, 'FlutterMacOS.framework')):
        if mode == 'release':
          raise PackageError(f'missing {folder}/FlutterMacOS.framework')
        continue
      name = f'FlutterMacOS-{mode}.framework.zip'
      _zip_framework(_fresh(dest, name), folder)
      made.append(name)
  elif platform in ('linux', 'windows'):
    library = 'libflutter_linux_gtk.so' if platform == 'linux' else 'flutter_windows.dll'
    stem = 'flutter_linux_gtk' if platform == 'linux' else 'flutter_windows'
    release = os.path.join(out_dir, 'host_release')
    if not os.path.isfile(os.path.join(release, library)):
      raise PackageError(f'missing {os.path.join(release, library)}')
    name = f'{stem}-release-{platform}-x64.zip'
    _zip_tree(_fresh(dest, name), release, [library])
    made.append(name)
  if part in ('all', 'engine-folder'):
    made.append(_engine_folder(out_dir, tester_dir, dest, f'engine-folder-{platform}-{cpu}.zip'))
  return made


def manifest(dest, flutter_version, engine_revision, content_hash, fork_commit,
             patch_set, patches='', built_by=''):
  """Writes manifest.json and SHA256SUMS over every .zip in [dest]."""
  zips = sorted(n for n in os.listdir(dest) if n.endswith('.zip'))
  if not zips:
    raise PackageError(f'no assets in {dest}')
  if not any(n.startswith('engine-folder-') for n in zips):
    raise PackageError('no engine folder among the assets')
  manifest_path = _fresh(dest, 'manifest.json')
  sums_path = _fresh(dest, 'SHA256SUMS')
  data = {
      'flutterVersion': flutter_version,
      'engineRevision': engine_revision,
      'engineContentHash': content_hash,
      'forkCommit': fork_commit,
      'patchSet': patch_set,
      'patches': [p for p in patches.split() if p],
      'artifacts': [{'name': n, 'sha256': sha256(os.path.join(dest, n)),
                     'bytes': os.path.getsize(os.path.join(dest, n))} for n in zips],
      'builtBy': built_by,
      'builtAt': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
  }
  with open(manifest_path, 'w') as f:
    json.dump(data, f, indent=2)
    f.write('\n')
  with open(sums_path, 'w') as f:
    for name in zips + ['manifest.json']:
      f.write(f'{sha256(os.path.join(dest, name))}  {name}\n')
  return data


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  sub = parser.add_subparsers(dest='command', required=True)
  a = sub.add_parser('assets')
  a.add_argument('--platform', required=True, choices=PLATFORMS)
  a.add_argument('--out-dir', required=True)
  a.add_argument('--dest', required=True)
  a.add_argument('--tester-dir', default='host_debug')
  a.add_argument('--cpu', default='x64', choices=['arm64', 'x64'])
  a.add_argument('--part', default='all', choices=['all', 'app', 'engine-folder'])
  m = sub.add_parser('manifest')
  m.add_argument('--dest', required=True)
  for flag in ('--flutter-version', '--engine-revision', '--content-hash',
               '--fork-commit', '--patch-set'):
    m.add_argument(flag, required=True)
  m.add_argument('--patches', default='')
  m.add_argument('--built-by', default='')
  args = parser.parse_args(argv)
  try:
    if args.command == 'assets':
      for name in assets(args.platform, args.out_dir, args.dest, args.tester_dir, args.cpu,
                         args.part):
        print(os.path.join(args.dest, name))
    else:
      manifest(args.dest, args.flutter_version, args.engine_revision, args.content_hash,
               args.fork_commit, args.patch_set, args.patches, args.built_by)
      print(os.path.join(args.dest, 'manifest.json'))
  except PackageError as e:
    print(f'package_artifacts: {e}', file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
