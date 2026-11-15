import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import struct
import shutil
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))


class FilesystemTests(unittest.TestCase):
    def test_real_read_and_fail_closed_original_protection(self):
        default = Path(__file__).resolve().parents[1] / 'build/tests/generated'
        fixture = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', default))
        exe = fixture.parent / ('ps3-native-contracts.exe' if os.name == 'nt' else 'ps3-native-contracts')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'disc'
            game = root / 'PS3_GAME'
            game.mkdir(parents=True)
            plain = game / 'own.txt'; plain.write_bytes(b'own-fixture\0!')
            protected = game / 'protected.edat'; protected.write_bytes(b'NPD\0' + bytes(256))
            keys, values, entries = bytearray(), bytearray(), bytearray()
            for key, value in (('TITLE', 'Own fixture'), ('TITLE_ID', 'TEST00001'), ('CATEGORY', 'DG')):
                raw = value.encode() + b'\0'
                entries += struct.pack('<HHIII', len(keys), 0x204, len(raw), len(raw), len(values))
                keys += key.encode() + b'\0'; values += raw
            key_offset = 20 + len(entries)
            (game / 'PARAM.SFO').write_bytes(struct.pack('<4s4I', b'\0PSF', 0x101, key_offset, key_offset + len(keys), 3) + entries + keys + values)
            (Path(tmp) / 'outside.txt').write_text('outside original mount')
            snapshot = {p: p.read_bytes() for p in game.iterdir()}
            env = dict(os.environ, PS3_VFS_ROOT=str(root), PS3_HDD0_ROOT=str(Path(tmp) / 'state'),
                       FS_FSTAT_CAP='1', FS_FSTAT_CAP_PATH='own.txt')
            for mode, expected in (('fs', 0), ('fs_syscall', 0), ('fs_boot', 0), ('fs_write', 86), ('fs_traversal', 86), ('fs_edat', 86)):
                before_free = shutil.disk_usage(tmp).free // 1024
                result = subprocess.run([str(exe), mode, str(root)], capture_output=True, timeout=20, env=env)
                self.assertEqual(result.returncode, expected, (mode, result.stdout, result.stderr))
                self.assertEqual(snapshot, {p: p.read_bytes() for p in game.iterdir()})
                if mode == 'fs':
                    self.assertEqual(int(re.search(rb'\[fstat-mtime\] (\d+)', result.stdout)[1]), int(plain.stat().st_mtime))
                if mode == 'fs_boot':
                    reported = int(re.search(rb'\[boot-space-kb\] (\d+)', result.stdout)[1])
                    self.assertLess(abs(reported - before_free), 16384)
            self.assertFalse((game / 'protected.edat.dec').exists())
            corrupt = bytearray((game / 'PARAM.SFO').read_bytes())
            struct.pack_into('<I', corrupt, 8, 0xFFFFFFF0)
            (game / 'PARAM.SFO').write_bytes(corrupt)
            result = subprocess.run([str(exe), 'fs_boot_broken', str(root)], capture_output=True, timeout=20, env=env)
            self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))

    def test_game_data_query_preserves_unicode_hdd_and_originals(self):
        default = Path(__file__).resolve().parents[1] / 'build/tests/generated'
        fixture = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', default))
        exe = fixture.parent / ('ps3-native-contracts.exe' if os.name == 'nt' else 'ps3-native-contracts')
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = base / 'disc'
            game = root / 'PS3_GAME'
            game.mkdir(parents=True)
            hdd = base / ('estado ação ' + 'x' * 80)
            hdd.mkdir()
            env = dict(os.environ, PS3_VFS_ROOT=str(root), PS3_HDD0_ROOT=str(hdd))
            def sfo(category):
                key, value = b'CATEGORY\0', category.encode() + b'\0'
                return (struct.pack('<4s4I', b'\0PSF', 0x101, 36, 36 + len(key), 1)
                        + struct.pack('<HHIII', 0, 0x204, len(value), len(value), 0) + key + value)
            (game / 'PARAM.SFO').write_bytes(sfo('DG'))
            def check(mode, expected=0):
                before = {p.relative_to(base): p.read_bytes() for p in base.rglob('*') if p.is_file()}
                dirs = {p.relative_to(base) for p in base.rglob('*') if p.is_dir()}
                free = shutil.disk_usage(hdd).free // 1024
                result = subprocess.run([str(exe), mode, str(root)], capture_output=True, timeout=20, env=env)
                self.assertEqual(result.returncode, expected, (mode, result.stdout, result.stderr))
                self.assertEqual(before, {p.relative_to(base): p.read_bytes() for p in base.rglob('*') if p.is_file()})
                self.assertEqual(dirs, {p.relative_to(base) for p in base.rglob('*') if p.is_dir()})
                match = re.search(rb'\[data-space-kb\] (\d+)', result.stdout)
                if match:
                    self.assertLess(abs(int(match[1]) - free), 16384)
            check('fs_data_missing')
            check('fs_data_escape')
            check('fs_data_permit')
            data = hdd / 'game/TEST00001'
            data.mkdir(parents=True)
            check('fs_data_missing')  # An empty directory is not installed game data.
            (data / 'PARAM.SFO').write_bytes(sfo('GD'))
            check('fs_data_present')
            check('fs_data_size')
            (data / 'PARAM.SFO').write_bytes(sfo('DG'))
            check('fs_data_broken')
            (data / 'PARAM.SFO').write_bytes(b'\0PSF' + bytes(16))
            check('fs_data_broken')

    def test_installation_recovery_and_atomic_publication(self):
        default = Path(__file__).resolve().parents[1] / 'build/tests/generated'
        fixture = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', default))
        exe = fixture.parent / ('ps3-native-contracts.exe' if os.name == 'nt' else 'ps3-native-contracts')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'disc'
            (root / 'PS3_GAME').mkdir(parents=True)
            hdd = Path(tmp) / 'estado ação'; hdd.mkdir()
            env = dict(os.environ, PS3_VFS_ROOT=str(root), PS3_HDD0_ROOT=str(hdd), PS3_NATIVE_CONTENT_ID='0'*64)
            def run(mode, code=0, override=None):
                r = subprocess.run([str(exe), mode, str(root)], capture_output=True, timeout=20, env=override or env)
                self.assertEqual(r.returncode, code, (r.stdout, r.stderr))
            run('fs_data_install_stage')
            run('fs_data_install_unload', 86)
            stage = hdd / '.native-install/TEST00001-0000000000000000/data'
            self.assertTrue((stage / 'USRDIR').is_dir())
            self.assertFalse((stage / 'PARAM.SFO').exists())
            self.assertFalse((hdd / 'game/TEST00001').exists())
            (stage / 'USRDIR/own.bin').write_bytes(b'original fixture data')
            before = {p.relative_to(hdd): p.read_bytes() for p in hdd.rglob('*') if p.is_file()}
            run('fs_data_install', 86, dict(env, PS3_NATIVE_CONTENT_ID='0'*63+'1'))
            self.assertEqual(before, {p.relative_to(hdd): p.read_bytes() for p in hdd.rglob('*') if p.is_file()})
            run('fs_data_install')
            final = hdd / 'game/TEST00001'
            self.assertEqual((final / 'USRDIR/own.bin').read_bytes(), b'original fixture data')
            self.assertEqual((final / 'USRDIR/ação.bin').read_bytes(), bytes(range(20)) + b'\xEE')
            from ps3native.catalog import sfo
            self.assertEqual(sfo(final / 'PARAM.SFO'),
                             {'CATEGORY':'GD', 'TITLE':'Updated title', 'TITLE_17':'Título local', 'TITLE_ID':'TEST00001', 'VERSION':'01.23'})
            self.assertFalse(stage.exists())
            # A concurrently created target must survive publication unchanged.
            second = Path(tmp) / 'race'; second.mkdir()
            run('fs_data_install_race', 86, dict(env, PS3_HDD0_ROOT=str(second)))
            self.assertTrue((second / 'game/TEST00001').is_dir())
            self.assertTrue((second / '.native-install/TEST00001-0000000000000000/data/PARAM.SFO').is_file())
    def test_installation_rejects_hard_links(self):
        default = Path(__file__).resolve().parents[1] / 'build/tests/generated'
        fixture = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', default))
        exe = fixture.parent / ('ps3-native-contracts.exe' if os.name == 'nt' else 'ps3-native-contracts')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'disc'; (root / 'PS3_GAME').mkdir(parents=True)
            linked = Path(tmp) / 'linked'; linked.mkdir()
            linked_env = dict(os.environ, PS3_VFS_ROOT=str(root), PS3_HDD0_ROOT=str(linked), PS3_NATIVE_CONTENT_ID='0'*64)
            def run(mode, code):
                result = subprocess.run([str(exe), mode, str(root)], capture_output=True, timeout=20, env=linked_env)
                self.assertEqual(result.returncode, code, (result.stdout, result.stderr))
            run('fs_data_install_stage', 0)
            outside = Path(tmp) / 'original.bin'; outside.write_bytes(b'preserved original')
            link = linked / '.native-install/TEST00001-0000000000000000/data/USRDIR/escape.bin'
            try:
                os.link(outside, link)
            except PermissionError:
                self.skipTest('Host denies creation of the owned hard-link fixture')
            run('fs_data_install_escape', 86)
            self.assertEqual(outside.read_bytes(), b'preserved original')
