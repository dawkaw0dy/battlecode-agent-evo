#!/usr/bin/env python3
"""pool.py OUT CAND [CAND...] --opps a,b,c [--maps m1,m2] [--jobs 4]
Plays every candidate vs every opponent on every map, both sides, in one run-batch.
Candidates/opponents are directories under bots/. Prints per (cand,opp) totals and per-map tallies."""
import argparse, json, os, subprocess, zipfile, glob, collections, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace(os.sep, '/')
CAMBC = ROOT + '/.venv/Scripts/cambc.exe'
ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('cands', nargs='+')
ap.add_argument('--opps', required=True); ap.add_argument('--maps', default='')
ap.add_argument('--jobs', type=int, default=4); ap.add_argument('--rounds', type=int, default=0)
a = ap.parse_args()
maps = [os.path.basename(p)[:-7] for p in sorted(glob.glob(ROOT + '/maps/*.map26a'))]
if a.maps: maps = a.maps.split(',')
opps = a.opps.split(',')
out = os.path.abspath(a.out); os.makedirs(out, exist_ok=True)
rows = []; tasks = []
for c in a.cands:
    for o in opps:
        for m in maps:
            for side in 'AB':
                x, y = (c, o) if side == 'A' else (o, c)
                tag = f'{c}__{o}__{m}__{side}'
                tasks.append((c, o, m, side, tag))
                rows.append(f"{ROOT}/bots/{x} {ROOT}/bots/{y} {ROOT}/maps/{m}.map26a {out}/{tag}.repl26a --statistics {out}/{tag}.json")
man = out + '/manifest.txt'; open(man, 'w').write('\n'.join(rows) + '\n')
# Build each distinct bot once, sequentially, to avoid concurrent build races.
for b in dict.fromkeys(list(a.cands) + opps):
    r = subprocess.run([CAMBC, 'build', f'{ROOT}/bots/{b}', '--release'], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        print('BUILD FAILED', b, r.stdout[-2000:], r.stderr[-2000:]); sys.exit(1)
t0 = time.time()
cmd = [CAMBC, 'run-batch', man, '--release', '--json', '--jobs', str(a.jobs), '--timeout', '600', '--color', 'never']
if a.rounds: cmd += ['--rounds', str(a.rounds)]
subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
res = collections.defaultdict(lambda: [0, 0, 0.0, 0])  # wins, games, margin sum, errors
permap = collections.defaultdict(lambda: [0, 0])
detail = []
for c, o, m, side, tag in tasks:
    try:
        d = json.load(open(f'{out}/{tag}.json'))
        team = 'teamA' if side == 'A' else 'teamB'
        sc = {t['team']: int(t['submittedGems']) for t in d['teams']}
        with zipfile.ZipFile(f'{out}/{tag}.repl26a') as z:
            errs = z.read('team-a.stdout' if side == 'A' else 'team-b.stdout').count(b'bot error:')
        win = d['match']['outcome'] == team
        mg = sc[team] - sc['teamB' if side == 'A' else 'teamA']
        faults = sum(d['diagnostics']['botFaults'].values()) if isinstance(d['diagnostics']['botFaults'], dict) else 0
        r = res[(c, o)]; r[0] += win; r[1] += 1; r[2] += mg; r[3] += (errs > 0) + (faults > 0)
        pm = permap[(c, m)]; pm[0] += win; pm[1] += 1
        detail.append((c, o, m, side, win, mg, d['match']['roundsStarted'], errs))
    except Exception as e:
        res[(c, o)][3] += 1; detail.append((c, o, m, side, None, 0, 0, str(e)))
json.dump(detail, open(out + '/detail.json', 'w'))
print(f'elapsed {time.time()-t0:.0f}s')
for c in a.cands:
    tw = tg = 0; tm = 0.0; te = 0
    for o in opps:
        w, g, m, e = res[(c, o)]; tw += w; tg += g; tm += m; te += e
        print(f'  {c:14s} vs {o:10s} {w:3d}/{g:<3d} margin {m/max(g,1):+7.1f} err {e}')
    print(f'POOL {c}: {tw}/{tg} ({100*tw/max(tg,1):.1f}%) margin {tm/max(tg,1):+.1f} err {te}')
    print('   maps: ' + ' '.join(f'{m[:6]}:{permap[(c,m)][0]}/{permap[(c,m)][1]}' for m in maps))
