#!/usr/bin/env python3
"""Batch benchmark: bench.py BOT_A BOT_B [--maps a,b] [--jobs N]

Plays BOT_A vs BOT_B on every map from both sides (matches are deterministic).
Reports wins for BOT_A, gem margin and finishing round per map.
BOT names are directories under bots/ (or paths).
"""
import argparse, json, os, subprocess, tempfile, shutil, glob, statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def bot_path(name):
    return name if os.path.isdir(name) else os.path.join(ROOT, "bots", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--maps", default="")
    ap.add_argument("--jobs", type=int, default=10)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    maps = [os.path.basename(p)[:-len(".map26a")] for p in sorted(glob.glob(os.path.join(ROOT, "maps", "*.map26a")))]
    if args.maps:
        maps = args.maps.split(",")
    A, B = bot_path(args.a), bot_path(args.b)
    tmp = tempfile.mkdtemp(prefix="bench-")
    rows, jobs = [], []
    for m in maps:
        for side in (0, 1):
            tag = f"{m}-{side}"
            x, y = (A, B) if side == 0 else (B, A)
            jobs.append((m, side, f"{tmp}/{tag}.json"))
            rows.append(f"{x} {y} {ROOT}/maps/{m}.map26a {tmp}/{tag}.repl26a --statistics {tmp}/{tag}.json --json")
    man = f"{tmp}/manifest.txt"
    open(man, "w").write("\n".join(rows) + "\n")
    subprocess.run([os.path.join(ROOT, "cambc"), "run-batch", man, "--jobs", str(args.jobs), "--release",
                    "--timeout", str(args.timeout), "--log-limit", "0"],
                   capture_output=True, text=True)
    res = {}
    for m, side, js in jobs:
        try:
            d = json.load(open(js))
        except Exception:
            res[(m, side)] = None
            continue
        g = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
        ga, gb = (g["teamA"], g["teamB"]) if side == 0 else (g["teamB"], g["teamA"])
        rnd = d["match"]["roundsStarted"]
        a_wins = (d["match"]["outcome"] == "teamA") == (side == 0)
        res[(m, side)] = (a_wins, ga - gb, rnd, ga, gb)
    shutil.rmtree(tmp, ignore_errors=True)

    tw = tn = 0
    margins = []
    rounds = []
    print(f"{args.a} vs {args.b}")
    for m in maps:
        cells, w = [], 0
        for side in (0, 1):
            r = res.get((m, side))
            if r is None:
                cells.append("ERR")
                continue
            w += r[0]
            tn += 1
            tw += r[0]
            margins.append(r[1])
            rounds.append(r[2])
            cells.append(f"{'W' if r[0] else 'L'} {r[3]:4d}-{r[4]:<4d} r{r[2]}")
        if not args.quiet:
            print(f"{m:12s} {w}/2  " + " | ".join(cells))
    print(f"TOTAL {tw}/{tn} wins, mean gem margin {statistics.mean(margins) if margins else 0:+.0f}, mean round {statistics.mean(rounds) if rounds else 0:.0f}")


if __name__ == "__main__":
    main()
