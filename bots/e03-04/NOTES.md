## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing). Parent: e02-01.
Draft: Vesper Vale (33% gold), Marcella (gold per tributed gem), Vendetta, Willow (heals, Strength 55), then Varg, Ryth,
Tavi, Ren, ... (3 picks: Vesper/Marcella/Vendetta; 4 picks add Willow; 5 picks add Varg instead of Ryth).
Every camp group is scored by (3*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 75% of current HP are
scaled down x0.05. Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
Combat: lowest-health enemy in range first; after our own basic attack, cast an attack reset and strike again.
Chase visible enemies within 4 tiles below 50% HP while we are above 60% HP (kills pay 100 gold + their gems).
Shop: Treasure emblem, Jungle emblem, then Power shards. Bank gems at >= 20 (or >= 5 near an altar, late game).
Recall < 35% HP (step away from threats first), recall to shop at >= 150 gold when safe; wait on spawn to 90% HP.

## Key parameters
Draft preference: vesper_vale, marcella, vendetta, willow, varg, ryth, tavi, ren, cassi_and_issac, stellar, prism,
heart_of_ruin. Draft memory unused.
kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP, stuck skip 12 turns -> 50 rounds,
group score weights gems x3, team target x1.5, own target x1.3.
Chase: kChaseRange 4, kChaseOwnHealth 0.6, kChaseEnemyHealth 0.5. Attack reset only right after our own basic attack.

## Changes this generation
Parent e02-01, mutation: parameter tweak, focus opening (draft). All via tools/benchmark.py (both sides, release).
Baseline (parent copy) vs e01-01 on 01elk-arena 01hooks 00quadrants 01big-world 00packs 00rows 00lanes 01starvation:
12/16, +418 mean; losses 01big-world A/B, 00packs B, 00rows B. Mirror vs parent: 6/12, +0 (each map won by one side).
- Kept: Willow replaces Ryth as 5th pick (5-pick maps). On 01big-world 00packs 00rows 00collision 01hooks 01elk-arena:
  vs parent 7/12 +116 (mirror 0); vs e01-01 01big-world -251 -> -13 total, 01elk-arena +1624 -> +1800.
- Kept: Willow ahead of Varg (4th pick, changes 4-pick maps). 00gemfight 00interlock 00swirl 01shortcut 00rushbait-v2
  00reverse-spiral vs e01-01: 8/12 +217 -> 11/12 +555; with 01hooks 00rows: vs e01-01 14/16 +523, vs parent 12/16 +234.
- Reverted: Willow ahead of Vendetta (3rd pick, changes 3-pick maps). On 00fragment 01broken-curve 01gold-rush 01the-farm
  00collision 00packs: vs parent 9/12 +266, but vs e01-01 10/12 with lower margin (01broken-curve -50/-1003, was
  +944/+233) and vs e00-01 9/12 +253 against 11/12 +394 with Vendetta 3rd.
- Validation (Willow 3rd build, same 4- and 5-pick drafts) on 00contest 01the-farm 00corners 01islands 00stampede
  01hooks 00rows 01broken-curve: vs e00-01 14/16 (the two losses were 01broken-curve, fixed by the revert above),
  vs starter 15/16. 0 faults, 0 caught exceptions in every run. 184 games in total.

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
- e03-04: Willow as 3rd pick (ahead of Vendetta): wins more 3-pick games vs the parent, but loses 01broken-curve on both
  sides vs e00-01 and e01-01 (up to -1003); Vendetta 3rd kept.

## Known weaknesses
- Draft is a fixed preference list; the map (camp mix, team size, distances) is not read, and draft memory is unused.
- 01big-world B and 00rows B still lose vs e01-01; 00reverse-spiral is roughly even.
- Chasing uses plain walking; no dash or stun use for chases. No wards, no altar locks, no dash travel.
