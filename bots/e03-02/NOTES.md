## Strategy (≤10 lines)
Submitted fallback: the parent's (e02-01) scaling economy unchanged, except that a sleeping golem is only woken
when at least 2 allies are within 6 tiles (parent: own Strength >= 90). The radical "squad" strategy below was
built and tested but lost badly to the parent, so it was not submitted.
Parent economy: draft Vesper Vale, Marcella, Vendetta, Varg, Ryth...; each champion scores camp groups by
(3*gems + gold) / (path distance + clear turns + 8), x0.05 if estimated camp damage > 75% of current HP; Treasure ->
Jungle -> Power shop; bank at >= 20 gems; recall < 35% HP; chase enemies < 50% HP within 4 tiles.

## Key parameters
Parent's: kBankGems 20, kShopGold 150, recall 35% HP, spawn heal 90%, danger 0.75*HP, chase 4 tiles / 60% own / 50% enemy.
New: golem wake needs >= 2 allies within 6 tiles.

## Changes this generation
Mutation: radical new strategy, focus camps. 60 games, all vs e02-01 on 01elk-arena 01hooks 00quadrants 00packs
01big-world 00lanes (both sides).
- Squad v1 (shared persistent team target for every group, gather 7 tiles out until 1-2 allies near before waking
  elk/golem groups, squad-sized clear estimate, solo detours to cluster-only groups within 4): 6/11, -94 mean,
  but on 01elk-arena our monster damage was 22k vs 107k and we died 29 times vs 11. Too passive and too clumped.
- Squad v2 (individual targets, squad only for golem+ groups, detour 5, danger on max HP): 2/12, -524.
- v3 (danger back on current HP, hold heavy target only above 70% HP): 1/11, -401. v4 (no detour): 3/9, -191.
- Fallback (submitted, parent + golem rule): 5/10 vs parent, mirror-like (+-same margin per side); 00lanes has caught
  exceptions on team B in both games, i.e. in the parent's code as well (see weaknesses).

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
- e03-02: Whole-team shared target + gathering outside camp range before waking elk/golem groups: far less farming,
  more deaths (vs parent 6/11 -94 at best, 1/11 -401 at worst). Squads lose to parallel farming here.
- e03-02: Solo detours to cluster-only groups within 4-5 tiles: no gain (with or without it the squad versions lost).
- e03-02: Danger check on max HP instead of current HP sends hurt champions into elk camps: many deaths.

## Known weaknesses
- 00lanes: team B raises "engine error 5: Invalid metadata, alignment, overlap or argument" (parent code, both
  sides of the mirror); not located. Replacing draw_chebyshev in tiles_around with per-tile sets did not remove it.
- Everything else as the parent: 01elk-arena / 00packs / 00quadrants side-dependent blowouts vs e02-05.
