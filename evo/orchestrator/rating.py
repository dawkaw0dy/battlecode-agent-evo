"""Scoring (PLAN §11): fault attribution, Bradley–Terry by MM, cluster bootstrap over maps.

A *game* here is one played map/side, normalised to team roles:
  {'A': bot on team A, 'B': bot on team B, 'map': name, 'outcome': 'teamA'|'teamB'|'unfinished'|'build_failed',
   'faults': {'teamA': n, 'teamB': n}, 'bot_errors': {...}, 'build_failed': [bots], 'gems': {...}}
"""
import math
import random

PRIOR = '__average__'  # fixed virtual opponent: one win and one loss for every bot


def from_row(row):
    """Normalise a §4 result row (a/b + side) to team roles."""
    a_is_A = row['side'] == 'A'
    return {'A': row['a'] if a_is_A else row['b'], 'B': row['b'] if a_is_A else row['a'],
            'map': row['map'], 'outcome': row['outcome'],
            'faults': row.get('faults', {'teamA': 0, 'teamB': 0}),
            'bot_errors': row.get('bot_errors', {'teamA': 0, 'teamB': 0}),
            'build_failed': row.get('build_failed', []), 'gems': row.get('gems', {'teamA': 0, 'teamB': 0}),
            'rounds': row.get('rounds')}


def team_of(game, bot):
    return 'teamA' if game['A'] == bot else 'teamB'


def is_faulted(game, bot):
    """Bot faulted, threw (caught exceptions) or failed to build in this game."""
    if game['outcome'] == 'build_failed':
        return bot in game['build_failed']
    team = team_of(game, bot)
    return game['faults'].get(team, 0) > 0 or game['bot_errors'].get(team, 0) > 0


def result(game):
    """(winner, loser) for a game that counts, else None (discarded), per the §11 table."""
    a, b = game['A'], game['B']
    fa, fb = is_faulted(game, a), is_faulted(game, b)
    if fa and fb:
        return None
    if fa:
        return (b, a)
    if fb:
        return (a, b)
    if game['outcome'] == 'teamA':
        return (a, b)
    if game['outcome'] == 'teamB':
        return (b, a)
    return None


def margin(game, bot):
    team = team_of(game, bot)
    other = 'teamB' if team == 'teamA' else 'teamA'
    return game['gems'].get(team, 0) - game['gems'].get(other, 0)


def is_engine_fault(game, bot):
    """Engine fault or failed build: what counts towards disqualification (D29). Caught exceptions do not."""
    if game['outcome'] == 'build_failed':
        return bot in game['build_failed']
    return game['faults'].get(team_of(game, bot), 0) > 0


def fault_rates(games, bots, include_exceptions=False):
    played = {b: 0 for b in bots}
    faulted = {b: 0 for b in bots}
    count = is_faulted if include_exceptions else is_engine_fault
    for g in games:
        for bot in (g['A'], g['B']):
            if bot in played:
                played[bot] += 1
                faulted[bot] += count(g, bot)
    return {b: (faulted[b] / played[b] if played[b] else 0.0) for b in bots}


def bradley_terry(pairs, bots, iterations=2000, tol=1e-10):
    """log-strengths from (winner, loser) pairs with a 1-win/1-loss prior against a fixed average player."""
    bots = list(bots)
    wins = {b: 1.0 for b in bots}  # the prior's virtual win
    count = {}  # games between i and j (unordered)

    def add(i, j):
        key = (i, j) if i < j else (j, i)
        count[key] = count.get(key, 0) + 1

    for w, l in pairs:
        if w in wins and l in wins:
            wins[w] += 1
            add(w, l)
    for b in bots:
        count[(b, PRIOR) if b < PRIOR else (PRIOR, b)] = 2  # one virtual win + one virtual loss
    p = {b: 1.0 for b in bots}
    p[PRIOR] = 1.0
    neighbours = {b: [] for b in bots}
    for (i, j), n in count.items():
        if i in neighbours:
            neighbours[i].append((j, n))
        if j in neighbours:
            neighbours[j].append((i, n))
    for _ in range(iterations):
        delta = 0.0
        new = {}
        for b in bots:
            denom = sum(n / (p[b] + p[o]) for o, n in neighbours[b])
            new[b] = wins[b] / denom
        for b in bots:
            delta = max(delta, abs(math.log(new[b]) - math.log(p[b])))
            p[b] = new[b]
        if delta < tol:
            break
    return {b: math.log(p[b]) for b in bots}


def fit(games, bots):
    return bradley_terry([r for r in map(result, games) if r], bots)


def bootstrap(games, bots, rng, replicates=200, level=0.90):
    """Cluster bootstrap over maps (both sides together): (low, high) log-strength interval per bot."""
    by_map = {}
    for g in games:
        by_map.setdefault(g['map'], []).append(g)
    maps = sorted(by_map)
    samples = {b: [] for b in bots}
    for _ in range(replicates):
        chosen = [maps[rng.randrange(len(maps))] for _ in maps] if maps else []
        strength = fit([g for m in chosen for g in by_map[m]], bots)
        for b in bots:
            samples[b].append(strength[b])
    lo_q, hi_q = (1 - level) / 2, 1 - (1 - level) / 2
    out = {}
    for b in bots:
        s = sorted(samples[b])
        if not s:
            out[b] = (0.0, 0.0)
            continue
        out[b] = (s[int(lo_q * (len(s) - 1))], s[int(math.ceil(hi_q * (len(s) - 1)))])
    return out


def mean_margins(games, bots):
    total = {b: 0 for b in bots}
    n = {b: 0 for b in bots}
    for g in games:
        if result(g) is None:
            continue
        for bot in (g['A'], g['B']):
            if bot in total:
                total[bot] += margin(g, bot)
                n[bot] += 1
    return {b: (total[b] / n[b] if n[b] else 0.0) for b in bots}


def rate(games, bots, *, max_fault_rate, seed, replicates=200, exceptions_disqualify=False):
    """Ratings for every bot: strength (-inf if disqualified), interval, fault rate, mean margin, rank order.

    Caught exceptions always make a game a loss (§11); they count towards disqualification only with
    exceptions_disqualify (D29: off from generation 1; generation 0 was rated with it on).
    """
    bots = sorted(bots)
    faults = fault_rates(games, bots, include_exceptions=exceptions_disqualify)
    strength = fit(games, bots)
    interval = bootstrap(games, bots, random.Random(seed), replicates)
    margins = mean_margins(games, bots)
    rows = {}
    for b in bots:
        dq = faults[b] > max_fault_rate
        rows[b] = {'strength': -math.inf if dq else strength[b], 'interval': interval[b],
                   'fault_rate': faults[b], 'mean_margin': margins[b], 'disqualified': dq}
    order = sorted(bots, key=lambda b: (rows[b]['strength'], rows[b]['mean_margin'], b), reverse=True)
    return rows, order
