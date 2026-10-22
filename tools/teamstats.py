#!/usr/bin/env python3
"""teamstats.py DIR : per-game team stats (candidate side first) for a bench2 --keep directory."""
import json, sys, os, glob
tot = {}
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.json'))):
    d = json.load(open(f)); side = 'A' if f.endswith('-0.json') else 'B'
    agg = {}
    for t in d['championTimelines']:
        k = t['owner']['team']; agg.setdefault(k, {}); agg[k][t['metric']] = agg[k].get(t['metric'], 0) + float(t['runs'][-1]['value'])
    for h in d['heatmaps']:
        k = h['owner']['team']; agg[k].setdefault(h['kind'], 0); agg[k][h['kind']] += sum(1 for r in h['runs'] if r['value'] != '0')
    me = 'teamA' if side == 'A' else 'teamB'; op = 'teamB' if side == 'A' else 'teamA'
    g = lambda k, m: int(agg[k].get(m, 0))
    row = {}
    for tag, k in (('me', me), ('op', op)):
        row[tag] = dict(col=g(k, 'collectedGems'), sub=g(k, 'submittedGems'), lost=g(k, 'collectedGems') - g(k, 'submittedGems') - g(k, 'gems'), deaths=g(k, 'death'), gold=g(k, 'earnedGold'))
        for kk, v in row[tag].items(): tot.setdefault(tag, {}); tot[tag][kk] = tot[tag].get(kk, 0) + v
    print(f"{os.path.basename(f):18s} r{d['match']['roundsStarted']:4d} me {row['me']} | op {row['op']}")
print("TOTAL me", tot.get('me'), "| op", tot.get('op'))
