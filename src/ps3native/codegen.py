"""Turn unsupported upstream lowerings into explicit execution stops."""
import json
from pathlib import Path
import re


def guard_sources(directory):
    counts = {}
    declaration = '#ifdef __cplusplus\nextern "C" [[noreturn]] void ps3_native_stop(void);\n#else\n_Noreturn void ps3_native_stop(void);\n#endif\n'
    pattern = re.compile(r'/\* (TODO: [^*]*|[^*]*unsupported SPR[^*]*|bl -> non-code [^*]*|trap: [^*]*)\*/;')
    for path in sorted(Path(directory).glob('ppu_recomp*.*')):
        if path.suffix not in ('.c', '.cpp', '.h'):
            continue
        try:
            text = path.read_text(encoding='utf-8')
            legacy_encoding = False
        except UnicodeDecodeError:
            # Upstream uses the Windows locale encoding for generated comments.
            text = path.read_text(encoding='cp1252')
            legacy_encoding = True
        def trap(match):
            message = json.dumps('[native-blocked] unsupported PPU lowering: ' + match[1].strip() + '\n')
            return '{ fprintf(stderr, ' + message + '); fflush(stderr); ps3_native_stop(); }'
        changed, count = pattern.subn(trap, text)
        # A branch outside known executable code cannot become a successful return.
        changed, branches = re.subn(r'return; /\* (b\w* -> non-code [^*]*)\*/',
                                    lambda m: trap(m), changed)
        old_stops = changed.count('fflush(stderr); exit(86);')
        changed = changed.replace('fflush(stderr); exit(86);', 'fflush(stderr); ps3_native_stop();')
        if path.suffix != '.h' and declaration not in changed:
            changed = declaration + changed
        if changed != text or legacy_encoding:
            path.write_text(changed, encoding='utf-8', newline='\n')
        counts[path.name] = count + branches + old_stops
    return counts
