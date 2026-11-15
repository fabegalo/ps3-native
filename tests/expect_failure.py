"""Negative controls must fail their oracle, rather than crash or time out."""
import subprocess
import sys

result = subprocess.run(sys.argv[1:], capture_output=True, text=True, timeout=30)
print(result.stdout)
if result.returncode != 1:
    print(f'Expected exactly exit 1, got {result.returncode}', file=sys.stderr)
    raise SystemExit(1)
