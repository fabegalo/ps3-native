import subprocess
import sys

for mode in ('pc', 'stack', 'unbacked', 'thread', 'unvisited', 'expired', 'context'):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=10)
    if result.returncode != 86 or b'unprepared AOT nonlocal return' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-1000:]))
print('Rejected unknown PCs, wrong/unbacked stacks, cross-thread/context, unvisited and expired frames')
