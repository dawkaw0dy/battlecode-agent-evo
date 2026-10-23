## Strategy (≤10 lines)
Parent e04-06 (spell-first pack: Vesper Vale, Ryth, Tavi, Marcella, Willow; spell planner; Lanchester engage; altar-lock
aware banking) with the camp subsystem (groups/choose_group/farm_camps) rewritten. Group stats come from the patch-100
tables (health, strength, range, monster count) and the engine's gem/gold/respawn values. A group's score is
(3 x gems + gold) / (path + respawn wait + clear turns + 5 capture turns + 6). Clear turns use the health actually left
when the whole region is visible; a cleared-but-uncaptured group costs 0 clear turns (steal it). Frontline damage =
strength x clear x (0.65 for multi-monster groups) x max(1/n, 0.5), jungle x0.7; over 75% of our HP -> x0.05.
Respawn timers are remembered as "alive at round", so a group that respawns before we arrive is not skipped.
Golem groups are skipped while our strength < 90 (pick_target never wakes them). Each champion writes a claim
(shared memory 8 + 4 x champion type: group+1, round) so the planner knows how many allies are on each group.

## Key parameters
gem weight 3, capture 5 turns + 6 overhead, team-target bonus 2.5, own-target 1.3, danger factor 0.05 at 75% HP,
kCrowded 1.0 (crowding off), kMaxWait 0 (no waiting at respawning groups), kEnemyNearGroup 1.0 (off), claim fresh 3 rounds.
Shared memory: 0..1 team target, 2..5 focus target, 6..7 enemy-lock mask, 8..55 per-champion-type camp claims.

## Changes this generation
Subsystem rewrite (camps): new GroupInfo (range, camps, gems, gold, respawn), choose_group and farm_camps rewritten.
farm_camps steps off a respawning region's spawn tiles (an occupied spawn tile delays the whole group's respawn) and
only writes the team target for live groups.
Results (local, deterministic, final config): vs e04-06 (parent) 00gemfight 1/2 (-35), 01fracture 2/2 (+230),
01broken-curve 2/2 (+540), 00quadrants 2/2 (+476), 00road-of-riches 0/2 (-648), 01the-farm 1/2.
vs e03-07: 4/8 (+118; gemfight 2/2 +602, broken-curve 2/2 +852, fracture 0/2 -291, road-of-riches 0/2 -690).
vs starter 4/4 (+439). 0 faults, 0 caught exceptions. 54 games total.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- (e02-05) Reading champion_at(tile).health() right after our attack killed the unit throws engine error 97 even when
  has_champion(tile) is true: check the handle's is_visible() first and guard each combat block locally.
- (e03-05) Team-target bonus 4.0 (strict deathball): 7/14 vs e02-01 (too little parallel farming); 1.5: 10/16. Keep 2.5.
- (e03-05) Spending spells on camps while enemy champions are within 7: 12/16 (+252) vs 13/16 (+346) when saving them.
- (e03-05) Ryth-first spell draft without Vesper Vale: 11/15 vs e02-01; Vesper first is better (13/16).
- (e03-05) `bitboard::draw_chebyshev` near the map edge raises engine error 5 (00lanes, 30 tall): set tiles manually.
- (e04-06) Skipping enemy-locked altars (parent behaviour): 0 tributed gems vs altar-lock bots (e03-07). Break them.
- (e05-03) Spreading by claims (x0.45 when enough allies already claim a group, team-target bonus only 1.3 for groups one
  champion can clear): 00gemfight vs parent 0/2 (-460); the pack must stay together on small contested maps.
- (e05-03) Waiting next to a respawning group (kMaxWait 12-25) and x0.6 per enemy near a region: no gain on gemfight
  (-514..-624) and noisy elsewhere; both disabled (kept as parameters).
- (e05-03) Middle setting (bonus 2.5, crowding x0.7, wait 12): gemfight 0/2, road-of-riches 0/2 vs parent.

## Known weaknesses
- 00road-of-riches loses to the parent and to e03-07 on both sides (-650..-1000); fracture loses to e03-07.
- Results swing a lot between near-identical settings (chaotic games); the tuning above rests on few games.
- The monolith is never farmed; golems only with strength >= 90.
