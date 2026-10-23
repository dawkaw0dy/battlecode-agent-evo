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
JOB_ID_CHARS = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-')


def runner_version(root=ROOT):
    return tomllib.loads((root / 'evo/config.toml').read_text(encoding='utf-8'))['eval']['runner_version']


HELDOUT_PREFIX = 'h-'          # held-out maps (§9.4) are named h-<id>
HELDOUT_DIR = 'heldout/maps'   # a checkout of the private held-out repository; never committed here


def is_heldout(name):
    return name.startswith(HELDOUT_PREFIX)


def map_dir(root, name):
    return Path(root) / (HELDOUT_DIR if is_heldout(name) else 'maps')


def map_sha256(root, name):
    return hashlib.sha256((map_dir(root, name) / f'{name}.map26a').read_bytes()).hexdigest()


def build(job_id, pairs, maps, *, generation=None, stage='M1', shard_games=200, root=ROOT):
    """A job dict; each pair is split into shards of at most shard_games games (2 per map)."""
    return build_units(job_id, [(a, b, maps) for a, b in pairs], generation=generation, stage=stage,
                       shard_games=shard_games, root=root)


def build_units(job_id, units, *, generation=None, stage='M1', shard_games=200, root=ROOT, heldout_repo=None,
                runs_on=None):
    """A job dict from (a, b, maps) units; each unit is split into shards of at most shard_games games.

    Held-out maps go into shards of their own (`heldout: true`), so only those shards need the
    private repository, and they never publish replays (a replay contains the map).
    """
    if not job_id or set(job_id) - JOB_ID_CHARS:
        raise ValueError(f'bad job id {job_id!r}')
    root = Path(root)
    bots = sorted({b for a, b_, _ in units for b in (a, b_)})
    maps = sorted({m for _, _, ms in units for m in ms})
    per_shard = max(1, shard_games // 2)
    shards = []
    for a, b, unit_maps in units:
        for heldout in (False, True):
            part = [m for m in unit_maps if is_heldout(m) == heldout]
            for i in range(0, len(part), per_shard):
                shard = {'a': a, 'b': b, 'maps': part[i:i + per_shard]}
                if heldout:
                    shard['heldout'] = True
                shards.append(shard)
    if any(s.get('heldout') for s in shards) and not heldout_repo:
        raise ValueError('held-out maps requested but no [eval].heldout_pool repository is configured')
    manifest = (root / 'evo/env/manifest.json').read_bytes()
    job = {
        'job_id': job_id, 'generation': generation, 'stage': stage,
        'runner_version': runner_version(root),
        'env_manifest_sha256': hashlib.sha256(manifest).hexdigest(),
        'src_sha256': {b: digest(root / 'bots' / b) for b in bots},
        'map_sha256': {m: map_sha256(root, m) for m in maps},
        'shards': shards,
    }
    if heldout_repo:
        job['heldout_repo'] = heldout_repo
    if runs_on:  # spot-check jobs replay on a different runner image (D35)
        job['runs_on'] = runs_on
    return job


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
            if is_heldout(m) != bool(shard.get('heldout')):
                raise ValueError(f'shard {i}: map {m!r} does not match the shard\'s heldout flag')
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
    heldout_repo = None
    if any(is_heldout(m) for m in args.maps):
        heldout_repo = tomllib.loads((ROOT / 'evo/config.toml').read_text(encoding='utf-8'))['eval'].get('heldout_pool')
    job = build_units(args.job_id, [(a, b, args.maps) for a, b in pairs], stage=args.stage, heldout_repo=heldout_repo)
    target = ROOT / 'evo/jobs' / f'{args.job_id}.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(dumps(job))
    print(target.relative_to(ROOT).as_posix())
    return 0


if __name__ == '__main__':
    sys.exit(main())
