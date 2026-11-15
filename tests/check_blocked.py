import subprocess
import sys

for mode in ('hle', 'ppu', 'indirect', 'syscall', 'unaudited_syscall', 'spu', 'fs_not_ready',
             'spu_indirect', 'spu_missing', 'spu_missing_async', 'spu_missing_job', 'sysutil_no_dispatcher',
             'provider_not_registered', 'gcm_unaudited', 'tile_compressed', 'io_unbacked',
             'callback_missing', 'callback_ct_missing', 'thread_flags', 'thread_exit_unowned', 'raw_spu_unreviewed',
             'audited_not_registered', 'once_invalid', 'spurs_missing_policy', 'spurs_poll',
             'memory_unbacked', 'memory_overlap', 'string_unterminated', 'spu_image_protected', 'sdk_missing', 'gcm_timestamp_index', 'gcm_prepared_multiple', 'gcm_prepared_backend_missing', 'rtc_calendar_invalid'):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'[native-blocked]' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[2], 'unsupported'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] unsupported SPU instruction' not in result.stderr:
    raise AssertionError(('SPU unsupported instruction', result.returncode, result.stderr[-2000:]))
print('Verified HLE, direct/indirect PPU, LV2 and SPU interpreter/instruction fail-closed paths')
result = subprocess.run([sys.argv[2], 'code_mutation'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] SPU compiled code changed' not in result.stderr:
    raise AssertionError(('SPU compiled code mutation', result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[2], 'overlay_mutation'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] SPU compiled code changed' not in result.stderr:
    raise AssertionError(('SPU streamed code mutation', result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[2], 'overlay_stale_return'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] SPU compiled code changed' not in result.stderr:
    raise AssertionError(('SPU return to retired code', result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[2], 'spurs_service_unprepared'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] unprepared SPU target=' not in result.stderr:
    raise AssertionError(('SPURS original service ownership', result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[2], 'stop_unowned'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] SPU halt without owned execution engine' not in result.stderr:
    raise AssertionError(('SPU stop execution ownership', result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[3], sys.argv[4], 'changed_export'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] original PRX export descriptor changed' not in result.stderr:
    raise AssertionError(('PRX export mutation', result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[3], sys.argv[4], 'missing_export'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] original cellSpurs owner has no prepared export' not in result.stderr:
    raise AssertionError(('PRX ownership fallback', result.returncode, result.stderr[-2000:]))
