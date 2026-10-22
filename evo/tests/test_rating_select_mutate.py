import math
import random

import pytest

from evo.orchestrator import mutate, rating, select


def game(a, b, m, winner=None, *, fa=0, fb=0, ea=0, eb=0, gems=(10, 5), build_failed=None):
    outcome = 'build_failed' if build_failed else ('teamA' if winner == a else 'teamB' if winner == b else 'unfinished')
    return {'A': a, 'B': b, 'map': m, 'outcome': outcome, 'faults': {'teamA': fa, 'teamB': fb},
            'bot_errors': {'teamA': ea, 'teamB': eb}, 'build_failed': build_failed or [],
            'gems': {'teamA': gems[0], 'teamB': gems[1]}, 'rounds': 2000}


def test_from_row_maps_sides_to_teams():
    row = {'a': 'x', 'b': 'y', 'map': 'm', 'side': 'B', 'outcome': 'teamA', 'gems': {'teamA': 3, 'teamB': 1}}
    g = rating.from_row(row)
    assert (g['A'], g['B']) == ('y', 'x') and rating.result(g) == ('y', 'x')


def test_section_11_table():
    assert rating.result(game('x', 'y', 'm', 'x')) == ('x', 'y')
    assert rating.result(game('x', 'y', 'm', 'x', fa=1)) == ('y', 'x')          # only x faulted -> loss for x
    assert rating.result(game('x', 'y', 'm', 'x', eb=2)) == ('x', 'y')          # only y threw
    assert rating.result(game('x', 'y', 'm', 'x', fa=1, eb=1)) is None          # both -> discarded
    assert rating.result(game('x', 'y', 'm', None)) is None                     # unfinished, no faults
    assert rating.result(game('x', 'y', 'm', build_failed=['y'])) == ('x', 'y')  # D22
    assert rating.result(game('x', 'y', 'm', build_failed=['x', 'y'])) is None


def test_bradley_terry_orders_and_stays_finite():
    games = [game('a', 'b', f'm{i}', 'a') for i in range(8)] + [game('b', 'c', f'm{i}', 'b') for i in range(8)]
    s = rating.fit(games, ['a', 'b', 'c'])
    assert s['a'] > s['b'] > s['c'] and all(math.isfinite(v) for v in s.values())
    even = rating.fit([game('a', 'b', 'm1', 'a'), game('a', 'b', 'm2', 'b')], ['a', 'b'])
    assert even['a'] == pytest.approx(even['b'], abs=1e-6)


def test_faulting_bot_is_disqualified_and_ranks_last():
    games = []
    for i in range(10):
        games.append(game('f', 'g', f'm{i}', 'f', fa=5))      # f faults every game
        games.append(game('g', 'h', f'm{i}', 'g'))
    rows, order = rating.rate(games, ['f', 'g', 'h'], max_fault_rate=0.10, seed=1, replicates=20)
    assert rows['f']['disqualified'] and rows['f']['strength'] == -math.inf
    assert order[-1] == 'f' and order[0] == 'g'
    lo, hi = rows['g']['interval']
    assert lo <= hi


def test_bootstrap_is_seeded():
    games = [game('a', 'b', f'm{i}', 'a' if i % 3 else 'b') for i in range(9)]
    one = rating.bootstrap(games, ['a', 'b'], random.Random(5), 50)
    two = rating.bootstrap(games, ['a', 'b'], random.Random(5), 50)
    assert one == two


def test_similar_needs_correlation_and_even_head_to_head():
    games = []
    for i in range(6):
        w = 'x' if i % 2 else 'o'  # x and y both beat o on odd maps only: identical vectors
        games.append(game('x', 'o', f'm{i}', w))
        games.append(game('y', 'o', f'm{i}', 'y' if w == 'x' else 'o'))
    assert not select.similar(games, 'x', 'y')                       # no head-to-head -> not similar
    games += [game('x', 'y', 'h1', 'x'), game('x', 'y', 'h2', 'y')]
    assert select.similar(games, 'x', 'y')                           # 50% head-to-head
    games += [game('x', 'y', 'h3', 'x'), game('x', 'y', 'h4', 'x')]
    assert not select.similar(games, 'x', 'y')                       # 75% -> distinct


