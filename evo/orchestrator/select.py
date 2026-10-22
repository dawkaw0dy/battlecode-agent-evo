"""Selection and repopulation (PLAN §12): survivors with the diversity filter (D25), slot allocation."""
import math

from evo.orchestrator import rating


def survivor_count(n, keep_fraction):
    return max(2, math.ceil(n * keep_fraction))


def cell_win_rates(games, bot, exclude):
    """Win rate of `bot` per (opponent, map) cell over counted games, excluding games against `exclude`."""
    wins, total = {}, {}
    for g in games:
        if bot not in (g['A'], g['B']):
            continue
        opp = g['B'] if g['A'] == bot else g['A']
        if opp == exclude:
            continue
        r = rating.result(g)
        if r is None:
            continue
        cell = (opp, g['map'])
        total[cell] = total.get(cell, 0) + 1
        wins[cell] = wins.get(cell, 0) + (r[0] == bot)
    return {c: wins[c] / total[c] for c in total}


def correlation(xs, ys):
    n = len(xs)
    if n < 4:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    vx = sum((x - mx) ** 2 for x in xs)
    vy = sum((y - my) ** 2 for y in ys)
    if vx == 0 or vy == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / math.sqrt(vx * vy)


def head_to_head(games, b, x):
    """Fraction of counted b-vs-x games that b won, or None without any."""
    won = n = 0
    for g in games:
        if {g['A'], g['B']} != {b, x}:
            continue
        r = rating.result(g)
        if r is None:
            continue
        n += 1
        won += r[0] == b
    return won / n if n else None


def similar(games, b, x, *, threshold=0.9, band=(0.45, 0.55)):
    vb, vx = cell_win_rates(games, b, x), cell_win_rates(games, x, b)
    common = sorted(set(vb) & set(vx))
    corr = correlation([vb[c] for c in common], [vx[c] for c in common])
    if corr is None or corr <= threshold:
        return False
    h2h = head_to_head(games, b, x)
    return h2h is not None and band[0] <= h2h <= band[1]


def survivors(order, ratings, games, s):
    """Top s non-disqualified bots in `order`, skipping any similar to one already chosen."""
    chosen, skipped = [], []
    for b in order:
        if ratings[b]['disqualified']:
            continue
        twin = next((x for x in chosen if similar(games, b, x)), None)
        if twin:
            skipped.append((b, twin))
            continue
        chosen.append(b)
        if len(chosen) == s:
            break
    return chosen, skipped


def largest_remainder(total, weights):
    """Split `total` slots in proportion to `weights` (Hamilton method; ties go to the earlier rank)."""
    wsum = sum(weights)
    if total <= 0 or wsum <= 0:
        return [0] * len(weights)
    quotas = [total * w / wsum for w in weights]
    alloc = [math.floor(q) for q in quotas]
    order = sorted(range(len(weights)), key=lambda i: (-(quotas[i] - alloc[i]), i))
    for i in order[:total - sum(alloc)]:
        alloc[i] += 1
    return alloc


def allocation(survivor_ids, children):
    s = len(survivor_ids)
    return largest_remainder(children, [s - r for r in range(s)])
