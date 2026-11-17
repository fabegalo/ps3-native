from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from ps3native.firmware import validate_prx, prepare_prx, firmware_artifacts, data_interfaces
from lle_fixture import own_prx, own_prx_data_export, own_prx_data_import


class FirmwareTests(unittest.TestCase):
    def test_original_variable_interfaces_are_not_function_descriptors(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp); source = base / 'variables.prx'
            source.write_bytes(own_prx_data_export())
            output = prepare_prx(ROOT, source, base / 'cache', 0x09000000, 'ownAlias')
            receipt = json.loads((output / 'receipt.json').read_text())
            self.assertEqual((receipt['exports'], receipt['data_exports']), (0, 1))
            self.assertIn('"ownAlias", 0xD00DBEEFu, 0x09000180u', (output / 'bundle.cpp').read_text())
            source.write_bytes(own_prx_data_import())
            output = prepare_prx(ROOT, source, base / 'cache', 0x09000000, 'ownSynthetic')
            self.assertEqual(json.loads((output / 'receipt.json').read_text())['data_imports'], 1)
            self.assertIn('"ownAlias", 0xD00DBEEFu, 0x090001E0u', (output / 'bundle.cpp').read_text())
            sys.path.insert(0, str(ROOT / 'external/ps3recomp/tools'))
            from lift_prx import Prx
            for offset, value in ((0x2B0, 4), (0x2B8, 4), (0x224, 0x10001), (0x220, 0x10000), (0x224, 0x10010000)):
                malformed = bytearray(own_prx_data_import()); struct.pack_into('>I', malformed, offset, value)
                source.write_bytes(malformed); prx = Prx(str(source)); image, _ = prx.build_image(0x09000000)
                with self.assertRaises(ValueError): data_interfaces(source, validate_prx(source, 0x09000000), image, 0x09000000)
    def test_multiple_original_libraries_share_one_prepared_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp); source = base / 'multiple.prx'
            source.write_bytes(own_prx(additional_library=True))
            for invalid in ('ownAlias', ['ownSynthetic'], ['missing'], [None], [[]]):
                with self.assertRaises(ValueError):
                    prepare_prx(ROOT, source, base / 'cache', 0x09000000, 'ownSynthetic', additional_libraries=invalid)
            output = prepare_prx(ROOT, source, base / 'cache', 0x09000000, 'ownSynthetic', additional_libraries=['ownAlias'])
            receipt = json.loads((output / 'receipt.json').read_text())
            self.assertEqual(receipt['key']['additional_libraries'], ['ownAlias'])
            self.assertEqual(receipt['exports'], 2)
            bundle = (output / 'bundle.cpp').read_text()
            self.assertIn('"ownAlias"', bundle)
            self.assertIn('0xD00DBEEFu, 0x09000170u', bundle)
            self.assertIn('0xD00DFEEDu, 0x09000170u', bundle)
            self.assertEqual(bundle.count('static const uint8_t image[]'), 1)
            self.assertIn(str(source.resolve()), firmware_artifacts(ROOT, output))
    def test_explicit_original_module_start_preparation(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp); source = base / 'start.prx'
            source.write_bytes(own_prx(module_start=True))
            with self.assertRaisesRegex(ValueError, 'module-start'):
                prepare_prx(ROOT, source, base / 'cache', 0x09000000, 'ownSynthetic')
            output = prepare_prx(ROOT, source, base / 'cache', 0x09000000, 'ownSynthetic', module_start=True)
            self.assertTrue(json.loads((output / 'receipt.json').read_text())['key']['module_start'])
            self.assertIn('0x09000198u', (output / 'bundle.cpp').read_text())
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
