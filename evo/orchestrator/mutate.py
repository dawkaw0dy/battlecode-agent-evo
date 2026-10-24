"""Seeded mutation choices (PLAN §13, D7, D26). Every choice for a slot comes from slot_rng."""
import hashlib
import random

from evo.orchestrator import rating


def slot_rng(seed, generation, slot):
    """D7: one RNG per (seed, generation, slot); relaunches reuse the same task and choices."""
    return random.Random(int(hashlib.sha256(f'{seed}:{generation}:{slot}'.encode()).hexdigest(), 16))


def weighted_choice(rng, weights):
    keys = sorted(weights)
    total = sum(weights[k] for k in keys)
    x = rng.random() * total
    for k in keys:
        x -= weights[k]
        if x < 0:
            return k
    return keys[-1]


def per_map_win_rates(games, bot):
    wins, total = {}, {}
    for g in games:
        if bot not in (g['A'], g['B']):
            continue
        r = rating.result(g)
        if r is None:
            continue
        total[g['map']] = total.get(g['map'], 0) + 1
        wins[g['map']] = wins.get(g['map'], 0) + (r[0] == bot)
    return {m: wins[m] / total[m] for m in total}


def crossover_partner(parent, survivors, games):
    """§13.1: the survivor strongest where the parent is weakest; None if there is no other survivor."""
    mine = per_map_win_rates(games, parent)
    best, best_gain = None, None
    for other in survivors:
        if other == parent:
            continue
        theirs = per_map_win_rates(games, other)
        common = set(mine) & set(theirs)
        gain = sum(max(mine[m], theirs[m]) for m in common) - sum(mine[m] for m in common)
        if best_gain is None or gain > best_gain:
            best, best_gain = other, gain
    return best


def evidence(parent, games, k):
    """§13.3: the parent's k worst counted losses by gem margin, at most one per map."""
    losses = []
    for g in games:
        r = rating.result(g)
        if r is None or r[1] != parent:
            continue
        losses.append((rating.margin(g, parent), g['map'], g))
    out, seen = [], set()
    for m, map_name, g in sorted(losses, key=lambda t: (t[0], t[1])):
        if map_name in seen:
            continue
        seen.add(map_name)
        team = rating.team_of(g, parent)
        other = 'teamB' if team == 'teamA' else 'teamA'
        out.append({'opponent': g['B'] if team == 'teamA' else g['A'], 'map': map_name,
                    'side': 'A' if team == 'teamA' else 'B', 'gems_parent': g['gems'].get(team, 0),
                    'gems_opponent': g['gems'].get(other, 0), 'rounds': g.get('rounds')})
        if len(out) == k:
            break
    return out


def choose_focus(rng, focus, focus_tags, map_tags, evidence_rows, previous_focus=None):
    """§13.2 + D26: weight 1 + evidence losses on maps carrying one of the focus's tags; half if repeated."""
    weights = {}
    for f in focus:
        tags = set(focus_tags.get(f, []))
        w = 1 + sum(1 for e in evidence_rows if tags & set(map_tags.get(e['map'], [])))
        weights[f] = w / 2 if f == previous_focus else w
    return weighted_choice(rng, weights)


def choose_archetype(rng, archetypes, recent, taken=(), last_used=None):
    """§13.4: an archetype no immigrant used in the previous two generations (nor this one).

    D41: when none is left (a gen-0 population of immigrants uses most of them), fall back to the
    archetypes whose last use is oldest, still none taken this generation if possible."""
    free = sorted(set(archetypes) - set(recent) - set(taken))
    if not free:
        pool = sorted(set(archetypes) - set(taken)) or sorted(set(archetypes))
        if not pool:
            raise ValueError('no archetypes found')
        last_used = last_used or {}
        oldest = min(last_used.get(a, -1) for a in pool)
        free = [a for a in pool if last_used.get(a, -1) == oldest]
    return free[rng.randrange(len(free))]
