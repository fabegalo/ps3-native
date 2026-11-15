import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from ps3native.catalog import inventory
from ps3native.preparation import prepare, spu_artifacts

ROOT = Path(__file__).resolve().parents[1]


class PreparationTests(unittest.TestCase):
    def test_spu_inventory_binds_real_generator_outputs_and_input_revision(self):
        tools = ROOT / 'external/ps3recomp/tools'
        sys.path.insert(0, str(tools))
        from wrap_spu_elf import wrap
        fixture_dir = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', ROOT / 'build/tests/generated'))
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source = base / 'own-spu.elf'
            source.write_bytes(wrap((fixture_dir / 'spu.bin').read_bytes(), base=0, entry=0))
            output = base / 'generated'
            result = subprocess.run([sys.executable, str(tools / 'build_spu_workloads.py'), '--input', str(source),
                            '--lifted', str(output), '--out', str(output / 'spu_workloads.c'),
                            '--constructor', '--merge-chunks'], timeout=15, capture_output=True)
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-2000:])
            artifacts = spu_artifacts(ROOT, output, source)
            report = json.loads((output / 'inventory.json').read_text())
            program = report['programs'][0]
            generated = output / program['name'] / 'spu_recomp.c'
            original_code = generated.read_bytes()
            self.assertIn(str(generated.resolve()), artifacts)
            report['complete'] = False
            report['programs'].append({'status': 'unsupported', 'reason': 'own negative control'})
            (output / 'inventory.json').write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, 'not explicitly prepared'):
                spu_artifacts(ROOT, output)
            report['provenance']['options']['allow_partial'] = True
            (output / 'inventory.json').write_text(json.dumps(report))
            self.assertIn(str(generated.resolve()), spu_artifacts(ROOT, output, source))
            source.write_bytes(b'changed revision')
            with self.assertRaisesRegex(ValueError, 'another ELF revision'):
                spu_artifacts(ROOT, output, source)
            generated.write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'artifact changed'):
                spu_artifacts(ROOT, output)
            generated.write_bytes(original_code)
            program['outputs'][program['name'] + '/../../escape'] = '0' * 64
            (output / 'inventory.json').write_text(json.dumps(report))
            with self.assertRaisesRegex(ValueError, 'artifact changed|escaped'):
                spu_artifacts(ROOT, output)

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
