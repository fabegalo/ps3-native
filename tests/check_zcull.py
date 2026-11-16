import subprocess
import sys

for mode, reason in (('zcull_uninitialized', b'needs guest depth/stencil initialization'),
                     ('zcull_format', b'does not cover the depth surface format or extent'),
                     ('report_unmapped', b'RSX report unsupported or invalid'),
                     ('report_misaligned', b'RSX report unsupported or invalid'),
                     ('conditional_unmapped', b'invalid conditional render'),
                     ('conditional_mode', b'invalid conditional render')):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=20)
    if result.returncode != 86 or reason not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
print('GPU/FIFO: rejected uninitialized/incompatible depth, invalid report addresses and conditional modes')
