"""Result verification and the local spot-check (PLAN §10)."""
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from evo.eval import jobfile
from evo.orchestrator import gitops
from evo.orchestrator.config import ROOT

WORKFLOW = '.github/workflows/evo-eval.yml'


def check_results(job, pushed_commit, results, root=ROOT):
    """(problems, missing games). Problems make the results unusable; missing games get re-queued (§10.2)."""
    problems = []
    if results.get('job_id') != job['job_id']:
        problems.append(f"job_id {results.get('job_id')!r} != {job['job_id']}")
    if results.get('trigger_sha') != pushed_commit:
        problems.append(f"trigger_sha {results.get('trigger_sha')} != pushed commit {pushed_commit}")
    wf = gitops.show(pushed_commit, WORKFLOW, root)
    if wf is None or hashlib.sha256(wf).hexdigest() != results.get('workflow_sha256'):
        problems.append('workflow_sha256 does not match evo-eval.yml at the pushed commit')
    requested = {}
    for g in jobfile.requested_games(job):
        requested[g] = requested.get(g, 0) + 1
    wanted = {g: 0 for g in requested}
    first = {}
    for row in results.get('rows', []):
        key = (row.get('a'), row.get('b'), row.get('map'), row.get('side'))
        if key not in wanted:
            problems.append(f'unrequested game {key}')
            continue
        wanted[key] += 1
        played = (row.get('outcome'), row.get('gems'), row.get('rounds'), row.get('faults'), row.get('bot_errors'),
                  row.get('replay_sha256'))
        if key in first and first[key] != played:
            problems.append(f'game {key} was played twice with different results')
        first.setdefault(key, played)
        if row.get('src_sha256') != {row['a']: job['src_sha256'][row['a']], row['b']: job['src_sha256'][row['b']]}:
            problems.append(f'{key}: src_sha256 differs from the job')
        if row.get('map_sha256') != job['map_sha256'][row['map']]:
            problems.append(f'{key}: map_sha256 differs from the job')
    # A job may request a game more than once (it should not: jobs.dedupe_pairs); identical repeats are fine.
    problems += [f'duplicate game {k}' for k, n in wanted.items() if n > requested[k]]
    missing = [k for k, n in wanted.items() if n == 0]
    return problems, missing


def spotcheck_sample(rows, cfg, generation, stage):
    """§10.4: max(spotcheck_min, fraction × games) played games, chosen from the generation seed."""
    import random
    played = sorted((r for r in rows if r['outcome'] != 'build_failed'),
                    key=lambda r: (r['a'], r['b'], r['map'], r['side']))
    k = min(len(played), max(cfg['eval']['spotcheck_min'], math.ceil(cfg['eval']['spotcheck_fraction'] * len(rows))))
    rng = random.Random(int(hashlib.sha256(f"{cfg['seed']}:{generation}:spotcheck:{stage}".encode()).hexdigest(), 16))
    return rng.sample(played, k)


def runner_path(root=ROOT):
    return Path(root) / '.venv' / ('Scripts/cambc.exe' if os.name == 'nt' else 'bin/cambc')


def replay_locally(a, b, maps, root=ROOT, jobs=None):
    """Play a vs b on `maps` (all public or all held-out), both sides, in one tools/benchmark.py run.

    Returns {(map, side): (outcome, gems, rounds)}; on total failure {'error': text}. One run builds
    each bot once and plays the games on all cores.
    """
    maps = list(maps)
    jobs = jobs or max(1, os.cpu_count() or 1)
    with tempfile.TemporaryDirectory(prefix='evo-spot-') as tmp:
        out = Path(tmp) / 'out'
        env = dict(os.environ, PYTHONUTF8='1', CAMBC_NO_UPDATE_CHECK='1')
        proc = subprocess.run([sys.executable, str(Path(root) / 'tools/benchmark.py'), a, b, '--maps', *maps,
                               '--map-dir', str(jobfile.map_dir(root, maps[0])),
                               '--jobs', str(min(jobs, 2 * len(maps))), '--timeout', '300', '--output', str(out),
                               '--runner', str(runner_path(root)), '--json', '--no-progress', '--color', 'never'],
                              cwd=root, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
        found = {}
        for m in maps:
            for side in ('A', 'B'):
                stats = out / f'{m}-{side}.json'
                if stats.is_file():
                    d = json.loads(stats.read_text(encoding='utf-8'))
                    outcome = d['match']['outcome']
                    found[(m, side)] = (outcome if outcome in ('teamA', 'teamB') else 'unfinished',
                                        {t['team']: int(t['submittedGems']) for t in d['teams']},
                                        d['match']['roundsStarted'])
        if not found:
            found['error'] = proc.stdout[-500:] + proc.stderr[-500:]
        return found


def spotcheck(sample, root=ROOT, replay=replay_locally, parallel=2):
    """Mismatch descriptions; empty = every sampled game replayed identically.

    Games are grouped per (a, b, public/held-out) so each pair is built once; `parallel` groups run at the
    same time so one group's build overlaps another's games.
    """
    from concurrent.futures import ThreadPoolExecutor
    groups = {}
    for row in sample:
        groups.setdefault((row['a'], row['b'], jobfile.is_heldout(row['map'])), []).append(row)
    keys = sorted(groups)
    cpu = max(1, os.cpu_count() or 1)

    def run(key):
        a, b, _ = key
        maps = sorted({r['map'] for r in groups[key]})
        if replay is replay_locally:
            return replay(a, b, maps, root, jobs=max(1, cpu // max(1, min(parallel, len(keys)))))
        return replay(a, b, maps, root)

    with ThreadPoolExecutor(max_workers=max(1, min(parallel, len(keys)))) as pool:
        results = dict(zip(keys, pool.map(run, keys)))
    mismatches = []
    for key in keys:
        a, b, _ = key
        local = results[key]
        for row in groups[key]:
            want = (row['outcome'], row['gems'], row['rounds'])
            got = local.get((row['map'], row['side']))
            if got is None or tuple(got) != want:
                mismatches.append(f"{a} vs {b} on {row['map']} side {row['side']}: Actions {want}, "
                                  f"local {got or local.get('error')}")
    return mismatches
