## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing).
Draft: Vesper Vale (33% gold), Marcella (gold per tributed gem), Willow (ally heals), then Vendetta, Varg, Ryth, ...
Every camp group is scored by (3*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 75% of current HP are
scaled down x0.05. Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
The full group re-score runs every 4 rounds (or when the target is skipped/respawning); otherwise the target is kept.
Combat: lowest-health enemy in range first; after our own basic attack, cast an attack reset and strike again.
Chase visible enemies within 4 tiles below 50% HP while we are above 60% HP (kills pay 100 gold + their gems).
Shop: Treasure emblem, Jungle emblem, then Power shards. Bank gems at >= 20 (or >= 5 near an altar, late game).
Recall < 35% HP (step away from threats first), recall to shop at >= 150 gold when safe; wait on spawn to 90% HP.

## Key parameters
Draft order: vesper_vale, marcella, willow, vendetta, varg, ryth, tavi, ren, cassi_and_issac, stellar, prism, heart_of_ruin.
kChoiceInterval 4 (rounds between full group re-scores). Group static data (spawn list, 3*gems+gold) cached once.
kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP, stuck skip 12 turns -> 50 rounds,
group score weights gems x3, team target x1.5, own target x1.3.
Chase: kChaseRange 4, kChaseOwnHealth 0.6, kChaseEnemyHealth 0.5. Attack reset only right after our own basic attack.

## Changes this generation
Parent e02-01, mutation: parameter tweak, focus opening. All via tools/benchmark.py (both sides, release).
- Baseline (parent copy) vs e01-01 on 01elk-arena 01hooks 00quadrants 01big-world 00packs 00rows: 8/12, +406 mean.
- Draft order, same 6 maps vs e01-01 (every bot in the panel drafts the same list, so we mirrored them):
  - vA, Vendetta 2nd (before Marcella): 11/12 +531. vs parent on 8 maps: 10/15 +235 (one game lost to a parent error).
  - vB, Willow 3rd (before Vendetta): 11/12 +584. vs parent on 8 maps: 12/16 +381. **Kept.**
  - vC, Vendetta 1st, Ryth before Varg: 10/12 +380. Reverted.
- Found while testing: engine error 5 ("Invalid metadata, alignment, overlap or argument") caught on 00lanes (5 picks)
  in our bot and the parents. A fuel probe showed turns routinely ending with 0 fuel left, i.e. suspended mid-action:
  choose_group alone cost 65-85k of the 100k turn fuel every turn. Fixes (kept):
  - tiles_around sets tiles with its own bounds checks instead of draw_chebyshev (TOOLS.md pitfall). Did not
    remove error 5 alone.
  - Cache each group's spawn tile list and value once (choose_group 75k -> 57k fuel).
  - Re-score all groups only every 4 rounds; between re-scores keep the current target unless it is skipped or
    seen respawning. vs vB (same draft, no fix) on 8 maps: 10/15 +58, vB threw error 5 once, we did not.
    vs parent on the same 8 maps: 10/16 +203.
- Final validation (01elk-arena 01hooks 00quadrants 00lanes 00packs 01big-world 01starvation 00corners):
  vs starter 16/16 (+829 mean), vs e00-01 13/15 (+542; one game voided by an e00-01 error), vs e01-01 15/16 (+611).
  0 faults, 0 caught exceptions on our side in all final runs. 236 games in total.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).

- e01-01: Tribute before fighting when next to an unlocked altar with >= 5 gems: +761 vs +1331 (lost 00lanes B). Reverted.
- e01-01: Recall at 45% HP: 7/12 but -347 total margin (less farming time). Reverted.
- e01-01: Recall at 28% HP: 6/12, +961, below the 35% baseline. Reverted.
- e02-01: Target enemies by health - 10 * carried gems: better vs e00-01 but worse vs the parent (5/12 -237 vs 6/12 +132). Reverted.
- e03-03: Draft Vendetta 2nd (before Marcella): 11/12 vs e01-01 but only 10/15 +235 vs parent, below Willow 3rd. Reverted.
- e03-03: Draft Vendetta 1st, Ryth before Varg: 10/12 +380 vs e01-01 (lost 01elk-arena B -600). Reverted.
- e03-03: Fuel: choose_group every turn used 65-85k of 100k fuel; turns ran out and were suspended mid-action, which
  coincided with engine error 5 on 00lanes. Measure with fuel_remaining() before adding per-turn work.

## Known weaknesses
- 01starvation and 01starfish still lose on some sides vs the parent; 00quadrants A loses vs e00-01.
- Fuel is still tight: move_towards_closest ~12k, a full re-score ~57k every 4th round; the pathing field is recomputed
  every step. Draft memory (1024 bytes) is still unused; the opening route is the plain group score.
- Chasing uses plain walking; no dash or stun use for chases. No wards, no altar locks, no dash travel.