def test_similar_undefined_for_constant_vectors():
    games = [game('x', 'o', f'm{i}', 'x') for i in range(6)] + [game('y', 'o', f'm{i}', 'y') for i in range(6)]
    games += [game('x', 'y', 'h1', 'x'), game('x', 'y', 'h2', 'y')]
    assert not select.similar(games, 'x', 'y')


def test_survivors_skip_twins():
    ratings = {b: {'disqualified': b == 'd'} for b in 'abcd'}
    games = []
    for i in range(6):
        w = i % 2
        for bot in ('a', 'b'):
            games.append(game(bot, 'o', f'm{i}', bot if w else 'o'))
    games += [game('a', 'b', 'h1', 'a'), game('a', 'b', 'h2', 'b')]
    chosen, skipped = select.survivors(['d', 'a', 'b', 'c'], ratings, games, 2)
    assert chosen == ['a', 'c'] and skipped == [('b', 'a')]


def test_allocation_matches_plan_examples():
    assert select.survivor_count(8, 0.25) == 2 and select.survivor_count(4, 0.25) == 2
    assert select.allocation(['a', 'b'], 6) == [4, 2]   # §12.2: n=8, s=2
    assert select.allocation(['a', 'b'], 2) == [1, 1]   # n=4
    assert sum(select.allocation(['a', 'b', 'c'], 7)) == 7


def test_slot_rng_is_stable_and_independent_of_attempt():
    a = [mutate.slot_rng(20261022, 3, 5).random() for _ in range(3)]
    b = [mutate.slot_rng(20261022, 3, 5).random() for _ in range(3)]
    c = mutate.slot_rng(20261022, 3, 6).random()
    assert a == b and a[0] != c


def test_operator_focus_and_archetype_choices_are_seeded():
    weights = {'tweak': 0.4, 'subsystem': 0.3, 'crossover': 0.2, 'radical': 0.1}
    picks = [mutate.weighted_choice(mutate.slot_rng(1, 1, s), weights) for s in range(400)]
    assert picks == [mutate.weighted_choice(mutate.slot_rng(1, 1, s), weights) for s in range(400)]
    assert picks.count('tweak') > picks.count('radical')
    with pytest.raises(ValueError):
        mutate.choose_archetype(random.Random(1), ['a', 'b'], recent=['a'], taken=['b'])
    assert mutate.choose_archetype(random.Random(1), ['a', 'b', 'c'], recent=['a', 'b']) == 'c'


def test_focus_weighting_uses_tags_and_halves_repeats():
    focus = ['camps', 'combat']
    tags = {'camps': ['many-camps'], 'combat': ['small']}
    map_tags = {'m1': ['many-camps'], 'm2': ['many-camps'], 'm3': ['many-camps']}
    ev = [{'map': 'm1'}, {'map': 'm2'}, {'map': 'm3'}]
    counts = {'camps': 0, 'combat': 0}
    for s in range(500):
        counts[mutate.choose_focus(mutate.slot_rng(2, 1, s), focus, tags, map_tags, ev)] += 1
    assert counts['camps'] > 3 * counts['combat']          # weights 4 vs 1
    halved = sum(mutate.choose_focus(mutate.slot_rng(2, 1, s), focus, tags, map_tags, ev, 'camps') == 'camps'
                 for s in range(500))
    assert halved < counts['camps']


def test_evidence_and_crossover_partner():
    games = [game('p', 'o', 'm1', 'o', gems=(100, 900)), game('p', 'o', 'm1', 'o', gems=(500, 600)),
             game('o', 'p', 'm2', 'o', gems=(900, 0)), game('p', 'o', 'm3', 'p', gems=(900, 100))]
    ev = mutate.evidence('p', games, 3)
    assert [(e['map'], e['side'], e['gems_parent'], e['gems_opponent']) for e in ev] == [('m2', 'B', 0, 900), ('m1', 'A', 100, 900)]
    games += [game('q', 'o', 'm1', 'q'), game('q', 'o', 'm2', 'q'), game('q', 'o', 'm3', 'o'),
              game('r', 'o', 'm1', 'o'), game('r', 'o', 'm2', 'o'), game('r', 'o', 'm3', 'r')]
    assert mutate.crossover_partner('p', ['p', 'r', 'q'], games) == 'q'
    assert mutate.crossover_partner('p', ['p'], games) is None
