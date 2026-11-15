"""Turn unsupported upstream lowerings into explicit execution stops."""
import json
from pathlib import Path
import re


def guard_sources(directory):
    counts = {}
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
            return '{ fprintf(stderr, ' + message + '); fflush(stderr); exit(86); }'
        changed, count = pattern.subn(trap, text)
        # A branch outside known executable code cannot become a successful return.
        changed, branches = re.subn(r'return; /\* (b\w* -> non-code [^*]*)\*/',
                                    lambda m: trap(m), changed)
        if count or branches or legacy_encoding:
            path.write_text('#include <stdlib.h>\n' + changed, encoding='utf-8', newline='\n')
        counts[path.name] = count + branches
    return counts
