import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('probe_compare', Path(__file__).with_name('compare_zcull_probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ZcullProbeTests(unittest.TestCase):
    def test_missing_hardware_and_unknown_counters_are_not_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'synthetic-protocol.jsonl'
            rows = [{'probe': 'ps3-native-zcull-v1', 'backend': 'd3d12'}]
            for case, expected in probe.CASES.items():
                for kind in range(1, 6):
                    rows.append({'backend': 'd3d12', 'case': case, 'type': kind, 'available': kind == 1,
                                 'timestamp': '0000000000000001' if kind == 1 else None,
                                 'value': f'{expected:08x}' if kind == 1 else None})
            rows.append({'probe_complete': True})
            def write():
                path.write_text('\n'.join(json.dumps(r) for r in rows), encoding='utf-8')
            write()
            self.assertEqual(probe.compare(path, None)['status'], 'hardware_missing')
            rows[-1]['probe_complete'] = False
            write()
            with self.assertRaisesRegex(ValueError, 'Incomplete'):
                probe.records(path, 'd3d12')
            rows[-1]['probe_complete'] = True
            rows[2]['value'] = '00000000'
            write()
            with self.assertRaisesRegex(ValueError, 'invented'):
                probe.records(path, 'd3d12')
