"""Own instruction fixtures, lifted by the locked upstream tools at build time."""
import json
import hashlib
from pathlib import Path
import struct
import subprocess
import sys

root, out = map(Path, sys.argv[1:])
tools = root / 'external/ps3recomp/tools'
out.mkdir(parents=True, exist_ok=True)
code = struct.pack('>5I', 0x3C800002, 0x38600013, 0x38630017, 0x90640010, 0x4E800020)
elf = bytearray(0x300)
elf[:64] = struct.pack('>16sHHIQQQIHHHHHH', b'\x7fELF\x02\x02\x01' + bytes(9),
                      2, 21, 1, 0x20000, 64, 0x200, 0, 64, 56, 3, 64, 3, 0)
for i, (off, addr, size, flags) in enumerate([(0x100, 0x10000, len(code), 5), (0x180, 0x20000, 32, 6)]):
    elf[64+i*56:120+i*56] = struct.pack('>IIQQQQQQ', 1, flags, off, addr, addr, size, size, 16)
elf[0x100:0x100+len(code)] = code
elf[0x180:0x188] = struct.pack('>II', 0x10000, 0x20000)
elf[176:232] = struct.pack('>IIQQQQQQ', 0x60000001, 0, 0x1A0, 0, 0, 32, 32, 4)
elf[0x1A0:0x1C0] = struct.pack('>8I', 32, 0x13BCC5F6, 1, 0x12345678, 1001, 0x100000, 0x100000, 0)
for i, (addr, off, size, flags) in enumerate([(0x10000, 0x100, len(code), 6), (0x20000, 0x180, 8, 3)], 1):
    elf[0x200+i*64:0x240+i*64] = struct.pack('>IIQQQQIIQQ', 0, 1, flags, addr, off, size, 0, 0, 4, 0)
(out / 'ppu.elf').write_bytes(elf)

def run(tool, *args):
    subprocess.run([sys.executable, str(tools / tool), *map(str, args)], check=True, timeout=60)

run('ppu_loader.py', out / 'ppu.elf', '--output', out)
run('ppu_lifter.py', out / 'ppu.elf', '--functions', out / 'ppu.functions.json',
    '--single-file', '--output', out)

# SPU GET -> LS -> increment word -> PUT -> stop. Channel encodings are ISA words.
words = []
def il(reg, value):
    words.append((0x81 << 23) | ((value & 0xffff) << 7) | reg)
def wrch(channel, reg):
    words.append((0x10d << 21) | (channel << 7) | reg)
for channel, value in [(16, 0x100), (17, 0), (18, 0x20000), (19, 16), (20, 1), (21, 0x40)]:
    # ila supplies the 18-bit EA; other constants fit il.
    if value == 0x20000:
        words.append((0x21 << 25) | (value << 7) | 3)
    else:
        il(3, value)
    wrch(channel, 3)
# lqd r10, 0x100(r9); ai r10,r10,1; stqd r10,0x100(r9).
il(9, 0)
words += [(0x34 << 24) | (0x10 << 14) | (9 << 7) | 10,
          (0x1c << 24) | (1 << 14) | (10 << 7) | 10,
          (0x24 << 24) | (0x10 << 14) | (9 << 7) | 10]
il(3, 0x20)
wrch(21, 3)
words.append(0)
(out / 'spu.bin').write_bytes(struct.pack(f'>{len(words)}I', *words))
(out / 'spu_bytes.h').write_text('static const unsigned char own_spu_bytes[] = {' +
    ','.join(str(b) for b in (out / 'spu.bin').read_bytes()) + '};\n')
run('spu_lifter.py', out / 'spu.bin', '--output', out)
# Upstream emits checkout-relative includes; generated output lives in our build tree.
for name in ('spu_recomp.h', 'spu_recomp.c'):
    path = out / name
    path.write_text(path.read_text().replace('../../runtime/spu/', ''), encoding='utf-8')
