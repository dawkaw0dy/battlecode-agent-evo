"""Command line: python -m evo <command> (PLAN §2).

M0 implements ``bootstrap`` and ``manifest``; the generation commands arrive in M2.
"""
import argparse
import os
import sys

from evo import bootstrap

LATER = ('init', 'step', 'run', 'status', 'approve', 'verify', 'spotcheck')


def main(argv=None):
    os.environ.setdefault('PYTHONIOENCODING', 'utf-8')
    parser = argparse.ArgumentParser(prog='python -m evo', description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    boot = sub.add_parser('bootstrap', help='create the venv, SDK, maps, starter bot and manifest (§1.1)')
    boot.add_argument('--no-smoke', action='store_true', help='skip tools/tests and the starter game')
    sub.add_parser('manifest', help='rewrite evo/env/manifest.json and print its sha256')
    for name in LATER:
        sub.add_parser(name, help='not implemented yet (M2)')
    args = parser.parse_args(argv)
    if args.command == 'bootstrap':
        return bootstrap.run(smoke_games=not args.no_smoke)
    if args.command == 'manifest':
        print(bootstrap.write_manifest())
        return 0
    print(f'evo {args.command}: not implemented yet (arrives in M2)', file=sys.stderr)
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
