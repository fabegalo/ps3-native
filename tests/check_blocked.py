import subprocess
import sys
import os
from pathlib import Path

for mode in ('atrac_missing_export', 'adec_missing_export'):
    result = subprocess.run([sys.argv[3], sys.argv[4], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'original ATRAC/ADEC owner has no prepared export' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))

for mode in ('http_missing_export', 'ssl_missing_export'):
    result = subprocess.run([sys.argv[3], sys.argv[4], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'original HTTP/SSL owner has no prepared export' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))

for mode in ('pending', 'reset'):
    result = subprocess.run([sys.argv[1], 'sm_params', mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'System Manager service 11 boot message is unavailable' not in result.stderr:
        raise AssertionError(('missing System Manager message', mode, result.returncode, result.stderr[-2000:]))

result = subprocess.run([sys.argv[1], 'mutex_named', 'open'], capture_output=True, timeout=15)
if result.returncode != 86 or b'shared or adaptive LV2 mutex requires implementation' not in result.stderr:
    raise AssertionError(('named mutex import', result.returncode, result.stderr[-2000:]))

result = subprocess.run([sys.argv[1], 'process_debug_unbound'], capture_output=True, timeout=15)
if result.returncode != 86 or b'requires original SELF control attributes' not in result.stderr:
    raise AssertionError(('unbound process control', result.returncode, result.stderr[-2000:]))

for mode, expected in (('opcode', b'filesystem control requires audit'),
                       ('mount', b'filesystem control C0000006 mount requires audit')):
    result = subprocess.run([sys.argv[1], 'fs_udf_query', mode], capture_output=True, timeout=15)
    if result.returncode != 86 or expected not in result.stderr:
        raise AssertionError(('filesystem control', mode, result.returncode, result.stderr[-2000:]))

for mode in ('hle', 'ppu', 'indirect', 'indirect_null', 'ppu_null_read', 'storage_budget', 'syscall', 'unaudited_syscall', 'mapper_named_pointer', 'rsx_memory_query', 'rsx_memory_attributes', 'rsx_context_flags', 'rsx_context_multiple', 'rsx_context_iomap_flags', 'rsx_context_iomap_overlap', 'rsx_context_attribute', 'rsx_context_fifo_pending', 'rsx_context_device_attribute', 'rsx_context_alternate_timing', 'rsx_context_sync_mode', 'rsx_context_tile_compressed', 'rsx_context_zcull_backend', 'rsx_context_zcull_overlap', 'rsx_context_semaphore_backend', 'rsx_context_semaphore_dma', 'rsx_context_device_release', 'rsx_context_main_surface', 'rsx_context_reference_backend', 'rsx_context_queue_backend', 'rsx_context_queue_metadata', 'rsx_context_scanout_backend', 'strncmp_invalid', 'video_clock_reverse', 'video_clock_edge', 'audio_probe', 'audio_option', 'mutex_shared', 'mutex_adaptive', 'mutex_queue_inherit', 'rw_contended', 'tty_channel', 'language_other', 'spu', 'fs_not_ready',
             'spu_indirect', 'spu_missing', 'spu_missing_async', 'spu_missing_job', 'sysutil_no_dispatcher',
             'provider_not_registered', 'gcm_unaudited', 'gcm_debug_level', 'tile_compressed', 'io_unbacked',
             'callback_missing', 'callback_ct_missing', 'thread_flags', 'thread_exit_unowned', 'raw_spu_unreviewed',
             'audited_not_registered', 'once_invalid', 'spurs_missing_policy', 'spurs_poll',
             'memory_unbacked', 'memory_overlap', 'string_unterminated', 'spu_image_protected', 'sdk_missing', 'gcm_timestamp_index', 'gcm_prepared_multiple', 'gcm_prepared_backend_missing', 'rtc_calendar_invalid'):
    result = subprocess.run([sys.argv[1], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'[native-blocked]' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
    if mode == 'strncmp_invalid' and b'invalid or unbounded string comparison' not in result.stderr:
        raise AssertionError((mode, 'comparison guard was not reached', result.stderr[-2000:]))
result = subprocess.run([sys.argv[1], 'tty'], capture_output=True, timeout=15)
payload = bytes(i % 251 for i in range(0x4011))
if result.returncode or result.stdout.count(payload) != 1:
    raise AssertionError(('TTY actual bytes/count, binary data, invalid pointers', result.returncode, len(result.stdout)))
for code, expected in (('pt-br', 17), ('en-gb', 18), ('zh-t', 10), ('ja', 0), ('19', 19)):
    result = subprocess.run([sys.argv[1], 'language'], capture_output=True, timeout=15,
                            env=dict(os.environ, PS3_LANG=code))
    if result.returncode or f'[language-value] {expected}'.encode() not in result.stdout:
        raise AssertionError(('system language', code, result.returncode, result.stdout, result.stderr))
result = subprocess.run([sys.argv[1], 'language'], capture_output=True, timeout=15,
                        env=dict(os.environ, PS3_LANG='unsupported'))
if result.returncode != 86 or b'[native-blocked]' not in result.stderr:
    raise AssertionError(('unmapped system language', result.returncode, result.stderr))
result = subprocess.run([sys.argv[2], 'unsupported'], capture_output=True, timeout=15)
for button, expected in (('cross', 1), ('circle', 0), ('invalid', None), (None, None)):
    environment = dict(os.environ)
    environment.pop('PS3_ENTER_BUTTON', None)
    if button is not None: environment['PS3_ENTER_BUTTON'] = button
    preference = subprocess.run([sys.argv[1], 'enter_button'], capture_output=True, timeout=15, env=environment)
    if expected is None:
        if preference.returncode != 86 or b'enter button requires' not in preference.stderr:
            raise AssertionError(('missing/invalid enter preference', button, preference.returncode, preference.stderr))
    elif preference.returncode or f'[language-value] {expected}'.encode() not in preference.stdout:
        raise AssertionError(('enter preference', button, preference.returncode, preference.stdout, preference.stderr))
if result.returncode != 86 or b'[native-blocked] unsupported SPU instruction' not in result.stderr:
    raise AssertionError(('SPU unsupported instruction', result.returncode, result.stderr[-2000:]))
print('Verified HLE, direct/indirect PPU, LV2 and SPU interpreter/instruction fail-closed paths')
result = subprocess.run([sys.argv[5], 'device_missing'], capture_output=True, timeout=15)
if result.returncode != 86 or b'audio output cannot consume a complete block' not in result.stderr:
    raise AssertionError(('missing audio device', result.returncode, result.stderr[-2000:]))
for mode, expected in (('variant_mutation', b'SPU AOT variant mismatch'), ('variant_unowned', b'unprepared SPU target='),
                       ('variant_unbound', b'SPU AOT variant owner is not bound')):
    result = subprocess.run([sys.argv[2], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or expected not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
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
result = subprocess.run([sys.argv[3], sys.argv[4], 'not_started'], capture_output=True, timeout=15)
if result.returncode != 86 or b'export called before module start' not in result.stderr:
    raise AssertionError(('PRX module-start order', result.returncode, result.stderr[-2000:]))
for mode, expected in (('changed_start', b'start descriptor changed'), ('bad_start_abi', b'start violated PPU ABI')):
    result = subprocess.run([sys.argv[3], sys.argv[4], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or expected not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
result = subprocess.run([sys.argv[3], sys.argv[4], 'missing_export'], capture_output=True, timeout=15)
if result.returncode != 86 or b'[native-blocked] original cellSpurs owner has no prepared export' not in result.stderr:
    raise AssertionError(('PRX ownership fallback', result.returncode, result.stderr[-2000:]))
for mode in ('gcm_missing_export', 'gcm_missing_private'):
    result = subprocess.run([sys.argv[3], sys.argv[4], mode], capture_output=True, timeout=15)
    if result.returncode != 86 or b'original GCM owner has no prepared export' not in result.stderr:
        raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
if sys.platform == 'win32':
    for mode in ('pressure', 'sensors', 'actuator'):
        result = subprocess.run([str(Path(sys.argv[1]).with_name('ps3-native-pad.exe')), mode], capture_output=True, timeout=15)
        expected = b'actuator request did not reach the host device' if mode == 'actuator' else b'controller data mode is not backed by measured host fields'
        if result.returncode != 86 or expected not in result.stderr:
            raise AssertionError((mode, result.returncode, result.stderr[-2000:]))
