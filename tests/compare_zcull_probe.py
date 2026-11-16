"""Compare measured probe records; absence of hardware evidence is not a pass."""
import argparse
import json
from pathlib import Path

CASES = {'reset': 0, 'visible': 4096, 'hidden': 0, 'reset_after_geometry': 0,
         'stats_disabled': 4096, 'enabled_then_disabled': 4096, 'surface_swap': 4096,
         'surface_restore': 0, **{f'region_{i}': 4096 for i in range(8)}, 'final_reset': 0}


def records(path, backend):
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('Probe log exceeds 1 MiB')
    result, header, complete = {}, False, False
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if not line.startswith('{'):
            continue
        row = json.loads(line)
        if row.get('probe') == 'ps3-native-zcull-v1' and row.get('backend') == backend:
            header = True
        if row.get('probe_complete') is True:
            complete = True
        if 'case' not in row:
            continue
        key = row['case'], row['type']
        if row['backend'] != backend or key in result or key[0] not in CASES or key[1] not in range(1, 6):
            raise ValueError('Unexpected/duplicate probe record')
        if type(row['available']) is not bool:
            raise ValueError('Invalid availability')
        value = int(row['value'], 16) if row['available'] else None
        if value is not None and not 0 <= value <= 0xFFFFFFFF:
            raise ValueError('Counter payload exceeds 32 bits')
        if key[1] == 1 and value != CASES[key[0]]:
            raise ValueError('Measured ZPASS disagrees with the synthetic scene')
        if row['available']:
            if not 0 < int(row['timestamp'], 16) <= 0xFFFFFFFFFFFFFFFF or int(row.get('padding', '0'), 16):
                raise ValueError('Invalid report timestamp/padding')
        elif row['value'] is not None or row['timestamp'] is not None:
            raise ValueError('Unavailable result carries invented data')
        result[key] = value
    if not header or not complete or set(result) != {(name, t) for name in CASES for t in range(1, 6)}:
        raise ValueError('Incomplete probe; truncated evidence is not a pass')
    return result


def compare(native, hardware):
    host = records(native, 'd3d12')
    if hardware is None:
        return {'hardware_compared': False, 'status': 'hardware_missing', 'unverified_types': [2, 3, 4, 5]}
    ps3 = records(hardware, 'ps3')
    missing = [f'{name}:{t}' for (name, t), value in host.items() if value is None]
    mismatches = [f'{name}:{t}' for (name, t), value in host.items() if value is not None and value != ps3[name, t]]
    return {'hardware_compared': True, 'status': 'gaps' if missing else 'mismatch' if mismatches else 'case_values_match',
            'unimplemented': missing, 'mismatches': mismatches, 'full_fidelity_verified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('native', type=Path)
    parser.add_argument('hardware', type=Path, nargs='?')
    args = parser.parse_args()
    report = compare(args.native, args.hardware)
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['status'] == 'case_values_match' else 2)
