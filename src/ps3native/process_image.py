"""Compile-time process binding of prepared AOT; originals and caches stay immutable."""
import argparse
import json
from pathlib import Path
import re

from ps3native.catalog import digest


def transform(text, prefix):
    if not re.fullmatch(r'image_[0-9a-f]{16}', prefix):
        raise ValueError('Invalid AOT image identifier')
    # Generated code only reads the host VM base. Keep the C ABI declaration.
    text = re.sub(r'\buint8_t\s*\*\s*vm_base\s*;',
                  'uint8_t* guest_process_image_base(void);', text)
    if re.search(r'\bvm_base\s*=(?!=)|&\s*vm_base\b', text):
        raise ValueError('AOT image attempts to replace/address the host VM base')
    text = re.sub(r'\bvm_base\b', 'guest_process_image_base()', text)
    text = re.sub(r'\bfunc_([0-9A-F]{8})\b', prefix + r'_func_\1', text)
    text = re.sub(r'\bfunction_table(_count)?\b', lambda m: prefix + '_' + m[0], text)
    return text


def compile_view(directory, output):
    directory, output = directory.resolve(), output.resolve()
    if output == directory or directory in output.parents or output in directory.parents:
        raise ValueError('Compile view must be separate from prepared input')
    receipt = json.loads((directory / 'receipt.json').read_text(encoding='utf-8'))
    if receipt.get('stage') != 'ppu-aot-generated' or not receipt.get('outputs'):
        raise ValueError('Expected a prepared PPU AOT image')
    for name, expected in receipt['outputs'].items():
        if Path(name).name != name or digest(directory / name) != expected:
            raise ValueError('Prepared PPU AOT artifact changed')
    prefix = 'image_' + receipt['key']['input_sha256'][:16]
    files = sorted(p for p in directory.glob('ppu_recomp*.*') if p.suffix in ('.h', '.c', '.cpp'))
    if not files or not (directory / 'ppu_recomp.h').is_file():
        raise ValueError('Prepared AOT sources are missing')
    if any(p.name not in receipt['outputs'] for p in files):
        raise ValueError('AOT source is absent from the preparation receipt')
    expected_names = {p.name for p in files} | {'register.cpp', 'process-image.json'}
    if output.exists():
        if any(not p.is_file() or p.name not in expected_names for p in output.iterdir()):
            raise ValueError('Compile view contains unexpected artifacts')
        if any(output.iterdir()):
            previous = json.loads((output / 'process-image.json').read_text(encoding='utf-8'))
            if previous.get('preparation_receipt') != str(directory / 'receipt.json'):
                raise ValueError('Compile view belongs to a different preparation')
            for name, expected in previous['outputs'].items():
                if name not in expected_names or digest(output / name) != expected:
                    raise ValueError('Compile view artifact changed')
    output.mkdir(parents=True, exist_ok=True)
    outputs = {}
    for source in files:
        target = output / source.name
        text = transform(source.read_text(encoding='utf-8'), prefix)
        if not target.exists() or target.read_text(encoding='utf-8') != text:
            target.write_text(text, encoding='utf-8', newline='\n')
        outputs[target.name] = digest(target)
    registration = output / 'register.cpp'
    text = ('#include "ppu_recomp.h"\n'
            'extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*));\n'
            f'extern "C" void {prefix}_register(void) {{\n'
            f'  for (uint64_t i = 0; i < {prefix}_function_table_count; ++i)\n'
            f'    ppu_register_function({prefix}_function_table[i].addr, {prefix}_function_table[i].func);\n'
            '}\n')
    if not registration.exists() or registration.read_text(encoding='utf-8') != text:
        registration.write_text(text, encoding='utf-8', newline='\n')
    outputs[registration.name] = digest(registration)
    result = {'schema': 1, 'prefix': prefix, 'preparation_receipt': str(directory / 'receipt.json'),
              'preparation_sha256': digest(directory / 'receipt.json'),
              'adapter_sha256': digest(Path(__file__)), 'outputs': outputs}
    (output / 'process-image.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    compile_view(args.directory, args.output)
