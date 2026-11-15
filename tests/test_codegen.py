from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
import struct

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from ps3native.codegen import guard_sources
from ps3native.boot_subset import pointer_table_seeds, select_boot


class CodegenTests(unittest.TestCase):
    def test_stripped_pointer_tables_add_preparation_seeds_only(self):
        data = struct.pack('>4I', 0x10000, 0x2000, 0x20000, 0x2000)
        opds = SimpleNamespace(sh_addr=0x1000, sh_offset=0, sh_size=16, sh_flags=2, sh_type=1)
        table = SimpleNamespace(sh_addr=0x3000, sh_offset=16, sh_size=12, sh_flags=2, sh_type=1)
        image = SimpleNamespace(elf_header=SimpleNamespace(e_entry=0x1000), raw_data=data,
                                section_headers=[opds, table])
        image.get_section_data = lambda i: data if i == 0 else struct.pack('>3I', 0x1000, 0x1008, 0)
        self.assertEqual(pointer_table_seeds(image), [0x10000, 0x20000])
        image.get_section_data = lambda i: data if i == 0 else struct.pack('>3I', 0x1000, 0x1008, 0xDEADBEEF)
        self.assertEqual(pointer_table_seeds(image), [])

    def test_unsupported_lowering_cannot_become_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'ppu_recomp.c'
            path.write_text('/* TODO: op31_x1 */;\nif (flag) return; /* bc -> non-code 0x0 */\n/* bl -> non-code 0x0 */;\n/* trap: tw 0, r0, r0 — ignored */;', encoding='utf-8')
            counts = guard_sources(tmp)
            self.assertEqual(counts[path.name], 4)
            text = path.read_text(encoding='utf-8')
            self.assertEqual(text.count('exit(86)'), 4)
            self.assertIn('if (flag) {', text)
            self.assertEqual(guard_sources(tmp)[path.name], 0)

    def test_subset_selects_entry_and_seeds_without_skipping_missing_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'ppu_recomp.h').write_text('// own test ABI')
            (directory / 'ppu_recomp_000.cpp').write_text('''// own generated-code shape
void func_00010000(ppu_context* ctx) {
    func_00010010(ctx);
}
void func_00010010(ppu_context* ctx) {
    func_00010020(ctx);
}
void func_00010020(ppu_context* ctx) {
    return;
}
''')
            output = select_boot(directory, 0x10000, limit=2, seeds=[0x10020])
            result = (output / 'ppu_recomp_boot_table.cpp').read_text()
            self.assertIn('ppu_unlifted_stub(0x00010010, ctx)', result)
            self.assertIn('function_table_count = 2', result)
            header_time = (output / 'ppu_recomp.h').stat().st_mtime_ns
            table_time = (output / 'ppu_recomp_boot_table.cpp').stat().st_mtime_ns
            select_boot(directory, 0x10000, limit=2, seeds=[0x10020])
            self.assertEqual((output / 'ppu_recomp.h').stat().st_mtime_ns, header_time)
            self.assertEqual((output / 'ppu_recomp_boot_table.cpp').stat().st_mtime_ns, table_time)
            select_boot(directory, 0x10000, limit=2, pages=[0x10010])
            self.assertIn('function_table_count = 3', (output / 'ppu_recomp_boot_table.cpp').read_text())
