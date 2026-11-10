#!/usr/bin/env python3
"""Tests for reelx/tools/package_artifacts.py against fake engine out trees."""

import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import package_artifacts as pa  # noqa: E402


def write(path, text):
  os.makedirs(os.path.dirname(path), exist_ok=True)
  with open(path, 'w') as f:
    f.write(text)


def fake_tester(out, folder='host_debug', exe='flutter_tester'):
  host = os.path.join(out, folder)
  write(os.path.join(host, exe), 'tester')
  os.chmod(os.path.join(host, exe), 0o755)
  write(os.path.join(host, 'icudtl.dat'), 'icu')
  write(os.path.join(host, 'flutter_patched_sdk', 'platform_strong.dill'), 'dill')
  write(os.path.join(host, 'gen', 'frontend_server_aot.dart.snapshot'), 'snap')
  write(os.path.join(host, 'dart-sdk', 'bin', 'dart'), 'dart')  # never shipped
  write(os.path.join(host, 'obj', 'big.o'), 'object')            # never shipped


class AssetsTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.mkdtemp()
    self.out = os.path.join(self.tmp, 'out')
    self.dest = os.path.join(self.tmp, 'dest')

  def tearDown(self):
    shutil.rmtree(self.tmp)

  def names(self, zip_name):
    with zipfile.ZipFile(os.path.join(self.dest, zip_name)) as z:
      return sorted(z.namelist())

  def test_linux_library_and_engine_folder(self):
    write(os.path.join(self.out, 'host_release', 'libflutter_linux_gtk.so'), 'engine')
    fake_tester(self.out)
    made = pa.assets('linux', self.out, self.dest)
    self.assertEqual(made, ['flutter_linux_gtk-release-linux-x64.zip', 'engine-folder-linux-x64.zip'])
    self.assertEqual(self.names(made[0]), ['libflutter_linux_gtk.so'])
    self.assertEqual(self.names(made[1]), [
        'out/host_debug/flutter_patched_sdk/platform_strong.dill',
        'out/host_debug/flutter_tester',
        'out/host_debug/gen/frontend_server_aot.dart.snapshot',
        'out/host_debug/icudtl.dat',
    ])
    with zipfile.ZipFile(os.path.join(self.dest, made[1])) as z:
      mode = z.getinfo('out/host_debug/flutter_tester').external_attr >> 16
    self.assertTrue(mode & stat.S_IXUSR, 'the tester stays executable')

  def test_windows_dll_and_exe_tester(self):
    write(os.path.join(self.out, 'host_release', 'flutter_windows.dll'), 'engine')
    fake_tester(self.out, exe='flutter_tester.exe')
    made = pa.assets('windows', self.out, self.dest)
    self.assertEqual(made, ['flutter_windows-release-windows-x64.zip', 'engine-folder-windows-x64.zip'])
    self.assertIn('out/host_debug/flutter_tester.exe', self.names(made[1]))

  def test_a_cpu_folder_is_shipped_as_host_debug(self):
    write(os.path.join(self.out, 'host_release', 'libflutter_linux_gtk.so'), 'engine')
    fake_tester(self.out, folder='host_debug_arm64')
    made = pa.assets('linux', self.out, self.dest, tester_dir='host_debug_arm64', cpu='arm64')
    self.assertEqual(made[1], 'engine-folder-linux-arm64.zip')
    self.assertIn('out/host_debug/flutter_tester', self.names(made[1]))

  @unittest.skipUnless(shutil.which('zip'), 'needs zip')
  def test_macos_frameworks_keep_their_symlinks(self):
    for mode in ('release', 'profile'):
      fw = os.path.join(self.out, f'mac_{mode}_universal', 'FlutterMacOS.framework')
      write(os.path.join(fw, 'Versions', 'A', 'FlutterMacOS'), mode)
      os.symlink('A', os.path.join(fw, 'Versions', 'Current'))
      os.symlink('Versions/Current/FlutterMacOS', os.path.join(fw, 'FlutterMacOS'))
    fake_tester(self.out, folder='host_debug_arm64')
    made = pa.assets('macos', self.out, self.dest, tester_dir='host_debug_arm64', cpu='arm64')
    self.assertEqual(made, ['FlutterMacOS-release.framework.zip', 'FlutterMacOS-profile.framework.zip',
                            'engine-folder-macos-arm64.zip'])
    with zipfile.ZipFile(os.path.join(self.dest, made[0])) as z:
      link = z.getinfo('FlutterMacOS.framework/FlutterMacOS')
      self.assertTrue(stat.S_ISLNK(link.external_attr >> 16), 'symlinks kept')

  def test_parts_package_separately(self):
    write(os.path.join(self.out, 'host_release', 'libflutter_linux_gtk.so'), 'engine')
    self.assertEqual(pa.assets('linux', self.out, self.dest, part='app'),
                     ['flutter_linux_gtk-release-linux-x64.zip'])
    fake_tester(self.out)
    self.assertEqual(pa.assets('linux', self.out, self.dest, part='engine-folder'),
                     ['engine-folder-linux-x64.zip'])

  def test_missing_pieces_are_refused(self):
    with self.assertRaises(pa.PackageError):
      pa.assets('linux', self.out, self.dest)  # no release library
    write(os.path.join(self.out, 'host_release', 'libflutter_linux_gtk.so'), 'engine')
    with self.assertRaises(pa.PackageError):
      pa.assets('linux', self.out, self.dest)  # no tester
    with self.assertRaises(pa.PackageError):
      pa.assets('beos', self.out, self.dest)

  def test_assets_are_never_replaced(self):
    write(os.path.join(self.out, 'host_release', 'libflutter_linux_gtk.so'), 'engine')
    fake_tester(self.out)
    pa.assets('linux', self.out, self.dest)
    with self.assertRaises(pa.PackageError):
      pa.assets('linux', self.out, self.dest)


