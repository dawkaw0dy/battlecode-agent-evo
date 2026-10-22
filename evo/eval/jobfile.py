"""Evaluation job files (PLAN §4): build, load and validate the request the workflow plays.

usage (hand-written jobs, M1): python -m evo.eval.jobfile <job_id> --pair a:b [--pair c:d] --maps m1 m2 ...
"""
import argparse
import hashlib
import json
import sys
import tomllib
from pathlib import Path

from evo.eval.digest import digest

ROOT = Path(__file__).resolve().parents[2]
JOB_ID_CHARS = set('abcdefghijklmnopqrstuvwxyz0123456789-')


def runner_version(root=ROOT):
    return tomllib.loads((root / 'evo/config.toml').read_text(encoding='utf-8'))['eval']['runner_version']


def map_sha256(root, name):
    return hashlib.sha256((Path(root) / 'maps' / f'{name}.map26a').read_bytes()).hexdigest()


def build(job_id, pairs, maps, *, generation=None, stage='M1', shard_games=200, root=ROOT):
    """A job dict; each pair is split into shards of at most shard_games games (2 per map)."""
    if not job_id or set(job_id) - JOB_ID_CHARS:
        raise ValueError(f'bad job id {job_id!r}')
    root = Path(root)
    bots = sorted({b for pair in pairs for b in pair})
    per_shard = max(1, shard_games // 2)
    shards = []
    for a, b in pairs:
        for i in range(0, len(maps), per_shard):
            shards.append({'a': a, 'b': b, 'maps': list(maps[i:i + per_shard])})
    manifest = (root / 'evo/env/manifest.json').read_bytes()
    return {
        'job_id': job_id, 'generation': generation, 'stage': stage,
        'runner_version': runner_version(root),
        'env_manifest_sha256': hashlib.sha256(manifest).hexdigest(),
        'src_sha256': {b: digest(root / 'bots' / b) for b in bots},
        'map_sha256': {m: map_sha256(root, m) for m in sorted(set(maps))},
        'shards': shards,
    }


def dumps(job):
    return (json.dumps(job, indent=1, sort_keys=True) + '\n').encode('utf-8')


def load(path):
    job = json.loads(Path(path).read_text(encoding='utf-8'))
    for key in ('job_id', 'runner_version', 'env_manifest_sha256', 'src_sha256', 'map_sha256', 'shards'):
        if key not in job:
            raise ValueError(f'job file lacks {key!r}')
    if Path(path).stem != job['job_id']:
        raise ValueError(f'file name {Path(path).name} does not match job_id {job["job_id"]}')
    for i, shard in enumerate(job['shards']):
        for bot in (shard['a'], shard['b']):
            if bot not in job['src_sha256'] or Path(bot).name != bot:
                raise ValueError(f'shard {i}: bad bot {bot!r}')
        for m in shard['maps']:
            if m not in job['map_sha256'] or Path(m).name != m:
                raise ValueError(f'shard {i}: bad map {m!r}')
    return job


def requested_games(job):
    """Every (a, b, map, side) the job asks for, in order."""
    return [(s['a'], s['b'], m, side) for s in job['shards'] for m in s['maps'] for side in ('A', 'B')]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('job_id')
    ap.add_argument('--pair', action='append', required=True, help='a:b')
    ap.add_argument('--maps', nargs='+', required=True)
    ap.add_argument('--stage', default='M1')
    args = ap.parse_args(argv)
    pairs = [tuple(p.split(':', 1)) for p in args.pair]
    job = build(args.job_id, pairs, args.maps, stage=args.stage)
    target = ROOT / 'evo/jobs' / f'{args.job_id}.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(dumps(job))
    print(target.relative_to(ROOT).as_posix())
    return 0


if __name__ == '__main__':
    sys.exit(main())
