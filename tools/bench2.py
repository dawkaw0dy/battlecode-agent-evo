#!/usr/bin/env python3
"""bench2.py BOT_A BOT_B [--maps a,b] [--jobs N] [--rounds R]
Plays A vs B on each map from both sides; prints wins for A, gem margins, errors."""
import argparse, json, os, subprocess, tempfile, shutil, glob, zipfile, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def bot_path(n): return n if os.path.isdir(n) else os.path.join(ROOT, "bots", n)
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--maps", default=""); ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--rounds", type=int, default=0); ap.add_argument("--keep", default="")
    ap.add_argument("--map-dir", default=os.path.join(ROOT, "maps"))
    args = ap.parse_args()
    args.map_dir = os.path.abspath(args.map_dir)
    maps = [os.path.basename(p)[:-7] for p in sorted(glob.glob(os.path.join(args.map_dir, "*.map26a")))]
    if args.maps: maps = args.maps.split(",")
    A, B = bot_path(args.a), bot_path(args.b)
    tmp = args.keep or tempfile.mkdtemp(prefix="bench-", dir=os.environ.get("BENCH_TMP", None))
    os.makedirs(tmp, exist_ok=True)
    rows, jobs = [], []
    for m in maps:
        for side in (0, 1):
            tag = f"{m}-{side}"; x, y = (A, B) if side == 0 else (B, A)
            jobs.append((m, side, f"{tmp}/{tag}.json", f"{tmp}/{tag}.repl26a"))
            rows.append(f"{x} {y} {args.map_dir}/{m}.map26a {tmp}/{tag}.repl26a --statistics {tmp}/{tag}.json --json")
    man = f"{tmp}/manifest.txt"; open(man, "w").write("\n".join(rows) + "\n")
    cmd = ["cambc", "run-batch", man, "--jobs", str(args.jobs), "--release", "--timeout", "900", "--log-limit", "200000", "--color", "never"]
    if args.rounds: cmd += ["--rounds", str(args.rounds)]
    env = dict(os.environ, CAMBC_NO_UPDATE_CHECK="1")
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if p.returncode != 0 and not args.keep: print(p.stdout[-3000:], p.stderr[-3000:])
    wins = 0; n = 0; margins = []; errs = 0; faults = 0; lines = []
    for m, side, js, rp in jobs:
        try: d = json.load(open(js))
        except Exception: lines.append(f"{m:12s} {'AB'[side]}  NO RESULT"); continue
        g = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
        ga, gb = (g["teamA"], g["teamB"]) if side == 0 else (g["teamB"], g["teamA"])
        rnd = d["match"]["roundsStarted"]; out = d["match"]["outcome"]
        aw = (out == "teamA") == (side == 0)
        bf = d["diagnostics"]["botFaults"]; myf = bf["teamA" if side == 0 else "teamB"]
        faults += myf; n += 1; wins += aw; margins.append(ga - gb)
        be = 0
        try:
            z = zipfile.ZipFile(rp); so = z.read("team-a.stdout" if side == 0 else "team-b.stdout").decode("utf8", "ignore")
            be = so.count("bot error")
            if "--v" in sys.argv: print(so[:3000])
        except Exception: pass
        errs += be
        lines.append(f"{m:12s} {'AB'[side]}  {'WIN ' if aw else 'loss'}  {ga:5d}-{gb:<5d} r{rnd:4d}  faults={myf} errs={be}")
    print("\n".join(lines))
    print(f"TOTAL {args.a} vs {args.b}: {wins}/{n} wins, mean margin {sum(margins)/max(1,len(margins)):+.0f}, faults={faults}, bot errors={errs}")
    if not args.keep: shutil.rmtree(tmp, ignore_errors=True)
main()
