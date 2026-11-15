from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from ps3native.firmware import validate_prx, prepare_prx, firmware_artifacts
from lle_fixture import own_prx


class FirmwareTests(unittest.TestCase):
    def test_prx_bounds_and_relocations_reject_malformed_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'own.prx'
            source.write_bytes(b'SCE\0own-encrypted')
            with self.assertRaisesRegex(ValueError, 'decrypted'):
                validate_prx(source, 0x08000000)
            source.write_bytes(own_prx())
            self.assertEqual(len(validate_prx(source, 0x08000000)), 2)
            with self.assertRaisesRegex(ValueError, 'image arena'):
                validate_prx(source, 0x0D000000)
            for offset, value in ((0x30B, 255), (0x30F, 44)):
                data = bytearray(own_prx())
                data[offset] = value
                source.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'relocation'):
                    validate_prx(source, 0x08000000)
            data = bytearray(own_prx())
            struct.pack_into('>Q', data, 0x310, 0xFFFFFFFFFFFFFFFF)
            source.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'relocation value'):
                validate_prx(source, 0x08000000)
            source.write_bytes(own_prx()[:-1])
            with self.assertRaisesRegex(ValueError, 'file bounds'):
                validate_prx(source, 0x08000000)

    def test_real_prx_pipeline_preserves_original_and_binds_cached_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source = base / 'own.prx'
            original = own_prx()
            source.write_bytes(original)
            output = prepare_prx(ROOT, source, base / 'cache', 0x08000000, 'ownSynthetic')
            self.assertEqual(source.read_bytes(), original)
            code = output / 'lift/ppu_recomp.c'
            stamp = code.stat().st_mtime_ns
            self.assertEqual(prepare_prx(ROOT, source, base / 'cache', 0x08000000, 'ownSynthetic'), output)
            self.assertEqual(code.stat().st_mtime_ns, stamp)
            self.assertIn(str(source.resolve()), firmware_artifacts(ROOT, output))
            old_code = code.read_bytes()
            code.write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'artifact changed'):
                firmware_artifacts(ROOT, output)
            code.write_bytes(old_code)
            source.write_bytes(b'changed input')
            with self.assertRaisesRegex(ValueError, 'input revision changed'):
                firmware_artifacts(ROOT, output)
            source.write_bytes(original)
            receipt = output / 'receipt.json'
            metadata = json.loads(receipt.read_text())
            metadata['outputs']['../escape'] = '0' * 64
            receipt.write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, 'escaped'):
                firmware_artifacts(ROOT, output)
