"""Read-only content identification. Hashes identify bytes, never entitlement."""
import hashlib
import json
import os
from pathlib import Path
import stat
import struct


def digest(path):
    before = path.stat()
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError(f'Content changed while hashing: {path}')
    return h.hexdigest()


def sfo(path):
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('PARAM.SFO exceeds 1 MiB metadata limit')
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError('Truncated PARAM.SFO')
    magic, version, keys, values, count = struct.unpack_from('<4s4I', data)
    if magic != b'\x00PSF' or version != 0x101 or not 20 + count * 16 <= keys <= values <= len(data):
        raise ValueError('Invalid PARAM.SFO header')
    result = {}
    for i in range(count):
        key, fmt, length, maximum, offset = struct.unpack_from('<HHIII', data, 20 + i * 16)
        start = keys + key
        end = data.find(b'\0', start, values)
        if not keys <= start < values or end < 0 or length > maximum or values + offset + maximum > len(data):
            raise ValueError('Invalid PARAM.SFO entry bounds')
        name = data[start:end].decode('utf-8')
        raw = data[values+offset:values+offset+length]
        if name in result:
            raise ValueError('Duplicate PARAM.SFO key')
        if fmt == 0x204:
            if not raw or raw[-1] != 0:
                raise ValueError('Unterminated PARAM.SFO string')
            result[name] = raw[:-1].decode('utf-8')
        elif fmt == 0x404 and length == 4:
            result[name] = int.from_bytes(raw, 'little')
        elif fmt == 0x4:
            result[name] = {'binary_bytes': length}
        else:
            raise ValueError(f'Unsupported PARAM.SFO format: {fmt:#x}')
    return result


def pup(path):
    size = path.stat().st_size
    with path.open('rb') as stream:
        header = stream.read(48)
        if len(header) != 48 or header[:8] != b'SCEUF\0\0\0':
            raise ValueError('Invalid PUP magic/header')
        package, image, count, header_size, data_size = struct.unpack('>5Q', header[8:])
        if not 0 < count <= 4096 or not 48 + count * 64 <= header_size <= size or data_size > size - header_size:
            raise ValueError('Invalid PUP table/size')
        entries, spans, ids = [], [], set()
        for _ in range(count):
            entry_id, offset, length = struct.unpack('>3Q8x', stream.read(32))
            if entry_id in ids or offset < header_size or offset + length > header_size + data_size:
                raise ValueError('Invalid PUP entry')
            ids.add(entry_id)
            spans.append((offset, offset + length))
            entries.append({'id': entry_id, 'offset': offset, 'size': length})
        spans.sort()
        if any(left[1] > right[0] for left, right in zip(spans, spans[1:])):
            raise ValueError('Overlapping PUP entries')
        for entry in entries:
            stream.seek(entry['offset'])
            entry['magic_hex'] = stream.read(min(8, entry['size'])).hex()
            entry['state'] = 'container-entry; not decrypted or recompiled'
        version_entry = next((e for e in entries if e['id'] == 0x100), None)
        version = None
        if version_entry and version_entry['size'] <= 256:
            stream.seek(version_entry['offset'])
            version = stream.read(version_entry['size']).decode('ascii').strip('\0\n\r ')
    return {'package_version': package, 'image_version': image, 'version_text': version,
            'entries': entries, 'authentication': 'not verified; SHA-256 identity only',
            'execution': 'unavailable; import is not firmware execution'}


def linked(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def inventory(source):
    source = Path(source).absolute()
    # Canonicalize parent aliases (macOS /var -> /private/var); reject the
    # selected root and all links inside it, where traversal changes content.
    if linked(source):
        raise ValueError(f'Links/reparse points are not imported: {source}')
    source = source.resolve(strict=True)
    paths = []
    if source.is_dir():
        for parent, dirs, files in os.walk(source, followlinks=False):
            for name in dirs + files:
                if linked(Path(parent) / name):
                    raise ValueError('Content tree contains links/reparse points')
            for name in files:
                paths.append(Path(parent) / name)
                if len(paths) > 100000:
                    raise ValueError('Inventory exceeds 100000 files; split the input')
    else:
        paths = [source]
    if not paths:
        raise ValueError('Empty content directory')
    records, metadata = [], {}
    for path in sorted(paths):
        before = path.stat()
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('Only regular files are supported')
        with path.open('rb') as stream:
            magic = stream.read(8)
        kind = ('pup' if magic == b'SCEUF\0\0\0' else
                'self-encrypted' if magic[:4] == b'SCE\0' else
                'elf-readable' if magic[:4] == b'\x7fELF' else
                'pkg-container' if magic[:4] == b'\x7fPKG' else
                'sfo' if magic[:4] == b'\0PSF' else 'data')
        relative = path.relative_to(source).as_posix() if source.is_dir() else path.name
        file_hash = digest(path)
        if kind == 'sfo':
            metadata[relative] = sfo(path)
        elif kind == 'pup':
            metadata[relative] = pup(path)
        after = path.stat()
        if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
            raise ValueError('Content changed during inventory')
        records.append({'path': relative, 'size': before.st_size, 'sha256': file_hash, 'kind': kind})
    identity = hashlib.sha256(json.dumps(records, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return {'schema': 1, 'id': identity, 'source': str(source), 'files': records, 'metadata': metadata,
            'state': 'inventoried', 'runnable': False}
