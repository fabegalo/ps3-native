"""Own PRX: add a shared-stack argument to a value reached through its TOC."""
import struct


def own_prx(module_start=False):
    data = bytearray(0x390)
    data[:16] = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH', data, 16, 0xFFA4, 21, 1, 0, 64, 0, 0, 64, 56, 3, 64, 0, 0)
    struct.pack_into('>IIQQQQQQ', data, 64, 1, 5, 0x100, 0, 0x120, 0x100, 0x100, 0x80)
    struct.pack_into('>IIQQQQQQ', data, 120, 1, 6, 0x200, 0x100, 0, 0x100, 0x100, 0x80)
    struct.pack_into('>IIQQQQQQ', data, 176, 0x700000A4, 0, 0x300, 0, 0, 0x90, 0, 0x10)
    data[0x124:0x130] = b'ownSynthetic'
    struct.pack_into('>IIIII', data, 0x140, 0x8180, 0x100, 0x11C, 0x11C, 0x11C)
    struct.pack_into('>IIII', data, 0x180, 0xE8610070, 0xE8828000, 0x7C632214, 0x4E800020)
    data[0x200] = 0x1C
    struct.pack_into('>H', data, 0x206, 1)
    struct.pack_into('>III', data, 0x210, 0x140, 0x160, 0x164)
    data[0x240:0x24C] = b'ownSynthetic'
    struct.pack_into('>II', data, 0x260, 0xD00DFEED, 0x170)
    struct.pack_into('>IIQ', data, 0x270, 0x80, 0x8180, 0)
    struct.pack_into('>Q', data, 0x280, 1)
    relocations = [(0x10, 1, 0x40), (0x14, 1, 0x60), (0x18, 1, 0x64),
                   (0x64, 1, 0x70), (0x70, 0, 0x80), (0x74, 0, 0x8180)]
    if module_start:
        data.extend(bytes(120))
        struct.pack_into('>Q', data, 208, 264)
        struct.pack_into('>I', data, 0x148, 0x138)
        data[0x21C] = 0x1C
        struct.pack_into('>H', data, 0x222, 1)
        struct.pack_into('>II', data, 0x230, 0x190, 0x194)
        struct.pack_into('>IIII', data, 0x290, 0xBC9A0086, 0x198, 0x90, 0x8180)
        struct.pack_into('>5I', data, 0x190, 0x80A2800C, 0x38A50001, 0x90A2800C, 0x38600000, 0x4E800020)
        relocations.extend([(0x30, 1, 0x90), (0x34, 1, 0x94), (0x94, 1, 0x98),
                            (0x98, 0, 0x90), (0x9C, 0, 0x8180)])
    for i, (offset, value_segment, addend) in enumerate(relocations):
        struct.pack_into('>QHBBIQ', data, 0x300 + i * 24, offset, 0, value_segment, 1, 1, addend)
    return bytes(data)
