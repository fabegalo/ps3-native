import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from ps3native.catalog import inventory, pup, sfo
from ps3native.cli import main


def make_sfo():
    keys = b'TITLE\0TITLE_ID\0'
    values = b'Own fixture\0TEST00001\0'
    return (struct.pack('<4s4I', b'\0PSF', 0x101, 52, 52 + len(keys), 2)
            + struct.pack('<HHIII', 0, 0x204, 12, 12, 0)
            + struct.pack('<HHIII', 6, 0x204, 10, 10, 12) + keys + values)


def make_pup():
    data = b'04.9200\n'
    return (b'SCEUF\0\0\0' + struct.pack('>5Q', 1, 1, 1, 112, len(data))
            + struct.pack('>3Q8x', 0x100, 112, len(data)) + bytes(32) + data)


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def call(self, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return main(list(map(str, args)))

    def test_sfo_metadata_and_corrupt_bounds(self):
        path = self.write('PARAM.SFO', make_sfo())
        self.assertEqual(sfo(path)['TITLE_ID'], 'TEST00001')
        data = bytearray(make_sfo())
        struct.pack_into('<I', data, 32, 0xffffffff)
        path.write_bytes(data)
        with self.assertRaises(ValueError):
            sfo(path)

    def test_sfo_truncations(self):
        for size in (0, 4, 19, 51, len(make_sfo()) - 1):
            with self.subTest(size=size):
                with self.assertRaises(ValueError):
                    sfo(self.write('PARAM.SFO', make_sfo()[:size]))

    def test_pup_identity_is_not_execution_or_authentication(self):
        path = self.write('PS3UPDAT.PUP', make_pup())
        result = inventory(path)
        metadata = result['metadata']['PS3UPDAT.PUP']
        self.assertEqual(metadata['version_text'], '04.9200')
        self.assertFalse(result['runnable'])
        self.assertIn('not verified', metadata['authentication'])

    def test_pup_rejects_invalid_offsets_and_counts(self):
        for offset, value in ((24, 0), (24, 4097), (32, 32), (56, 0), (64, 99999)):
            data = bytearray(make_pup())
            struct.pack_into('>Q', data, offset, value)
            with self.subTest(offset=offset, value=value):
                with self.assertRaises(ValueError):
                    pup(self.write('bad.PUP', data))

    def test_import_unicode_readonly_idempotent_and_invalidation(self):
        path = self.write('Jogo ação/PS3_GAME/PARAM.SFO', make_sfo())
        eboot = self.write('Jogo ação/PS3_GAME/USRDIR/EBOOT.BIN', b'SCE\0encrypted')
        source = path.parent.parent
        before = {p: p.read_bytes() for p in source.rglob('*') if p.is_file()}
        catalog = self.root / 'catalog'
        args = ('--catalog', catalog, 'import', source)
        self.assertEqual(self.call(*args), 0)
        self.assertEqual(self.call(*args), 0)
        self.assertEqual(len(list(catalog.glob('*.json'))), 1)
        saved = json.loads(next(catalog.glob('*.json')).read_text(encoding='utf-8'))
        self.assertEqual(saved['state'], 'inventoried')
        self.assertEqual(saved['files'][-1]['kind'], 'self-encrypted')
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        eboot.write_bytes(b'SCE\0revision2')
        self.assertNotEqual(saved['id'], inventory(source)['id'])
        self.assertEqual(self.call(*args), 0)
        self.assertEqual(len(list(catalog.glob('*.json'))), 2)

    def test_catalog_cannot_modify_original_tree(self):
        source = self.write('game/data', b'own').parent
        self.assertEqual(self.call('--catalog', source / 'catalog', 'import', source), 2)
        self.assertFalse((source / 'catalog').exists())

    def test_unsupported_commands_fail_explicitly(self):
        original = self.write('EBOOT.BIN', b'SCE\0encrypted')
        for command in ('prepare', 'run'):
            self.assertEqual(self.call(command, original), 3)
        self.assertEqual(self.call('system'), 3)
        self.assertEqual(self.call('run'), 2)
        self.assertEqual(self.call('run', original, '--fixture', 'ppu'), 2)

    def test_links_rejected(self):
        source = self.write('game/data', b'own').parent
        link = source / 'link'
        try:
            link.symlink_to(source / 'data')
        except OSError:
            self.skipTest('Host does not permit symbolic links')
        with self.assertRaises(ValueError):
            inventory(source)
        with self.assertRaises(ValueError):
            inventory(link)
        link.unlink()
        alias = self.root / 'parent-alias'
        alias.symlink_to(source, target_is_directory=True)
        self.assertEqual(inventory(alias / 'data')['source'], str((source / 'data').resolve()))

    def test_native_launcher_unicode_and_exit_codes(self):
        fixture_dir = Path(os.environ.get('PS3_NATIVE_FIXTURE_DIR', Path(__file__).resolve().parents[1] / 'build/tests/generated'))
        exe = fixture_dir.parents[1] / ('ps3-native.exe' if os.name == 'nt' else 'ps3-native')
        original = self.write('ゲーム ação/EBOOT.BIN', b'SCE\0fixture')
        result = subprocess.run([str(exe), '--catalog', str(self.root / 'catalog'), 'import', str(original)],
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        self.assertEqual(record['source'], str(original.resolve()))
        result = subprocess.run([str(exe), 'system'], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 3)
        self.assertFalse(json.loads(result.stdout)['runnable'])

    def test_missing_empty_content(self):
        with self.assertRaises(OSError):
            inventory(self.root / 'missing')
        with self.assertRaises(ValueError):
            inventory(self.root)


if __name__ == '__main__':
    unittest.main()
