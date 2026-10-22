#!/usr/bin/env python3
"""Turn one shard's benchmark output into §4 result rows (PLAN §9.2 step 6).

  extract.py <out_dir> --job evo/jobs/<id>.json --shard <i> [--build-failed BOT ...] > rows.json

Reads <out_dir>/<map>-<side>.json and .repl26a directly. A game without a stats file gets no
row (verification re-queues it, §10.2). With --build-failed, no games were played and every
requested game becomes a `build_failed` row (D22).
"""
import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from evo.eval import jobfile  # noqa: E402


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def game_row(out, a, b, m, side, job):
    tag = f'{m}-{side}'
    stats_path, replay_path = out / f'{tag}.json', out / f'{tag}.repl26a'
    if not stats_path.is_file():
        return None
    data = json.loads(stats_path.read_text(encoding='utf-8'))
    outcome = data['match']['outcome']
    bot_errors = {'teamA': 0, 'teamB': 0}
    replay_sha = None
    if replay_path.is_file():
        replay_sha = sha256(replay_path)
        with zipfile.ZipFile(replay_path) as replay:
            names = set(replay.namelist())
            for team, s in (('teamA', 'a'), ('teamB', 'b')):
                if f'team-{s}.stdout' in names:
                    bot_errors[team] = replay.read(f'team-{s}.stdout').count(b'bot error:')
    faults = data['diagnostics']['botFaults']
    return {'a': a, 'b': b, 'map': m, 'side': side,
            'outcome': outcome if outcome in ('teamA', 'teamB') else 'unfinished',
            'gems': {t['team']: int(t['submittedGems']) for t in data['teams']},
            'rounds': data['match']['roundsStarted'],
            'faults': {'teamA': int(faults.get('teamA', 0)), 'teamB': int(faults.get('teamB', 0))},
            'bot_errors': bot_errors,
            'src_sha256': {a: job['src_sha256'][a], b: job['src_sha256'][b]},
            'map_sha256': job['map_sha256'][m],
            'stats_sha256': sha256(stats_path), 'replay_sha256': replay_sha}


def rows(out, job, index, build_failed=()):
    shard = job['shards'][index]
    a, b = shard['a'], shard['b']
    result = []
    for m in shard['maps']:
        for side in ('A', 'B'):
            if build_failed:
                result.append({'a': a, 'b': b, 'map': m, 'side': side, 'outcome': 'build_failed',
                               'build_failed': sorted(set(build_failed)),
                               'src_sha256': {a: job['src_sha256'][a], b: job['src_sha256'][b]},
                               'map_sha256': job['map_sha256'][m]})
                continue
            row = game_row(Path(out), a, b, m, side, job)
            if row is not None:
                result.append(row)
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('out')
    ap.add_argument('--job', required=True)
    ap.add_argument('--shard', type=int, required=True)
    ap.add_argument('--build-failed', nargs='*', default=[])
    args = ap.parse_args(argv)
    job = jobfile.load(args.job)
    json.dump({'job_id': job['job_id'], 'shard': args.shard,
               'rows': rows(args.out, job, args.shard, args.build_failed)}, sys.stdout, indent=1)
    print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
