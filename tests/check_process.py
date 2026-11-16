"""Unported guest-process services must stop before touching root state."""
import subprocess
import sys

for mode, boundary in [('hle', b'HLE'), ('lv2', b'LV2/SPU/RSX'), ('prx', b'PRX')]:
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=10)
    if result.returncode != 86 or b'isolated process ' + boundary not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-1000:]))
print('Unported isolated-process HLE, LV2/SPU/RSX and PRX services rejected')
