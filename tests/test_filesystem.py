import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import struct
import shutil
import re


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
            env = dict(os.environ, PS3_VFS_ROOT=str(root), PS3_HDD0_ROOT=str(Path(tmp) / 'state'))
            for mode, expected in (('fs', 0), ('fs_boot', 0), ('fs_write', 86), ('fs_traversal', 86), ('fs_edat', 86)):
                before_free = shutil.disk_usage(tmp).free // 1024
                result = subprocess.run([str(exe), mode, str(root)], capture_output=True, timeout=20, env=env)
                self.assertEqual(result.returncode, expected, (mode, result.stdout, result.stderr))
                self.assertEqual(snapshot, {p: p.read_bytes() for p in game.iterdir()})
                if mode == 'fs_boot':
                    reported = int(re.search(rb'\[boot-space-kb\] (\d+)', result.stdout)[1])
                    self.assertLess(abs(reported - before_free), 16384)
            self.assertFalse((game / 'protected.edat.dec').exists())
            corrupt = bytearray((game / 'PARAM.SFO').read_bytes())
            struct.pack_into('<I', corrupt, 8, 0xFFFFFFF0)
            (game / 'PARAM.SFO').write_bytes(corrupt)
            result = subprocess.run([str(exe), 'fs_boot_broken', str(root)], capture_output=True, timeout=20, env=env)
            self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
