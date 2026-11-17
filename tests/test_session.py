from pathlib import Path
import sys
import tempfile
import unittest
import struct
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from ps3native.session import bind, run, _self_control


def own_self():
    elf = bytearray(120)
    elf[:6] = b'\x7fELF\2\2'
    struct.pack_into('>Q', elf, 32, 64)
    struct.pack_into('>HH', elf, 54, 56, 1)
    image = bytearray(304)
    image[:8] = b'SCE\0\0\0\0\2'; image[10:12] = b'\0\1'
    struct.pack_into('>Q', image, 16, len(image))
    struct.pack_into('>QQ', image, 48, 128, 192)
    struct.pack_into('>QQ', image, 88, 256, 48)
    image[128:192] = elf[:64]; image[192:248] = elf[64:]
    struct.pack_into('>IIQ', image, 256, 1, 48, 0)
    image[272] = 0x40
    return bytes(elf), image


class SessionTests(unittest.TestCase):
    def test_original_self_control_ranges_and_pairing(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, elf = Path(tmp) / 'own.self', Path(tmp) / 'own.elf'
            executable, image = own_self(); elf.write_bytes(executable); source.write_bytes(image)
            self.assertEqual(_self_control(source, elf), '40' + '00' * 31)
            for offset, data in ((16, struct.pack('>Q', 1024 * 1024 + 1)),
                                 (88, struct.pack('>Q', 300)), (264, struct.pack('>Q', 1)),
                                 (260, struct.pack('>I', 0)), (256, struct.pack('>I', 2)),
                                 (128, b'X'), (192, b'X')):
                bad = image.copy(); bad[offset:offset + len(data)] = data; source.write_bytes(bad)
                with self.assertRaises(ValueError): _self_control(source, elf)
            duplicate = image + image[256:304]
            struct.pack_into('>Q', duplicate, 16, len(duplicate))
            struct.pack_into('>Q', duplicate, 96, 96)
            struct.pack_into('>Q', duplicate, 264, 1)
            source.write_bytes(duplicate)
            with self.assertRaisesRegex(ValueError, 'process control flags'): _self_control(source, elf)

    def test_changed_binary_or_input_is_rejected_before_process_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            generation, build = root / 'aot', root / 'build'
            generation.mkdir(); build.mkdir()
            for name in ('src/native_guard.cpp', 'src/module_hooks.cpp', 'src/native_stop.cpp', 'src/native_storage.cpp', 'src/native_storage.h', 'src/native_system_manager.cpp', 'src/native_system_manager.h', 'src/native_lle.cpp', 'src/native_lle.h', 'src/ps3native/firmware.py', 'src/ps3native/session.py', 'cmake/NativeModule.cmake', 'CMakeLists.txt', 'src/ps3native/codegen.py'):
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
                launch.return_value.poll.return_value = 0
                launch.return_value.wait.return_value = 0
                with patch.dict('os.environ', {'PS3_ENTER_BUTTON': 'invalid', 'SPU_FALLBACK': '1', 'PAD_SCRIPT': 'own-untrusted-input', 'D3D12_VSYNC': '0', 'PS3_BOOT_PATH': '/untrusted', 'PS3_BOOT_CONTROL': 'ff' * 32}):
                    for button in (None, 'cross', 'circle'):
                        run(root, original, build, enter_button=button)
                        env = launch.call_args.kwargs['env']
                        self.assertEqual(env.get('PS3_ENTER_BUTTON'), button)
                        self.assertNotIn('SPU_FALLBACK', env)
                        self.assertNotIn('PAD_SCRIPT', env)
                        self.assertNotIn('D3D12_VSYNC', env)
                        self.assertNotIn('PS3_BOOT_PATH', env)
                        self.assertNotIn('PS3_BOOT_CONTROL', env)
                for invalid in ('relative', '/dev_flash/../file', '/dev_flash\\file', '/x\0y', '/' + 'x' * 1024):
                    with self.assertRaisesRegex(ValueError, 'boot path'):
                        bind(root, original, elf, generation, build, boot_path=invalid)
                bind(root, original, elf, generation, build, boot_path='/dev_flash/vsh/module/vsh.self')
                run(root, original, build)
                self.assertEqual(launch.call_args.kwargs['env']['PS3_BOOT_PATH'], '/dev_flash/vsh/module/vsh.self')
                for invalid in ({'priority': True, 'flags': 16}, {'priority': 3072, 'flags': 16},
                                {'priority': 1001, 'flags': 17}, {'priority': 1001}, []):
                    with self.assertRaisesRegex(ValueError, 'spawn attributes'):
                        bind(root, original, elf, generation, build, spawn_attributes=invalid)
                bind(root, original, elf, generation, build, spawn_attributes={'priority': 1001, 'flags': 16})
                run(root, original, build)
                self.assertEqual(launch.call_args.kwargs['env']['PS3_BOOT_PRIORITY'], '1001')
                self.assertEqual(launch.call_args.kwargs['env']['PS3_BOOT_SPAWN_FLAGS'], '16')
                executable, self_image = own_self(); elf.write_bytes(executable)
                self_path = root / 'own.self'; self_path.write_bytes(self_image)
                bind(root, original, elf, generation, build, self_path=self_path)
                run(root, original, build)
                self.assertEqual(launch.call_args.kwargs['env']['PS3_BOOT_CONTROL'], '40' + '00' * 31)
                self_image[-1] ^= 1; self_path.write_bytes(self_image)
                launch.reset_mock()
                with self.assertRaisesRegex(ValueError, 'SELF process attributes changed'):
                    run(root, original, build)
                launch.assert_not_called()
                self_image[-1] ^= 1; self_path.write_bytes(self_image)
                launch.reset_mock()
                with self.assertRaisesRegex(ValueError, 'cross or circle'):
                    run(root, original, build, enter_button='invalid')
                for name in ('src/native_storage.cpp', 'src/native_storage.h', 'src/native_system_manager.cpp', 'src/native_system_manager.h'):
                    source = root / name; before = source.read_bytes()
                    source.write_bytes(before + b'changed storage contract')
                    with self.assertRaisesRegex(ValueError, 'artifact changed'):
                        run(root, original, build)
                    source.write_bytes(before)
                exe.write_bytes(b'changed binary')
                with self.assertRaisesRegex(ValueError, 'artifact changed'):
                    run(root, original, build)
                launch.assert_not_called()
