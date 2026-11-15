import subprocess
import sys

for mode in ('hle', 'ppu', 'indirect', 'syscall', 'unaudited_syscall', 'spu', 'fs_not_ready',
             'spu_indirect', 'spu_missing', 'spu_missing_async', 'spu_missing_job', 'sysutil_no_dispatcher',
             'provider_not_registered', 'gcm_unaudited', 'tile_compressed', 'io_unbacked',
             'callback_missing', 'callback_ct_missing', 'thread_flags', 'thread_exit_unowned', 'raw_spu_unreviewed',
             'audited_not_registered', 'once_invalid', 'spurs_missing_policy', 'spurs_poll'):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'[native-blocked]' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[2], 'unsupported'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] unsupported SPU instruction' not in result.stderr:
    raise AssertionError(('SPU unsupported instruction', result.returncode, result.stderr[-2000:]))
print('Verified HLE, direct/indirect PPU, LV2 and SPU interpreter/instruction fail-closed paths')
