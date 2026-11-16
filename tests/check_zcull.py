import subprocess
import sys

for mode, reason in (('zcull_uninitialized', b'needs guest depth/stencil initialization'),
                     ('zcull_format', b'does not cover the depth surface format or extent')):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=20)
    if result.returncode != 86 or reason not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
print('Real GPU: rejected uninitialized ZCULL depth/stencil and incompatible surface format')
