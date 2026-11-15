from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from ps3native.session import bind, run


class SessionTests(unittest.TestCase):
    def test_changed_binary_or_input_is_rejected_before_process_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generation, build = root / 'aot', root / 'build'
            generation.mkdir(); build.mkdir()
            for name in ('src/native_guard.cpp', 'src/module_hooks.cpp', 'src/native_stop.cpp', 'src/native_lle.cpp', 'src/native_lle.h', 'src/ps3native/firmware.py', 'cmake/NativeModule.cmake', 'CMakeLists.txt', 'src/ps3native/codegen.py'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('own contract input')
            elf = root / 'input.elf'; elf.write_bytes(b'own input')
            (generation / 'ppu_recomp.c').write_text('// own generated code')
            (build / 'CMakeCache.txt').write_text(f'PS3_NATIVE_RECOMP_DIR:PATH={generation}\n')
            import os
            exe = build / ('ps3-native-module.exe' if os.name == 'nt' else 'ps3-native-module')
            exe.write_bytes(b'not executed by this test')
            original = {'id': 'own-input', 'source': str(elf)}
            with patch('ps3native.session.dependency', return_value=(None, {'commit': 'own-pin'})), patch('ps3native.session.subprocess.Popen') as launch:
                bind(root, original, elf, generation, build)
                with self.assertRaisesRegex(ValueError, 'does not match'):
                    run(root, {**original, 'id': 'different-input'}, build)
                exe.write_bytes(b'changed binary')
                with self.assertRaisesRegex(ValueError, 'artifact changed'):
                    run(root, original, build)
                launch.assert_not_called()
