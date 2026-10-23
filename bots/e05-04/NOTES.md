## Strategy (≤10 lines)
Parent e04-06 (spell-first pack: Vesper Vale, Ryth, Tavi, Marcella, Willow; spell planner; Lanchester engage;
defense subsystem with lock breaking/placing and safe tributes) with the pathing subsystem rewritten behind the same
interface (move_towards_closest(goals), move_away_from(threats)). Routes come from one SDK distance field rooted at
the goals over terrain (walls, holes, altars, enemy spawn), camps, teleports and visible champions within 2 tiles
(farther champions will have moved). The step is a free, reached neighbour that lowers the distance, choosing the one
least exposed to awake camps (their attack range) and enemy champions (attack range + 1), bush tiles last. A ready
dash (Vesper) carries us 2 tiles toward the goal (holes may be crossed) when the goal is >= 4 away and nothing hostile
is visible. Retreat maximises distance to the nearest threat, then exposure, then path length to our spawn; hurt
(<50% HP) or rich (>=20 gems) champions dash away when an enemy champion is within 2. A move returning false stops
walking for the turn.

## Key parameters
kUnitBlockRadius 2 (champions farther away do not block the route), kDashTravelDistance 4, kBushPenalty 0.5,
exposure: awake camp strength within its attack_range; enemy strength + 0.5 x focus within attack_range + 1.
Retreat score: -10 x min(nearest threat, 8) + 0.02 x exposure + 0.1 x path distance to our spawn.
Rest as in the parent: kBankGems 30, kWaryBankGems 15, kRecallHealth 0.35, shared memory 0..1 team target,
2..5 focus target, 6..7 enemy-lock mask.

## Changes this generation
Subsystem rewrite (pathing): move_towards_closest, move_away_from, new dash_to_best, hazards/exposure helpers.
Results (local, deterministic; maps 01fracture, 00road-of-riches, 00gemfight, 01the-farm, 01broken-curve, 00quadrants):
vs e03-07 parent 8/12 (+225) -> 9/12 (+279); fracture 0/2 -> 1/2, road-of-riches 0/2 -> 1/2.
vs e02-01 parent 7/12 (+351) -> 7/12 (+292). vs e04-06 (parent) 6/12 (+88).
On 00lanes, 01gold-rush, 00fragment, 01big-world: 6/8 (+279) vs e03-07, 6/8 (+333) vs e02-01.
vs starter 8/8, e01-01 7/8, e00-01 3/6 valid (opponent's bot errors in 2 games). 0 faults, 0 own bot errors.
168 games used in total.

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
- (e05-04) No travel dash (dash kept for escapes only): 23/36 vs 22/36 on the six test maps but 11/16 (+195) vs
  12/16 (+305) on four others; kept the travel dash.
- (e05-04) Blocking every visible champion on the route (parent behaviour), falling back to the 2-tile radius only when
  cut off: 4/8 vs e03-07 and 2/8 vs e04-06 on farm/fracture/road/gemfight (worse); kept the 2-tile radius.

## Known weaknesses
- 01the-farm: the new pathing often tributes 0 gems there (0-0 and 0-34 vs e03-07; parent 264-115, 440-186); the
  champions collect ~50-90 gems but never bank them. Not diagnosed (no time): suspect banking routes or retreat
  scoring around the dense camps next to the spawn.
- 01fracture still loses to e02-01 both sides; 00road-of-riches A still loses to e03-07, e02-01 and the parent.
- Exposure counts every enemy champion in reach even though awake camps only hit the nearest champion.
