"""Upstream PPU analysis/code generation with immutable, verified stage receipts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from ps3native.catalog import digest, inventory


def dependency(root):
    lock = json.loads((root / 'deps.lock.json').read_text(encoding='utf-8'))['ps3recomp']
    sdk = root / lock['path']
    for args in (['rev-parse', 'HEAD'], ['status', '--porcelain']):
        result = subprocess.run(['git', '-C', str(sdk), *args], check=True,
                                capture_output=True, text=True, timeout=15).stdout.strip()
        if (args[0] == 'rev-parse' and result != lock['commit']) or (args[0] == 'status' and result):
            raise ValueError('Upstream checkout does not match the pristine locked revision')
    return sdk, lock


def prepare(root, content, cache):
    source = Path(content['source'])
    if source.is_dir() or len(content['files']) != 1 or content['files'][0]['kind'] != 'elf-readable':
        return {'stage': 'blocked', 'runnable': False,
                'blocker': 'M0 code generation accepts a single readable PPU ET_EXEC ELF. SELF/PUP/PRX/container extraction and module binding are pending.'}
    if source.stat().st_size > 256 * 1024 * 1024:
        raise ValueError('M0 preparation input limit is 256 MiB')
    if source == cache.resolve() or source.parent == cache.resolve() or source in cache.resolve().parents:
        raise ValueError('Preparation cache must be separate from originals')
    sdk, lock = dependency(root)
    # Reuse the upstream parser; do not accept its lifter's raw-binary fallback.
    sys.path.insert(0, str(sdk / 'tools'))
    from elf_parser import ELFFile
    elf = ELFFile(str(source))
    elf.load()
    h = elf.elf_header
    if not h.is64 or not h.big_endian or h.e_machine != 21 or h.e_type != 2:
        return {'stage': 'blocked', 'runnable': False, 'blocker': 'Expected big-endian ELF64 PowerPC64 ET_EXEC; PRX relocation is a separate future stage.'}
    source_size = source.stat().st_size
    for segment in elf.program_headers:
        if (segment.p_filesz > segment.p_memsz or segment.p_offset + segment.p_filesz > source_size
                or segment.p_vaddr + segment.p_memsz > 0x100000000):
            raise ValueError('Invalid ELF segment bounds')
    if not any(p.p_type == 1 and p.p_flags & 1 and p.p_filesz for p in elf.program_headers):
        raise ValueError('ELF has no executable PT_LOAD segment')
    # Normalize the source name: identity of the generated code must not depend on a title filename.
    key = {'schema': 1, 'input_sha256': content['files'][0]['sha256'],
           'upstream': lock['commit'], 'adapter_sha256': digest(Path(__file__)),
           'options': ['ppu-loader', 'single-file', 'no-runtime-fallback']}
    cache_key = hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()
    destination = cache / cache_key
    if destination.exists():
        receipt = json.loads((destination / 'receipt.json').read_text(encoding='utf-8'))
        if receipt['key'] != key or not receipt['outputs']:
            raise ValueError('Invalid preparation receipt')
        for name, expected in receipt['outputs'].items():
            if Path(name).name != name or digest(destination / name) != expected:
                raise ValueError('Cached output changed; refusing to reuse it')
        receipt['cache_hit'] = True
        receipt['cache_path'] = str(destination)
        return receipt
    cache.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.pending-', dir=cache))
    try:
        # Only this <=256 MiB executable is staged; never duplicate game trees.
        staged_input = stage / 'input.elf'
        shutil.copyfile(source, staged_input)
        if digest(staged_input) != key['input_sha256']:
            raise ValueError('Input revision changed before preparation')
        for command in ([sys.executable, str(sdk / 'tools/ppu_loader.py'), str(staged_input), '--output', str(stage)],
                        [sys.executable, str(sdk / 'tools/ppu_lifter.py'), str(staged_input),
                         '--functions', str(stage / 'input.functions.json'), '--single-file', '--output', str(stage)]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise ValueError(f'Upstream preparation failed: {(result.stderr + result.stdout)[-2000:]}')
        if inventory(source)['files'][0]['sha256'] != key['input_sha256']:
            raise ValueError('Original changed during preparation')
        staged_input.unlink()
        outputs = {p.name: digest(p) for p in sorted(stage.iterdir()) if p.is_file()}
        receipt = {'key': key, 'stage': 'ppu-aot-generated', 'runnable': False,
                   'outputs': outputs, 'blocker': 'Code generated; generic completeness validation, dependency binding and native executable build are pending. No game execution is certified.'}
        (stage / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        # Atomic publication: a failed generation never replaces a valid cache.
        os.rename(stage, destination)
        receipt.update(cache_hit=False, cache_path=str(destination))
        return receipt
    finally:
        # Only remove our exact, task-owned staging directory; preserve published caches.
        if stage.exists():
            shutil.rmtree(stage)
