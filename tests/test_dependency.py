import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
from ps3native.dependency import source_hash


class DependencyTests(unittest.TestCase):
    def test_locked_patch_applies_to_exact_upstream_files(self):
        lock = json.loads((ROOT / 'deps.lock.json').read_text(encoding='utf-8'))['ps3recomp']
        sdk = ROOT / lock['path']
        with tempfile.TemporaryDirectory() as tmp:
            pristine = Path(tmp)
            for patch in lock['patches']:
                for name, hashes in patch['files'].items():
                    if hashes['before'] is None:
                        self.assertIsNone(source_hash(pristine / name))
                        continue
                    source = subprocess.check_output(['git', '-C', str(sdk), 'show', lock['commit'] + ':' + name])
                    target = pristine / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source)
                    self.assertEqual(source_hash(target), hashes['before'])
                result = subprocess.run(['git', 'apply', str(ROOT / patch['path'])], cwd=pristine,
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                for name, hashes in patch['files'].items():
                    self.assertEqual(source_hash(pristine / name), hashes['after'])
