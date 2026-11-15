"""Verify the immutable upstream pin and exact, hash-locked generic patches."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def source_hash(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def dependency(root, apply=False):
    lock = json.loads((root / 'deps.lock.json').read_text(encoding='utf-8'))['ps3recomp']
    sdk = root / lock['path']
    def git(*args):
        try:
            return subprocess.run(['git', '-C', str(sdk), *args], check=True,
                                  capture_output=True, text=True, timeout=30).stdout
        except subprocess.CalledProcessError as exc:
            raise ValueError(f'Upstream Git operation failed: {exc.stderr.strip()}') from exc
    if git('rev-parse', 'HEAD').strip() != lock['commit']:
        raise ValueError('Upstream revision does not match deps.lock.json')
    expected = set()
    for patch in lock['patches']:
        path = root / patch['path']
        if source_hash(path) != patch['sha256']:
            raise ValueError('Locked patch hash mismatch')
        hashes = {name: source_hash(sdk / name) for name in patch['files']}
        if apply and all(hashes[n] == states['before'] for n, states in patch['files'].items()):
            git('apply', '--ignore-space-change', '--check', str(path.resolve()))
            git('apply', '--ignore-space-change', str(path.resolve()))
        for name, states in patch['files'].items():
            if source_hash(sdk / name) != states['after']:
                raise ValueError(f'Locked upstream adaptation missing/changed: {name}')
            expected.add(name)
    changed = {line[3:] for line in git('status', '--porcelain').splitlines()}
    if changed != expected:
        raise ValueError(f'Unrecorded upstream modifications: {changed ^ expected}')
    return sdk, lock


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    dependency(Path(__file__).resolve().parents[2], apply=args.apply)
