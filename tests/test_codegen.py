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
    def test_ppu_reachability_keeps_calls_and_excludes_data_after_returns(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'external/ps3recomp/tools'))
        from ppu_lifter import PPULifter
        from ppu_disasm import decode
        words = [0x48001001, 0x4E800020, 0x48003001, 0x38800063]
        insns = [decode(word, 0x1000 + i * 4) for i, word in enumerate(words)]
        lifter = PPULifter(); body = lifter.lift_function(insns, 0x1000, 0x1010)
        self.assertIn(0x2000, lifter.call_targets)
        self.assertNotIn(0x4008, lifter.call_targets)
        self.assertNotIn('gpr[4]', '\n'.join(body.body_lines))
        # An independently prepared interior entry remains executable.
        tail = lifter.lift_function(insns, 0x100C, 0x1010)
        self.assertIn('gpr[4]', '\n'.join(tail.body_lines))

    def test_ppu_reachability_preserves_both_conditional_paths_and_indirect_cases(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'external/ps3recomp/tools'))
        from ppu_lifter import PPULifter
        from ppu_disasm import decode
        def lift(words, cases=None):
            lifter = PPULifter(); lifter.jump_tables = cases or {}
            instructions = [decode(word, 0x1000 + i * 4) for i, word in enumerate(words)]
            return '\n'.join(lifter.lift_function(instructions, 0x1000, 0x1000 + len(words) * 4).body_lines)
        body = lift([0x4182000C, 0x38600001, 0x48000008, 0x38800002, 0x4E800020, 0x38A00003])
        self.assertIn('gpr[3]', body); self.assertIn('gpr[4]', body); self.assertNotIn('gpr[5]', body)
        conditional_return = lift([0x4D820020, 0x38600001, 0x4E800020])
        self.assertIn('gpr[3]', conditional_return)
        indirect_call = lift([0x4E800421, 0x38600001, 0x4E800020])
        self.assertIn('gpr[3]', indirect_call)
        indirect_tail = lift([0x4E800420, 0x38800063, 0x38600001, 0x4E800020], {0x1000: [0x1008]})
        self.assertIn('gpr[3]', indirect_tail); self.assertNotIn('gpr[4]', indirect_tail)

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
            self.assertEqual(text.count('ps3_native_stop();'), 4)
            self.assertIn('if (flag) {', text)
            self.assertEqual(guard_sources(tmp)[path.name], 0)
            path.write_text('fprintf(stderr, "[native-blocked] old guard"); fflush(stderr); exit(86);', encoding='utf-8')
            self.assertEqual(guard_sources(tmp)[path.name], 1)
            self.assertIn('ps3_native_stop();', path.read_text(encoding='utf-8'))

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
