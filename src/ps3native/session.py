"""Bounded native diagnostic sessions, bound to exact local inputs and binary."""
import json
import os
from pathlib import Path
import subprocess
import time
import re
import struct

from ps3native.catalog import digest
from ps3native.dependency import dependency
from ps3native.boot_subset import pointer_table_seeds, select_boot
from ps3native.preparation import spu_artifacts
from ps3native.firmware import firmware_artifacts


def _boot_path(value):
    if value is not None and (not isinstance(value, str) or not value.startswith('/') or
            len(value.encode('utf-8')) > 1023 or any(c in value for c in ('\0', '\\', ':')) or
            '..' in value.split('/')):
        raise ValueError('Invalid guest boot path')
    return value


def _spawn_attributes(value):
    if value is not None and (not isinstance(value, dict) or set(value) != {'priority', 'flags'} or
            type(value['priority']) is not int or not 0 <= value['priority'] <= 3071 or
            type(value['flags']) is not int or value['flags'] not in range(0, 0x80, 0x10)):
        raise ValueError('Invalid guest spawn attributes')
    return value


def _self_control(source, elf):
    """Read original process attributes; structural pairing is not signature verification."""
    with source.open('rb') as stream:
        header = stream.read(112)
        if len(header) != 112 or header[:8] != b'SCE\0\0\0\0\2' or header[10:12] != b'\0\1':
            raise ValueError('Invalid original SELF header')
        limit = struct.unpack_from('>Q', header, 16)[0]
        eh, ph = struct.unpack_from('>QQ', header, 48)
        control, size = struct.unpack_from('>QQ', header, 88)
        if not 112 <= limit <= min(source.stat().st_size, 1024 * 1024):
            raise ValueError('Invalid original SELF header bounds')
        stream.seek(0); data = stream.read(limit)
    def part(offset, length):
        if offset < 112 or offset > limit or length > limit - offset:
            raise ValueError('Invalid original SELF header range')
        return data[offset:offset + length]
    with elf.open('rb') as stream:
        executable = stream.read(64)
        if len(executable) != 64 or executable[:6] != b'\x7fELF\2\2' or part(eh, 64) != executable:
            raise ValueError('Original SELF and ELF headers differ')
        entry_size, count = struct.unpack_from('>HH', executable, 54)
        if entry_size != 56 or not 0 < count <= 128:
            raise ValueError('Invalid original ELF program headers')
        if eh + 64 > ph or ph + count * entry_size > control:
            raise ValueError('Overlapping original SELF headers')
        stream.seek(struct.unpack_from('>Q', executable, 32)[0])
        if part(ph, count * entry_size) != stream.read(count * entry_size):
            raise ValueError('Original SELF and ELF program headers differ')
    records = part(control, size)
    flags = None
    while records:
        if len(records) < 16:
            raise ValueError('Truncated original SELF control header')
        kind, length, following = struct.unpack_from('>IIQ', records)
        if length < 16 or length % 16 or length > len(records) or following != int(length < len(records)):
            raise ValueError('Invalid original SELF control record')
        if kind == 1:
            if length != 48 or flags is not None:
                raise ValueError('Invalid original SELF process control flags')
            flags = records[16:48].hex()
        records = records[length:]
    if flags is None:
        raise ValueError('Original SELF process control flags are missing')
    return flags


