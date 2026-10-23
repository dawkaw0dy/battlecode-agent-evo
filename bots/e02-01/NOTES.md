## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing).
Draft: Vesper Vale (33% gold), Marcella (gold per tributed gem), then Vendetta, Varg, Ryth, Tavi, Willow, Ren, ...
Every camp group is scored by (3*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 75% of current HP are
scaled down x0.05. Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
Combat: lowest-health enemy in range first; after our own basic attack, cast an attack reset and strike again.
Chase visible enemies within 4 tiles below 50% HP while we are above 60% HP (kills pay 100 gold + their gems).
Shop: Treasure emblem, Jungle emblem, then Power shards. Bank gems at >= 20 (or >= 5 near an altar, late game).
Recall < 35% HP (step away from threats first), recall to shop at >= 150 gold when safe; wait on spawn to 90% HP.

## Key parameters
kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP, stuck skip 12 turns -> 50 rounds,
group score weights gems x3, team target x1.5, own target x1.3.
Chase: kChaseRange 4, kChaseOwnHealth 0.6, kChaseEnemyHealth 0.5. Attack reset only right after our own basic attack.

## Changes this generation
Parent e01-01, mutation: parameter tweak, focus combat. All via tools/benchmark.py (both sides, release).
- Baseline (parent copy) vs e00-01 on 01hooks 00gem-fields 00gemfight: 2/6, -1770 total margin.
- Kept: attack reset right after our own successful basic attack, then a second basic attack (parent had resets
  disabled). Same 3 maps: 4/6, +2591. 10 other maps: vs e00-01 12/20 +4436 (parent there: 10/19 +196),
  vs parent 12/20 +1484. No caught exceptions.
- Reverted: enemy target score = health - 10 * carried gems. vs e00-01 (00gemfight 00packs 00split 01starfish)
  4/8 +655 (before 3/8 -13), but vs parent (00ambush 00rows 01elk-arena 00packs 01big-world 01starfish)
  5/12 -237 (before 6/12 +132).
- Kept: chase hurt enemies (rule above, enemy threshold 35%): e00-01 set 5/8 +2447, parent set 8/12 +4430.
- Kept: chase threshold 35% -> 50%: e00-01 set 7/8 +3177, parent set 8/12 +4121.
- Final validation on 00contest 01the-farm 00corners 01islands 00stampede 01hooks: vs starter 12/12 (+552 mean),
  vs e00-01 11/12 (+292 mean), vs e01-01 10/12 (+276 mean). 0 faults, 0 caught exceptions. 168 games in total.

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

## Known weaknesses
- 01big-world still loses on both sides vs the parent (-48, -203); 00packs B and 00rows B lose.
- Chasing uses plain walking: ranged champions give up the attack that turn when they step; no dash or stun use for chases.
- No wards, no altar locks, no dash travel; retreat logic is unchanged from the parent (35% HP recall).
