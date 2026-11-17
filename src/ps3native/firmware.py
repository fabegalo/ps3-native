"""Prepare a decrypted original PRX with explicit, hash-bound export ownership."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from ps3native.catalog import digest
from ps3native.codegen import guard_sources
from ps3native.dependency import dependency


def validate_prx(source, base):
    data = source.read_bytes()
    if not 64 <= len(data) <= 64 * 1024 * 1024 or data[:6] != b'\x7fELF\x02\x02':
        raise ValueError('Expected a decrypted ELF64 big-endian PRX')
    if struct.unpack_from('>HH', data, 16) != (0xFFA4, 21):
        raise ValueError('Expected a PPC64 relocatable PRX')
    phoff = struct.unpack_from('>Q', data, 32)[0]
    width, count = struct.unpack_from('>HH', data, 54)
    if width != 56 or not 0 < count <= 128 or phoff + count * width > len(data):
        raise ValueError('Invalid PRX program-header bounds')
    loads, relocation = [], None
    for i in range(count):
        kind, flags, offset, address, physical, filesz, memsz, align = struct.unpack_from('>IIQQQQQQ', data, phoff + i * width)
        if offset + filesz > len(data):
            raise ValueError('Invalid PRX segment file bounds')
        if kind == 1:
            if filesz > memsz or address + memsz > 0x100000000 - base:
                raise ValueError('Invalid PRX load range')
            if not loads and not flags & 1:
                raise ValueError('PRX first load segment must contain executable code')
            loads.append((offset, address, filesz, memsz))
            if len(loads) == 1 and physical + 52 > len(data):
                raise ValueError('Invalid PRX module-info offset')
        elif kind == 0x700000A4:
            if relocation is not None or filesz % 24:
                raise ValueError('Invalid PRX relocation table')
            relocation = offset, filesz
    if not loads or relocation is None:
        raise ValueError('PRX load segments or relocation table are missing')
    size = max(address + memsz for _, address, _, memsz in loads)
    if not 0 < size <= 64 * 1024 * 1024 or not 0x10000 <= base < base + size <= 0x0D000000:
        raise ValueError('PRX does not fit the supported lower-memory image arena')
    for offset in range(relocation[0], sum(relocation), 24):
        target, _, value_segment, target_segment, kind, addend = struct.unpack_from('>QHBBIQ', data, offset)
        # The selected upstream implementation has audited absolute 32/16-bit relocations.
        sizes = {1: 4, 4: 2, 5: 2, 6: 2, 57: 2}
        if kind not in sizes or target_segment >= len(loads) or (value_segment != 255 and value_segment >= len(loads)):
            raise ValueError('Unsupported PRX relocation type or segment index')
        if target + sizes[kind] > loads[target_segment][3]:
            raise ValueError('PRX relocation target exceeds its segment')
        value = (0 if value_segment == 255 else base + loads[value_segment][1]) + addend
        if value > 0xFFFFFFFF:
            raise ValueError('PRX relocation value exceeds the guest address space')
        if kind == 57:
            file_offset, _, file_size, _ = loads[target_segment]
            if (target % 4 != 2 or target + 2 > file_size or value & 3 or
                    struct.unpack_from('>I', data, file_offset + target - 2)[0] >> 26 not in (58, 62)):
                raise ValueError('Invalid or unsupported PRX ADDR16_LO_DS instruction/alignment')
    return loads


def data_interfaces(source, loads, image, base):
    """Prepare optional ADDR32 variable references; executable fixups remain unsupported."""
    data = source.read_bytes()
    phoff = struct.unpack_from('>Q', data, 32)[0]
    count = struct.unpack_from('>H', data, 56)[0]
    headers = [struct.unpack_from('>IIQQQQQQ', data, phoff + i * 56) for i in range(count)]
    info = next(p[4] for p in headers if p[0] == 1)
    writable = [(base + p[3], base + p[3] + p[6]) for p in headers if p[0] == 1 and p[1] & 2]
    def offset(address, size):
        if not any(base + va <= address and address + size <= base + va + fs for _, va, fs, _ in loads):
            raise ValueError('PRX data-interface metadata exceeds file-backed loads')
        return address - base
    def u32(address):
        return struct.unpack_from('>I', image, offset(address, 4))[0]
    def name(address):
        value = bytearray()
        for i in range(256):
            byte = image[offset(address + i, 1)]
            if not byte:
                text = value.decode('ascii')
                if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', text):
                    raise ValueError('Invalid PRX data-interface library name')
                return text
            value.append(byte)
        raise ValueError('Unterminated PRX data-interface library name')
    exported, imported = [], []
    for is_import, field, width in ((False, 0x24, 0x1C), (True, 0x2C, 0x2C)):
        first, last = struct.unpack_from('>II', data, info + field)
        cursor, end = base + first, base + last
        if cursor > end:
            raise ValueError('Reversed PRX library table')
        while cursor < end:
            o = offset(cursor, width)
            if image[o] != width or cursor + width > end:
                raise ValueError('Unsupported PRX data-interface table layout')
            attributes, funcs, variables, tls = struct.unpack_from('>HHHH', image, o + 4)
            if variables > 4096:
                raise ValueError('PRX variable table exceeds the preparation limit')
            if tls:
                raise ValueError('PRX TLS variable linkage is not prepared')
            library_address = u32(cursor + 0x10)
            if variables and library_address:
                library = name(library_address)
                if is_import:
                    if attributes != 9:
                        raise ValueError('Only optional PRX variable imports are prepared')
                    nids, refs = u32(cursor + 0x1C), u32(cursor + 0x20)
                    for i in range(variables):
                        nid, ref = u32(nids + i * 4), u32(refs + i * 4)
                        for _ in range(1024):
                            kind, slot, addend = struct.unpack_from('>III', image, offset(ref, 12))
                            if not kind:
                                break
                            if kind != 1 or addend or slot & 3 or not any(a <= slot and slot + 4 <= b for a, b in writable) or base + loads[0][1] <= slot < base + loads[0][1] + loads[0][3]:
                                raise ValueError('PRX variable reference requires an unprepared fixup')
                            imported.append((library, nid, slot)); ref += 12
                            if len(imported) > 4096:
                                raise ValueError('PRX variable references exceed the preparation limit')
                        else:
                            raise ValueError('Unterminated PRX variable references')
                else:
                    if attributes not in (1, 9):
                        raise ValueError('Unsupported PRX variable export attributes')
                    nids, addresses = u32(cursor + 0x14), u32(cursor + 0x18)
                    for i in range(variables):
                        nid, address = u32(nids + (funcs + i) * 4), u32(addresses + (funcs + i) * 4)
                        if not any(base + va <= address < base + va + ms for _, va, _, ms in loads):
                            raise ValueError('PRX variable export exceeds loaded memory')
                        exported.append((library, nid, address))
                        if len(exported) > 4096:
                            raise ValueError('PRX variable exports exceed the preparation limit')
            cursor += width
    if len(exported) > 4096 or len(imported) > 4096 or len({p[2] for p in imported}) != len(imported):
        raise ValueError('Invalid or duplicate PRX variable references')
    if len({(p[0], p[1]) for p in exported}) != len(exported):
        raise ValueError('Duplicate PRX variable exports')
    return sorted(exported), sorted(imported)


def firmware_artifacts(root, directory):
    directory = directory.resolve()
    receipt_path = directory / 'receipt.json'
    receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
    _, lock = dependency(root)
    key = receipt['key']
    if receipt.get('schema') != 1 or receipt.get('stage') != 'original-prx-aot-generated':
        raise ValueError('Invalid original PRX preparation receipt')
    if key['upstream_commit'] != lock['commit'] or key['adapter_sha256'] != digest(Path(__file__)) or key['guards_sha256'] != digest(Path(__file__).with_name('codegen.py')):
        raise ValueError('Original PRX preparation adapter changed')
    for name, expected in key['tools'].items():
        if Path(name).name != name or digest(root / 'external/ps3recomp/tools' / name) != expected:
            raise ValueError('Original PRX preparation tool changed')
    if set(key['tools']) != {'lift_prx.py', 'ppu_lifter.py', 'ppu_disasm.py'}:
        raise ValueError('Incomplete original PRX generator provenance')
    if not {'bundle.cpp', 'lift/ppu_recomp.c', 'lift/ppu_recomp.h'}.issubset(receipt['outputs']):
        raise ValueError('Original PRX preparation is incomplete')
    artifacts = {str(receipt_path): digest(receipt_path)}
    for name, expected in receipt['outputs'].items():
        path = (directory / name).resolve()
        if not path.is_relative_to(directory) or digest(path) != expected:
            raise ValueError('Original PRX prepared artifact changed or escaped its directory')
        artifacts[str(path)] = expected
    for name, expected in receipt['inputs'].items():
        if digest(Path(name)) != expected:
            raise ValueError('Original PRX input revision changed')
        artifacts[name] = expected
    return artifacts


def prepare_prx(root, source, cache, base, library, firmware=None, module_start=False, additional_libraries=()):
    source, cache = source.resolve(), cache.resolve()
    if source.is_relative_to(cache):
        raise ValueError('Original PRX must remain outside its preparation cache')
    if not isinstance(additional_libraries, (tuple, list)):
        raise ValueError('Additional original PRX libraries must be a sequence')
    libraries = (library, *additional_libraries)
    if any(not isinstance(name, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name) for name in libraries) or len(libraries) > 33 or len(set(libraries)) != len(libraries):
        raise ValueError('Invalid original PRX library name')
    sdk, lock = dependency(root)
    tools = sdk / 'tools'
    loads = validate_prx(source, base)
    inputs = {str(source): digest(source)}
    if firmware is not None:
        inputs[str(firmware.resolve())] = digest(firmware)
    key = {'source_sha256': inputs[str(source)], 'base': base, 'library': library,
           'module_start': bool(module_start),
           'firmware_sha256': digest(firmware) if firmware is not None else None,
           'upstream_commit': lock['commit'], 'adapter_sha256': digest(Path(__file__)),
           'guards_sha256': digest(Path(__file__).with_name('codegen.py')),
           'tools': {name: digest(tools / name) for name in ('lift_prx.py', 'ppu_lifter.py', 'ppu_disasm.py')}}
    if additional_libraries:
        key['additional_libraries'] = list(additional_libraries)
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()
    if destination.exists():
        firmware_artifacts(root, destination)
        return destination
    stage = Path(tempfile.mkdtemp(prefix='.pending-', dir=cache))
    try:
        log = stage / 'prepare.log'
        def run(args):
            with log.open('ab') as output:
                subprocess.run([sys.executable, *map(str, args)], stdout=output, stderr=subprocess.STDOUT,
                               check=True, timeout=120)
            if log.stat().st_size > 1024 * 1024:
                raise ValueError('Original PRX preparation log budget exceeded')
        relocated = stage / 'relocated'
        run([tools / 'lift_prx.py', source, '--base', hex(base), '--output', relocated])
        stem = source.stem
        exports = json.loads((relocated / f'{stem}_exports.json').read_text())
        imports = json.loads((relocated / f'{stem}_imports.json').read_text())
        image = (relocated / f'{stem}_image.bin').read_bytes()
        variable_exports, variable_imports = data_interfaces(source, loads, image, base)
        data_libraries = {p[0] for p in variable_exports}
        if any(name not in exports['exports'] and name not in data_libraries for name in libraries) or ('!syslib' in exports['exports'] and not module_start):
            raise ValueError('Selected PRX library is missing or requires an unimplemented module-start contract')
        start_opd = 0
        if '!syslib' in exports['exports']:
            start = exports['exports']['!syslib'].get('0xBC9A0086')
            if not start:
                raise ValueError('PRX lifecycle metadata has no supported module-start entry')
            start_opd = int(start, 16)
        variable_exports = [p for p in variable_exports if p[0] in libraries]
        selected = sorted((int(nid, 16), int(opd, 16)) for name in libraries for nid, opd in exports['exports'].get(name, {}).items())
        if len({nid for nid, _ in selected}) != len(selected):
            raise ValueError('Original PRX selected libraries have colliding NIDs')
        import_nids = {int(nid, 16) for entries in imports.values() for nid in entries}
        if import_nids & {nid for nid, _ in selected}:
            raise ValueError('PRX import/export NID ownership collision')
        _, text_va, text_size, _ = loads[0]
        for entries in imports.values():
            for address in entries.values():
                stub = int(address, 16)
                if stub & 3 or not base + text_va <= stub <= base + text_va + text_size - 16:
                    raise ValueError('PRX import stub exceeds executable code')
        for nid, opd in selected + ([(0xBC9A0086, start_opd)] if start_opd else []):
            offset = opd - base
            if not 0 <= offset <= len(image) - 8:
                raise ValueError('PRX export descriptor exceeds its image')
            code, toc = struct.unpack_from('>II', image, offset)
            if code & 3 or not any(base + va <= code < base + va + fs for _, va, fs, _ in loads) or toc != int(exports['toc'], 16):
                raise ValueError('Invalid PRX export code/TOC descriptor')
        stubs = relocated / 'hle_stubs.json'
        stubs.write_text(json.dumps([{'library': lib, 'nid': nid, 'stub': address}
                                    for lib, entries in imports.items() for nid, address in entries.items()]))
        lifted = stage / 'lift'
        run([tools / 'ppu_lifter.py', relocated / f'{stem}_image.bin', '--raw', '--base', hex(base),
             '--toc', exports['toc'], '--functions', relocated / f'{stem}_functions.json', '--hle-stubs', stubs,
             '--single-file', '--jobs', '1', '--output', lifted])
        guard_sources(lifted)
        source_path = lifted / 'ppu_recomp.c'
        code = source_path.read_text(encoding='utf-8')
        table = code.split('const func_entry function_table[] = {', 1)[1].split('\n};', 1)[0]
        functions = sorted((int(address, 16), name) for address, name in re.findall(
            r'\{\s*0x([0-9A-Fa-f]+)ULL,\s*(func_[0-9A-Fa-f_]+),', table))
        if not functions or len(functions) != len({address for address, _ in functions}):
            raise ValueError('Invalid generated PRX function table')
        if any(not base <= address < base + len(image) for address, _ in functions):
            raise ValueError('Generated PRX function exceeds its image')
        prefix = 'prx_' + inputs[str(source)][:16] + '_'
        for path in (source_path, lifted / 'ppu_recomp.h'):
            text = path.read_text(encoding='utf-8')
            text = re.sub(r'\bfunction_table_count\b', prefix + 'function_table_count', text)
            text = re.sub(r'\bfunction_table\b', prefix + 'function_table', text)
            path.write_text(text, encoding='utf-8', newline='\n')
        bundle = ['#include "lift/ppu_recomp.h"', '#include "native_lle.h"', 'static const uint8_t image[] = {']
        bundle.extend(','.join(f'0x{byte:02X}' for byte in image[i:i + 32]) + ',' for i in range(0, len(image), 32))
        bundle += ['};', 'static const NativeLleFunction functions[] = {']
        bundle.extend(f'{{0x{address:08X}u, {name}}},' for address, name in functions)
        bundle += ['};']
        if selected:
            bundle += ['static const NativeLleExport exports[] = {']
            bundle.extend(f'{{0x{nid:08X}u, 0x{opd:08X}u}},' for nid, opd in selected)
            bundle += ['};']
        for name, kind, values in [('data_exports', 'NativeLleDataExport', variable_exports), ('data_imports', 'NativeLleDataImport', variable_imports)]:
            if values:
                bundle += [f'static const {kind} {name}[] = {{']
                bundle.extend(f'{{{json.dumps(lib)}, 0x{nid:08X}u, 0x{address:08X}u}},' for lib, nid, address in values)
                bundle += ['};']
        if additional_libraries:
            bundle += ['static const char* const additional_libraries[] = {' + ','.join(json.dumps(name) for name in additional_libraries) + '};']
        extra = ', additional_libraries, sizeof(additional_libraries)/sizeof(additional_libraries[0])' if additional_libraries else ', nullptr, 0'
        for name, values in [('data_exports', variable_exports), ('data_imports', variable_imports)]:
            extra += f', {name}, sizeof({name})/sizeof({name}[0])' if values else ', nullptr, 0'
        function_exports = 'exports, sizeof(exports)/sizeof(exports[0])' if selected else 'nullptr, 0'
        bundle += ['static const NativeLleModule original_module = {',
                   f'{json.dumps(exports["module"])}, {json.dumps(library)}, 0x{base:08X}u, sizeof(image), image,',
                   f'functions, sizeof(functions)/sizeof(functions[0]), {function_exports}, 0x{start_opd:08X}u{extra}', '};',
                   'extern "C" const NativeLleModule* ps3_native_firmware_definition(void) { return &original_module; }', '']
        (stage / 'bundle.cpp').write_text('\n'.join(bundle), encoding='utf-8', newline='\n')
        if any(digest(Path(name)) != expected for name, expected in inputs.items()):
            raise ValueError('Original PRX input changed during preparation')
        outputs = {p.relative_to(stage).as_posix(): digest(p) for p in sorted(stage.rglob('*')) if p.is_file()}
        receipt = {'schema': 1, 'stage': 'original-prx-aot-generated', 'key': key, 'inputs': inputs,
                   'outputs': outputs, 'module': exports['module'], 'library': library,
                   'functions': len(functions), 'exports': len(selected), 'gameplay_verified': False}
        receipt['data_exports'] = len(variable_exports)
        receipt['data_imports'] = len(variable_imports)
        (stage / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        stage.rename(destination)
        return destination
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--base', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--library', required=True)
    parser.add_argument('--additional-library', action='append', default=[])
    parser.add_argument('--module-start', action='store_true')
    parser.add_argument('--firmware', type=Path)
    args = parser.parse_args()
    print(prepare_prx(Path(__file__).resolve().parents[2], args.source, args.cache, args.base, args.library, args.firmware, args.module_start, args.additional_library))
