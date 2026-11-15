"""Bounded native diagnostic sessions, bound to exact local inputs and binary."""
import json
import os
from pathlib import Path
import subprocess
import time
import re

from ps3native.catalog import digest
from ps3native.dependency import dependency
from ps3native.boot_subset import pointer_table_seeds, select_boot


def bind(root, original, elf, generation, build):
    _, lock = dependency(root)
    exe = build / ('ps3-native-module.exe' if os.name == 'nt' else 'ps3-native-module')
    if not exe.is_file():
        raise ValueError('Native module is not built')
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    configured = re.search(r'^PS3_NATIVE_RECOMP_DIR:PATH=(.*)$', cache, re.M)
    if not configured or Path(configured[1]).resolve() != generation.resolve():
        raise ValueError('Build is configured for a different AOT generation')
    receipt = {'schema': 2, 'diagnostic_only': True, 'original_id': original['id'],
               'original_source': original['source'], 'elf': str(elf.resolve()), 'elf_sha256': digest(elf),
               'binary': str(exe.resolve()), 'binary_sha256': digest(exe),
               'upstream': lock, 'cmake_cache_sha256': digest(build / 'CMakeCache.txt'),
               'generation': {str(p.resolve()): digest(p) for p in generation.glob('ppu_recomp*.*')},
               'guard_sha256': digest(root / 'src/native_guard.cpp'),
               'own_sources': {str(p): digest(root / p) for p in
                               ('src/module_hooks.cpp', 'cmake/NativeModule.cmake', 'CMakeLists.txt', 'src/ps3native/codegen.py')}}
    target = build / 'module/receipt.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix('.pending')
    temporary.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, target)
    return receipt


def run(root, original, build, seconds=20):
    _, lock = dependency(root)
    receipt = json.loads((build / 'module/receipt.json').read_text(encoding='utf-8'))
    if receipt.get('schema') != 2:
        raise ValueError('Native receipt is outdated; prepare the module again')
    if receipt['original_id'] != original['id'] or receipt['upstream'] != lock:
        raise ValueError('Native session does not match this input revision/runtime')
    checks = {receipt['elf']: receipt['elf_sha256'], receipt['binary']: receipt['binary_sha256'],
              str(build / 'CMakeCache.txt'): receipt['cmake_cache_sha256'],
              str(root / 'src/native_guard.cpp'): receipt['guard_sha256'], **receipt['generation'],
              **{str(root / p): expected for p, expected in receipt['own_sources'].items()}}
    for name, expected in checks.items():
        if digest(Path(name)) != expected:
            raise ValueError(f'Prepared native artifact changed: {name}')
    state = root / '.local/state'
    state.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # Diagnostic overrides in the upstream must not silently change guest behavior.
    env = {k: v for k, v in env.items() if not k.upper().startswith(('YDKJ_', 'PPU_', 'SPU_', 'SPURS_', 'PS3_', 'LBP_', 'GH3_', 'FS_', 'RSX_', 'GCM_', 'PSNR_', 'A2O_', 'SYS_HEAP_', 'TTY_'))}
    source = Path(original['source'])
    vfs = source.parent if source.name == 'PS3_GAME' or source.is_file() else source
    env.update(PS3_VFS_ROOT=str(vfs), PS3_HDD0_ROOT=str(state / 'dev_hdd0'),
               PS3_HDD1_ROOT=str(state / 'dev_hdd1'), PS3_STDIO_UNBUFFERED='1')
    metadata = original.get('metadata', {}).get('PARAM.SFO', {})
    if isinstance(metadata.get('TITLE'), str): env['PS3_TITLE'] = metadata['TITLE']
    (state / 'dev_hdd0').mkdir(exist_ok=True)
    (state / 'dev_hdd1').mkdir(exist_ok=True)
    logs = root / '.local/session'
    logs.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    reason = None
    with (logs / 'stdout.log').open('wb') as out, (logs / 'stderr.log').open('wb') as err:
        process = subprocess.Popen([receipt['binary'], receipt['elf']], cwd=state, env=env, stdout=out, stderr=err)
        while process.poll() is None:
            if time.monotonic() - start >= seconds:
                reason = 'duration limit'
            elif out.tell() + err.tell() >= 1024 * 1024:
                reason = '1 MiB log limit'
            if reason:
                process.kill()
                break
            time.sleep(0.05)
        code = process.wait()
    return {'exit': code, 'stop': reason, 'seconds': round(time.monotonic() - start, 3),
            'stdout': str(logs / 'stdout.log'), 'stderr': str(logs / 'stderr.log'),
            'diagnostic_only': True, 'gameplay_verified': False}


def build_native(root, original, elf, generation, build):
    sdk, lock = dependency(root)
    generation = generation.resolve()
    receipt = json.loads((generation / 'receipt.json').read_text(encoding='utf-8'))
    if receipt['key']['input_sha256'] != digest(elf):
        raise ValueError('AOT generation belongs to another ELF revision')
    key = receipt['key']
    commit = key.get('upstream_commit') or key.get('upstream', {}).get('commit')
    if commit != lock['commit'] or key['guards_sha256'] != digest(root / 'src/ps3native/codegen.py'):
        raise ValueError('AOT generation requires the locked lifter and current strict adapter')
    for name, expected in receipt['outputs'].items():
        if Path(name).name != name or digest(generation / name) != expected:
            raise ValueError('AOT output changed; regenerate before native preparation')
    import sys
    sys.path.insert(0, str(sdk / 'tools'))
    from elf_parser import ELFFile
    image = ELFFile(str(elf)); image.load()
    from elf_parser import vaddr_to_offset
    import struct
    entry_offset = vaddr_to_offset(image.program_headers, image.elf_header.e_entry)
    entry = struct.unpack_from('>I', image.raw_data, entry_offset)[0]
    seeds = set(pointer_table_seeds(image))
    priority = []
    previous = generation / 'boot/selection.json'
    limit = 1024
    if previous.exists():
        selected = json.loads(previous.read_text(encoding='utf-8'))
        if selected['entry'] != entry:
            raise ValueError('Existing startup slice belongs to another entry')
        seeds.update(selected['selected'])
        limit = len(seeds) + 256
        session_receipt = build / 'module/receipt.json'
        if session_receipt.exists() and json.loads(session_receipt.read_text(encoding='utf-8'))['original_id'] == original['id']:
            log = root / '.local/session/stderr.log'
            if log.exists():
                priority = [int(a, 16) for a in re.findall(r'\[native-blocked\].*PPU target=0x([0-9A-Fa-f]+)', log.read_text(encoding='utf-8', errors='replace'))]
        if not priority:
            limit = len(seeds)
    output = select_boot(generation, entry, limit, seeds=sorted(seeds), pages=priority)
    subprocess.run(['cmake', '-S', str(root), '-B', str(build), f'-DPS3_NATIVE_RECOMP_DIR={output}'], check=True, timeout=120)
    subprocess.run(['cmake', '--build', str(build), '--target', 'ps3-native-module', '--parallel', '4'], check=True, timeout=1200)
    return bind(root, original, elf, output, build)
