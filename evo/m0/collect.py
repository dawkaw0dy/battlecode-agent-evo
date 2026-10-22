"""M0 probe: summarise one games job into games-<RUN>.json (run inside Actions)."""
import glob
import hashlib
import json
import os
import subprocess


def rows():
    out = []
    for f in sorted(glob.glob('out/*-[AB].json')):
        d = json.load(open(f))
        m = d['match']
        out.append({'tag': os.path.basename(f)[:-5], 'outcome': m['outcome'], 'rounds': m['roundsStarted'],
                    'seed': m.get('seed'), 'gems': {t['team']: int(t['submittedGems']) for t in d['teams']},
                    'faults': d['diagnostics']['botFaults'],
                    'replay_sha256': hashlib.sha256(open(f[:-5] + '.repl26a', 'rb').read()).hexdigest()})
    return out


def read(path, parse=None):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return parse(f) if parse else f.read()


def main():
    run = os.environ['RUN']
    cambc = subprocess.run(['.venv/bin/cambc', '--version'], capture_output=True, text=True).stdout.strip()
    json.dump({
        'run': int(run), 'runner_name': os.environ.get('RUNNER_NAME'), 'nproc': os.cpu_count(), 'cambc': cambc,
        'times': {k: int(os.environ[k]) for k in ('t_start', 't_installed', 't_built', 't_played') if k in os.environ},
        'single_pair_seconds': os.environ.get('t_single'),
        'single_pair_summary': read('single/summary.json', json.load),
        'summary': read('out/summary.json', json.load),
        'rows': rows(),
        'tools': read('tools.tsv', lambda f: [line.rstrip('\n').split('\t') for line in f]),
        'assets': read('assets.txt'),
    }, open(f'games-{run}.json', 'w'), indent=1)


if __name__ == '__main__':
    main()