def bind(root, original, elf, generation, build, *, boot_path=None, spawn_attributes=None, self_path=None):
    boot_path = _boot_path(boot_path)
    spawn_attributes = _spawn_attributes(spawn_attributes)
    self_input = None
    if self_path is not None:
        self_path = Path(self_path).resolve()
        self_input = {'path': str(self_path), 'sha256': digest(self_path), 'control': _self_control(self_path, elf)}
    _, lock = dependency(root)
    exe = build / ('ps3-native-module.exe' if os.name == 'nt' else 'ps3-native-module')
    if not exe.is_file():
        raise ValueError('Native module is not built')
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    configured = re.search(r'^PS3_NATIVE_RECOMP_DIR:PATH=(.*)$', cache, re.M)
    if not configured or Path(configured[1]).resolve() != generation.resolve():
        raise ValueError('Build is configured for a different AOT generation')
    spu = re.search(r'^PS3_NATIVE_SPU_DIR:PATH=(.+)$', cache, re.M)
    spu_files = spu_artifacts(root, Path(spu[1]), elf) if spu else {}
    firmware = re.search(r'^PS3_NATIVE_FIRMWARE_DIR:PATH=(.+)$', cache, re.M)
    firmware_files = firmware_artifacts(root, Path(firmware[1])) if firmware else {}
    extra_firmware = re.search(r'^PS3_NATIVE_EXTRA_FIRMWARE_DIRS:STRING=(.+)$', cache, re.M)
    if extra_firmware:
        for directory in extra_firmware[1].split(';'):
            firmware_files.update(firmware_artifacts(root, Path(directory)))
    receipt = {'schema': 2, 'diagnostic_only': True, 'original_id': original['id'],
               'original_source': original['source'], 'elf': str(elf.resolve()), 'elf_sha256': digest(elf),
               'binary': str(exe.resolve()), 'binary_sha256': digest(exe), 'boot_path': boot_path,
               'spawn_attributes': spawn_attributes,
               'self_input': self_input,
               'upstream': lock, 'cmake_cache_sha256': digest(build / 'CMakeCache.txt'),
               'generation': {**{str(p.resolve()): digest(p) for p in generation.glob('ppu_recomp*.*')}, **spu_files, **firmware_files},
               'guard_sha256': digest(root / 'src/native_guard.cpp'),
               'own_sources': {str(p): digest(root / p) for p in
                               ('src/module_hooks.cpp', 'src/native_stop.cpp', 'src/native_storage.cpp', 'src/native_storage.h', 'src/native_system_manager.cpp', 'src/native_system_manager.h', 'src/native_config.cpp', 'src/native_input.cpp', 'src/native_lle.cpp', 'src/native_lle.h', 'src/ps3native/firmware.py', 'src/ps3native/session.py', 'cmake/NativeModule.cmake', 'CMakeLists.txt', 'src/ps3native/codegen.py')}}
    target = build / 'module/receipt.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix('.pending')
    temporary.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, target)
    return receipt


def run(root, original, build, seconds=20, *, enter_button=None):
    if enter_button is not None and enter_button not in ('cross', 'circle'):
        raise ValueError('Enter button must be cross or circle')
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
    self_input = receipt.get('self_input')
    if self_input is not None and (digest(Path(self_input['path'])) != self_input['sha256'] or
            _self_control(Path(self_input['path']), Path(receipt['elf'])) != self_input['control']):
        raise ValueError('Original SELF process attributes changed')
    state = root / '.local/state'
    state.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # Diagnostic overrides in the upstream must not silently change guest behavior.
    env = {k: v for k, v in env.items() if not k.upper().startswith(('YDKJ_', 'PPU_', 'SPU_', 'SPURS_', 'PS3_', 'PAD_', 'D3D12_', 'LBP_', 'GH3_', 'FS_', 'RSX_', 'GCM_', 'PSNR_', 'A2O_', 'SYS_HEAP_', 'TTY_'))}
    if enter_button is not None: env['PS3_ENTER_BUTTON'] = enter_button
    boot_path = _boot_path(receipt.get('boot_path'))
    if boot_path is not None: env['PS3_BOOT_PATH'] = boot_path
    if self_input is not None: env['PS3_BOOT_CONTROL'] = self_input['control']
    spawn = _spawn_attributes(receipt.get('spawn_attributes'))
    if spawn is not None:
        env['PS3_BOOT_PRIORITY'] = str(spawn['priority'])
        env['PS3_BOOT_SPAWN_FLAGS'] = str(spawn['flags'])
    source = Path(original['source'])
    vfs = source.parent if source.name == 'PS3_GAME' or source.is_file() else source
    env.update(PS3_VFS_ROOT=str(vfs), PS3_HDD0_ROOT=str(state / 'dev_hdd0'),
               PS3_NATIVE_CONTENT_ID=original['id'],
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
