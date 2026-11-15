"""Management CLI. Unsupported execution returns 3; malformed inputs return 2."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from ps3native.catalog import digest, inventory
from ps3native.preparation import dependency, prepare

ROOT = Path(__file__).resolve().parents[2]


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.pending-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(prog='ps3-native', description='PS3 AOT content manager; original XMB support is pending.')
    parser.add_argument('--catalog', type=Path, default=ROOT / '.local/catalog')
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('inspect', 'import', 'prepare', 'run'):
        p = sub.add_parser(command)
        p.add_argument('source', nargs='?', type=Path)
        if command in ('prepare', 'run'):
            p.add_argument('--fixture', choices=['ppu', 'spu'])
            p.add_argument('--build-dir', type=Path, default=ROOT / 'build')
        if command == 'prepare':
            p.add_argument('--cache', type=Path, default=ROOT / '.local/prepared')
    sub.add_parser('system')
    args = parser.parse_args(argv)
    try:
        if args.command == 'system':
            print(json.dumps({'runnable': False, 'blocker': 'Original VSH/XMB loading, PRX dependency binding and system/game lifecycle are not implemented.'}))
            return 3
        if getattr(args, 'fixture', None):
            if args.source:
                raise ValueError('Fixture mode does not accept user content')
            build = args.build_dir.resolve()
            if args.command == 'prepare':
                # CMake owns compiler/tool/input dependencies and incremental invalidation.
                return subprocess.run(['cmake', '--build', str(build), '--config', 'Release',
                                       '--target', f'ps3-native-{args.fixture}'], check=False).returncode
            dependency(ROOT)
            receipt = json.loads((build / 'tests/generated/fixture.json').read_text(encoding='utf-8'))
            for name, expected in receipt['artifacts'].items():
                if Path(name).name != name or digest(build / 'tests/generated' / name) != expected:
                    raise ValueError('Fixture artifacts changed; rebuild before running')
            exe = build / 'tests' / f'ps3-native-{args.fixture}'
            if os.name == 'nt':
                exe = exe.with_suffix('.exe')
            if not exe.is_file():
                raise ValueError('Fixture is not built; run prepare --fixture first')
            command = [str(exe)]
            if args.fixture == 'ppu':
                command.append(str(build / 'tests/generated/ppu.elf'))
            return subprocess.run(command, timeout=30, check=False).returncode
        if args.source is None:
            raise ValueError('A content path or explicit --fixture is required')
        # Reject output within originals before writing catalog files.
        source = args.source.resolve(strict=True)
        if args.command == 'import' and (source == args.catalog.resolve() or source in args.catalog.resolve().parents):
            raise ValueError('Catalog must be outside the original content')
        result = inventory(args.source)
        if args.command == 'import':
            atomic_json(args.catalog / (result['id'] + '.json'), result)
        elif args.command == 'prepare':
            result = prepare(ROOT, result, args.cache)
            print(json.dumps(result, ensure_ascii=True, indent=2))
            return 3 if not result['runnable'] else 0
        elif args.command == 'run':
            result['blocker'] = 'Generic module binding and fail-closed AOT completeness gate are pending. Only explicit synthetic fixtures execute in M0.'
            print(json.dumps(result, ensure_ascii=True, indent=2))
            return 3
        print(json.dumps(result, ensure_ascii=True, indent=2))
        return 0
    except (OSError, ValueError, KeyError, UnicodeError, struct.error, subprocess.SubprocessError) as exc:
        print(f'ps3-native: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
