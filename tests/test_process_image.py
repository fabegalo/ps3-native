import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from ps3native.catalog import digest
from ps3native.process_image import compile_view, transform


class ProcessImageTests(unittest.TestCase):
    def test_binding_preserves_guest_addresses_and_abi(self):
        source = ('extern "C" uint8_t* vm_base;\n'
                  'void func_00010000(ppu_context* ctx) { vm_base[0x20000] = 1; }\n'
                  'const func_entry function_table[] = {{0x10000, func_00010000, "entry"}};\n')
        text = transform(source, 'image_0123456789abcdef')
        self.assertIn('extern "C" uint8_t* guest_process_image_base(void);', text)
        self.assertIn('guest_process_image_base()[0x20000]', text)
        self.assertIn('{0x10000, image_0123456789abcdef_func_00010000', text)
        self.assertNotIn(' vm_base', text)
        with self.assertRaises(ValueError):
            transform('vm_base = nullptr;', 'image_0123456789abcdef')
        with self.assertRaises(ValueError):
            transform('auto pointer = &vm_base;', 'image_0123456789abcdef')

    def test_view_checks_inputs_and_does_not_modify_preparation(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / 'source', Path(tmp) / 'view'
            source.mkdir()
            for name, text in {'ppu_recomp.h': 'extern uint8_t* vm_base;',
                               'ppu_recomp.c': '#include "ppu_recomp.h"\n'}.items():
                (source / name).write_text(text, encoding='utf-8')
            receipt = {'stage': 'ppu-aot-generated', 'key': {'input_sha256': '01' * 32},
                       'outputs': {p.name: digest(p) for p in source.iterdir()}}
            (source / 'receipt.json').write_text(json.dumps(receipt), encoding='utf-8')
            result = compile_view(source, output)
            self.assertEqual(result['prefix'], 'image_' + '01' * 8)
            self.assertEqual((source / 'ppu_recomp.h').read_text(), 'extern uint8_t* vm_base;')
            self.assertIn('register.cpp', result['outputs'])
            self.assertEqual(compile_view(source, output)['outputs'], result['outputs'])
            (output / 'ppu_recomp_stale.cpp').write_text('stale', encoding='utf-8')
            with self.assertRaises(ValueError):
                compile_view(source, output)
            self.assertEqual((output / 'ppu_recomp_stale.cpp').read_text(), 'stale')
            with self.assertRaises(ValueError):
                compile_view(source, source / 'view')
            (source / 'ppu_recomp.c').write_text('changed', encoding='utf-8')
            with self.assertRaises(ValueError):
                compile_view(source, output)