artifacts = {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in
             ('ppu.elf', 'ppu_recomp.c', 'ppu_recomp.h', 'spu.bin', 'spu_recomp.c', 'spu_recomp.h')}
(out / 'fixture.json').write_text(json.dumps({'provenance': 'ps3-native synthetic',
                                             'ppu_result': 42, 'artifacts': artifacts}), encoding='utf-8')

# Independent LV2 thread: wait for a mailbox word, publish it as the exit status.
thread_words = [(0x00d << 21) | (29 << 7) | 7, (0x10d << 21) | (28 << 7) | 7, 0x102, 0x40200000]
thread = out / 'thread'
thread.mkdir(exist_ok=True)
(thread / 'code.bin').write_bytes(struct.pack('>4I', *thread_words))
run('spu_lifter.py', thread / 'code.bin', '--base', '0x100', '--symbol-prefix', 'thread_', '--output', thread)
for name in ('spu_recomp.h', 'spu_recomp.c'):
    path = thread / name
    path.write_text(path.read_text().replace('../../runtime/spu/', ''), encoding='utf-8')

sys.path.insert(0, str(root / 'src'))
from lle_fixture import own_prx
from ps3native.firmware import prepare_prx
prx = out / 'own.prx'
prx.write_bytes(own_prx(module_start=True))
prepared = prepare_prx(root, prx, out / 'lle-cache', 0x08000000, 'ownSynthetic', module_start=True)
(out / 'lle_bridge.cpp').write_text('#include "' + (prepared / 'bundle.cpp').as_posix() + '"\n')
(out / 'lle_recomp.cpp').write_text('#include "' + (prepared / 'lift/ppu_recomp.c').as_posix() + '"\n')

# A real lifted call into two offline variants sharing one local-store address.
sys.path.insert(0, str(tools))
from wrap_spu_elf import wrap
variants = out / 'variants'
variants.mkdir(exist_ok=True)
target = 0x3FEC0
owner_bytes = struct.pack('>II', 0x33000000 | ((((target - 0x100) // 4) & 0xFFFF) << 7), 0)
payloads = {'owner': owner_bytes, 'first': struct.pack('>II', 0x40800383, 0x35000000),
            'second': struct.pack('>II', 0x40800483, 0x35000000)}
images = {name: wrap(data, base=0x100 if name == 'owner' else target) for name, data in payloads.items()}
hashes = {name: hashlib.sha256(data).hexdigest() for name, data in images.items()}
for name, data in images.items(): (variants / (name + '.elf')).write_bytes(data)
bindings = variants / 'bindings.json'
bindings.write_text(json.dumps({'schema_version':1, 'variants':[
    {'owner_sha256':hashes['owner'], 'variant_sha256':hashes[name]} for name in ('first','second')]}))
run('build_spu_workloads.py', '--input', variants / 'owner.elf', '--input', variants / 'first.elf',
    '--input', variants / 'second.elf', '--code-variants', bindings, '--lifted', variants / 'aot',
    '--out', variants / 'aot/registry.c', '--register-fn', 'own_variants_register', '--constructor')
report = json.loads((variants / 'aot/inventory.json').read_text())
assert report['complete'] and len(report['programs']) == 3 and len(report['code_variants']) == 2
for name in images:
    path = variants / 'aot' / ('spu_' + hashes[name]) / 'spu_recomp.c'
    (variants / (name + '.c')).write_text('#include "' + path.as_posix() + '"\n')
(variants / 'registry.c').write_text('#include "' + (variants / 'aot/registry.c').as_posix() + '"\n')
owner_id = next(p['image_id'] for p in report['programs'] if p['sha256'] == hashes['owner'])
meta = f'#define OWN_VARIANT_OWNER_ID {owner_id}\n#define OWN_VARIANT_ENTRY {target}\n'
for name, data in payloads.items():
    meta += f'static const unsigned char own_variant_{name}[] = {{' + ','.join(str(x) for x in data) + '};\n'
(variants / 'meta.h').write_text(meta)