class ManifestTest(unittest.TestCase):

  def setUp(self):
    self.dest = tempfile.mkdtemp()
    for name in ('flutter_linux_gtk-release-linux-x64.zip', 'engine-folder-linux-x64.zip',
                 'flutter_windows-release-windows-x64.zip'):
      write(os.path.join(self.dest, name), name)

  def tearDown(self):
    shutil.rmtree(self.dest)

  def make(self, **kw):
    args = dict(flutter_version='3.47.9', engine_revision='89c39edf', content_hash='9e025265',
                fork_commit='abc', patch_set='ext.1', patches='P0 P1')
    args.update(kw)
    return pa.manifest(self.dest, **args)

  def test_manifest_lists_every_asset_with_its_sha256(self):
    data = self.make()
    self.assertEqual(data['patchSet'], 'ext.1')
    self.assertEqual(data['patches'], ['P0', 'P1'])
    names = [a['name'] for a in data['artifacts']]
    self.assertEqual(names, sorted(names))
    self.assertEqual(len(names), 3)
    first = data['artifacts'][0]
    with open(os.path.join(self.dest, first['name']), 'rb') as f:
      self.assertEqual(first['sha256'], hashlib.sha256(f.read()).hexdigest())
    with open(os.path.join(self.dest, 'manifest.json')) as f:
      self.assertEqual(json.load(f)['engineContentHash'], '9e025265')

  def test_sums_cover_the_assets_and_the_manifest(self):
    self.make()
    with open(os.path.join(self.dest, 'SHA256SUMS')) as f:
      lines = f.read().splitlines()
    self.assertEqual([l.split('  ')[1] for l in lines][-1], 'manifest.json')
    self.assertEqual(len(lines), 4)
    for line in lines:
      digest, name = line.split('  ')
      self.assertEqual(digest, pa.sha256(os.path.join(self.dest, name)))

  def test_a_release_needs_an_engine_folder(self):
    os.remove(os.path.join(self.dest, 'engine-folder-linux-x64.zip'))
    with self.assertRaises(pa.PackageError):
      self.make()

  def test_a_manifest_is_never_replaced(self):
    self.make()
    with self.assertRaises(pa.PackageError):
      self.make()


if __name__ == '__main__':
  unittest.main()
