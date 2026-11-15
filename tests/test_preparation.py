import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from ps3native.catalog import inventory
from ps3native.preparation import prepare

ROOT = Path(__file__).resolve().parents[1]


class PreparationTests(unittest.TestCase):
    def test_real_lifter_cache_reuse_invalidation_and_tampering(self):
        fixture_dir = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', ROOT / 'build/tests/generated'))
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source = base / 'original.elf'
            original = (fixture_dir / 'ppu.elf').read_bytes()
            source.write_bytes(original)
            cache = base / 'cache'
            first = prepare(ROOT, inventory(source), cache)
            self.assertEqual(first['stage'], 'ppu-aot-generated')
            self.assertFalse(first['runnable'])
            self.assertFalse(first['cache_hit'])
            second = prepare(ROOT, inventory(source), cache)
            self.assertTrue(second['cache_hit'])
            self.assertEqual(source.read_bytes(), original)
            changed = bytearray(original)
            changed[0x107] = 20  # li r3,20 instead of 19: a distinct revision.
            source.write_bytes(changed)
            third = prepare(ROOT, inventory(source), cache)
            self.assertNotEqual(first['cache_path'], third['cache_path'])
            self.assertNotEqual(first['outputs']['ppu_recomp.c'], third['outputs']['ppu_recomp.c'])
            source.write_bytes(original)
            (Path(first['cache_path']) / 'ppu_recomp.c').write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'Cached output changed'):
                prepare(ROOT, inventory(source), cache)
            self.assertTrue(Path(third['cache_path']).is_dir())

    def test_encrypted_content_never_reaches_lifter(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'EBOOT.BIN'
            source.write_bytes(b'SCE\0own-encrypted-fixture')
            result = prepare(ROOT, inventory(source), Path(tmp) / 'cache')
            self.assertEqual(result['stage'], 'blocked')
            self.assertFalse(result['runnable'])
            self.assertFalse((Path(tmp) / 'cache').exists())
