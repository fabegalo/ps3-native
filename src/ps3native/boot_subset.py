"""Bounded offline AOT startup slice; missing code stops, never interprets."""
import json
from pathlib import Path
import re
import struct


def pointer_table_seeds(elf):
    entry = elf.elf_header.e_entry
    section = next(s for s in elf.section_headers if s.sh_addr <= entry < s.sh_addr + s.sh_size)
    data = elf.raw_data[section.sh_offset:section.sh_offset + section.sh_size]
    opds = {section.sh_addr + i: struct.unpack_from('>I', data, i)[0] for i in range(0, len(data) - 7, 8)}
    seeds = set()
    for i, s in enumerate(elf.section_headers):
        # Small allocated pointer tables include constructors in stripped ELFs.
        # Extra AOT preparation does not execute a candidate pointer.
        if s.sh_flags & 2 and not s.sh_flags & 4 and 8 <= s.sh_size <= 4096 and s.sh_type != 8 and s.sh_size % 4 == 0:
            words = struct.unpack('>' + str(s.sh_size // 4) + 'I', elf.get_section_data(i))
            valid = [w for w in words if w in opds]
            if len(valid) >= 2 and sum(w in opds or w in (0, 0xffffffff) for w in words) >= len(words) * 0.9:
                seeds.update(opds[w] for w in valid)
    return sorted(seeds)


def select_boot(directory, entry, limit=256, seeds=(), pages=()):
    directory = Path(directory)
    index, calls, preamble = {}, {}, None
    pattern = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{', re.M)
    for path in sorted(directory.glob('ppu_recomp*.*')):
        if path.suffix not in ('.c', '.cpp'):
            continue
        text = path.read_text(encoding='utf-8')
        matches = list(pattern.finditer(text))
        if matches and preamble is None:
            preamble = text[:matches[0].start()]
        for match in matches:
            end = text.index('\n}', match.end()) + 2
            address = int(match[1], 16)
            index[address] = (path, match.start(), end)
            calls[address] = {int(a, 16) for a in re.findall(r'func_([0-9A-F]{8})', text[match.end():end])}
    if entry not in index:
        raise ValueError('Entry code is absent from AOT output')
    if any(a not in index for a in seeds):
        raise ValueError('A requested PPU target has no upstream AOT body; analyze its boundary before continuing')
    page_ids = {p >> 16 for p in pages}
    nearby = [a for a in sorted(index) if a >> 16 in page_ids]
    pending = [entry, *pages, *nearby, *seeds]
    limit = max(limit, len(set(pending)))
    selected = set()
    while pending and len(selected) < limit:
        address = pending.pop(0)
        if address in selected or address not in index:
            continue
        selected.add(address)
        pending.extend(sorted(calls[address] - selected))
    output = directory / 'boot'
    output.mkdir(exist_ok=True)
    header = (directory / 'ppu_recomp.h').read_bytes()
    header_path = output / 'ppu_recomp.h'
    if not header_path.exists() or header_path.read_bytes() != header:
        header_path.write_bytes(header)
    chunks = [preamble]
    loaded = {}
    bodies = {}
    for address in sorted(selected):
        path, start, end = index[address]
        if path not in loaded:
            loaded[path] = path.read_text(encoding='utf-8')
        bodies.setdefault(address >> 18, []).append(loaded[path][start:end])
    for previous in output.glob('ppu_recomp_boot*.cpp'):
        if re.fullmatch(r'ppu_recomp_boot(?:_\d+)?\.cpp', previous.name):
            previous.unlink()  # Exact earlier selector outputs, not upstream or user files.
        elif re.fullmatch(r'ppu_recomp_boot_a[0-9a-f]{4}\.cpp', previous.name) and int(previous.stem[-4:], 16) not in bodies:
            previous.unlink()
    for number, group in bodies.items():
        text = preamble + '\n'.join(group)
        path = output / f'ppu_recomp_boot_a{number:04x}.cpp'
        if not path.exists() or path.read_text(encoding='utf-8') != text:
            path.write_text(text, encoding='utf-8', newline='\n')
    external = set().union(*(calls[a] for a in selected)) - selected
    chunks.append('extern "C" void ppu_unlifted_stub(uint64_t, ppu_context*);')
    for address in sorted(external):
        chunks.append(f'void func_{address:08X}(ppu_context* ctx) {{ ppu_unlifted_stub(0x{address:08X}, ctx); }}')
    chunks.append('const func_entry function_table[] = {')
    chunks.extend(f'{{0x{a:08X}, func_{a:08X}, "func_{a:08X}"}},' for a in sorted(selected))
    chunks.append('{0, nullptr, nullptr}};')
    chunks.append(f'const uint64_t function_table_count = {len(selected)};')
    table = '\n'.join(chunks)
    table_path = output / 'ppu_recomp_boot_table.cpp'
    if not table_path.exists() or table_path.read_text(encoding='utf-8') != table:
        table_path.write_text(table, encoding='utf-8', newline='\n')
    (output / 'selection.json').write_text(json.dumps({'entry': entry, 'selected': sorted(selected),
                                                    'unprepared_targets': sorted(external)}), encoding='utf-8')
    return output
