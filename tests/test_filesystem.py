import os
from pathlib import Path
import subprocess
import tempfile
import unittest


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
            (Path(tmp) / 'outside.txt').write_text('outside original mount')
            snapshot = {p: p.read_bytes() for p in game.iterdir()}
            for mode, expected in (('fs', 0), ('fs_write', 86), ('fs_traversal', 86), ('fs_edat', 86)):
                result = subprocess.run([str(exe), mode, str(root)], capture_output=True, timeout=20)
                self.assertEqual(result.returncode, expected, (mode, result.stdout, result.stderr))
                self.assertEqual(snapshot, {p: p.read_bytes() for p in game.iterdir()})
            self.assertFalse((game / 'protected.edat.dec').exists())
