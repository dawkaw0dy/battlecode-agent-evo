## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing).
Draft: Vesper Vale (33% gold), Marcella (gold per tributed gem), then Vendetta, Varg, Ryth, Tavi, Willow, Ren, ...
Every camp group is scored by (8*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 100% of current HP are
scaled down x0.05. Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
Combat: lowest-health enemy in range first; after our own basic attack, cast an attack reset and strike again.
Chase visible enemies within 4 tiles below 50% HP while we are above 60% HP (kills pay 100 gold + their gems).
Shop: Treasure emblem, Jungle emblem, then Power shards. Bank gems at >= 20 (or >= 5 near an altar, late game).
Recall < 35% HP (step away from threats first), recall to shop at >= 150 gold when safe; wait on spawn to 90% HP.

## Key parameters
kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 1.0*HP, stuck skip 12 turns -> 50 rounds,
group score weights gems x8 (was x3), overhead +8 turns, team target x1.5, own target x1.3.
Chase: kChaseRange 4, kChaseOwnHealth 0.6, kChaseEnemyHealth 0.5. Attack reset only right after our own basic attack.

## Changes this generation
Parent e02-01, mutation: parameter tweak, focus camps. All via tools/benchmark.py (both sides, release, 4 jobs).
Focus maps (parent losses): 01elk-arena 01hooks 00quadrants 01big-world 00packs 00rows, vs e01-01 + e00-01 (24 games).
- Baseline (parent copy): 17/24, total margin +9670.
- Gem weight x3 -> x5: 19/24, +10291. Kept as a step.
- Gem weight x5 -> x8: 20/24, +11114. Kept (post-patch-100 camps pay few gems: wolf 4, elk 15, gem cluster 7,
  golem 36, so groups with gem clusters/elks should beat gold-heavy groups).
- Gem weight x15: 17/24, +9883. Reverted to x8.
- Score overhead +8 -> +16: 16/24, +7570. Reverted.
- Score overhead +8 -> +4: 20/24, +10145 (won 01big-world both sides, but lower margin). Reverted.
- Danger threshold 0.75 -> 1.0 of current HP: 20/24, +11231. Kept: on fresh maps (00contest 01the-farm 00corners
  01islands 00stampede 01starfish) vs the parent e02-01, gem x8 + danger 0.75 went 3/12 -2316, gem x8 + danger 1.0
  went 7/12 -7 (gem x3 + danger 1.0: 6/12 +182; gem x5 + danger 1.0: 6/12 -284, all near parity).
- Final (gem x8, danger 1.0) on the focus maps: vs e02-01 10/12 +3782, vs starter 11/12 +6732.
  0 engine faults, 0 caught exceptions. 264 games in total.

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
- e03-01: Group score gem weight x15: 17/24 +9883 vs x8 20/24 +11114 (overvalues far groups). Reverted to x8.
- e03-01: Group score overhead +16 turns: 16/24 +7570; +4 turns: 20/24 +10145; both below +8. Reverted.
- e03-01: Gem weight x8 with danger 0.75 lost to the parent on fresh maps (3/12 -2316); needs danger 1.0.

## Known weaknesses
- On maps outside the focus set the bot is only at parity with the parent (about 6-7/12); the gem-weight gain is
  concentrated on the focus maps and may partly be overfitting.
- 00rows B and 00packs B still lose vs e01-01; 00quadrants lost both sides vs e00-01 with danger 1.0.
- No wards, no altar locks, no dash travel; clear-time model ignores camp range (elk 4, golem 4, monolith 5).
