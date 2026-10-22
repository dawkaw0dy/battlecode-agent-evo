#!/usr/bin/env python3
"""analyze.py stats.json : per-champion summary of a match."""
import json, sys
d = json.load(open(sys.argv[1]))
m = d['match']; print(f"outcome={m['outcome']} rounds={m['roundsStarted']} ")
metrics = {}
for t in d['championTimelines']:
    o = t['owner']; key = (o['team'], o['id'], o['name'])
    metrics.setdefault(key, {})[t['metric']] = t['runs'][-1]['value']
    if t['metric'] == 'submittedGems':
        # time to first 100 gems
        acc = 0; r100 = None
        for run in t['runs']:
            if int(run['value']) >= 100 and r100 is None: r100 = acc
            acc += run['duration']
        metrics[key]['r100'] = r100
hm = {}
for h in d['heatmaps']:
    o = h['owner']; key = (o['team'], o['id'], o['name'])
    n = sum(1 for r in h['runs'] if r['value'] != '0')
    hm.setdefault(key, {})[h['kind']] = n
allm = sorted({k for v in metrics.values() for k in v})
print("team id name            " + " ".join(f"{k[:10]:>10}" for k in allm) + "   deaths kills")
for key in sorted(metrics):
    v = metrics[key]
    print(f"{key[0]:5s} {key[1]:2d} {key[2]:14s} " + " ".join(f"{str(v.get(k,'')):>10}" for k in allm) + f"   {hm.get(key,{}).get('death',0):6d} {hm.get(key,{}).get('kill',0):5d}")
