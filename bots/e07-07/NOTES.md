## Strategy (≤10 lines)
e05-05 (itself: parent A e04-06 spell pack, Lanchester engage, lock-aware banking) crossed with parent B e04-04's camp subsystem,
plus a draft-time map plan (plan.h/plan.cpp) stored in draft memory (magic 0xE5) that every champion reads.
The draft measures map area, team size, path length spawn -> nearest altar, reachable non-monolith groups and their
gem value, and chooses: draft (spell pack, or B's sturdy farmers on large maps with >= 20 groups), packing (team-target
multiplier 2.5 on small, 3-champion or gem-starved maps, 1.0 elsewhere) and bank thresholds (20/12 on small maps,
40 on large maps with far altars, else 30/15).
Camps (from B): per-group memory of remaining HP, waiting reward and respawn round; score = value / (approach or
respawn wait + clear + capture); wait next to, never on, a respawning group's spawn tiles.
Fuel: whole-map distance fields are only computed when fuel allows (area*11 + 12k); banking checks cheap
Chebyshev criteria first. This alone fixed the idle/suspended champions on 01fracture-size maps.

## Key parameters
Plan: small area <= 1600, large >= 3600; starved = reachable camp gems < 80 (01elk-arena 72, 01starvation 132);
sturdy draft if large && groups >= 20 && !starved; pack 2.5 if starved || small || team_size <= 3 else 1.0.
Bank 30 / wary 15 (default), 20/12 small, 40 large with altar path > 30. kRecallHealth 0.35, kShopGold 150.
Camp danger: damage = strength x clear_turns / min(helpers, 2), score x (budget/damage)^2 (min 0.02);
groups needing > 25 rounds of respawn wait are skipped. field_fuel = w*h*11 + 12000; kScanFuel 15000.
Shared memory unchanged from A: 0..1 team target, 2..5 focus target, 6..7 enemy-lock mask.
New: failed step tiles stay blocked for 20 rounds; group reachability computed once when fuel > field_fuel + 20000.
Pathing tie-breaks (e07-07): equal-distance steps prefer max Chebyshev distance to visible enemy champions; equal
retreat steps prefer the tile nearest our spawn region.

## Changes this generation
Parameter tweak of e06-04, focus pathing (benchmark, both sides, vs e05-05 / e06-05 on 01elk-arena, 00rushbait-v2,
01broken-curve, 00lanes; parent: 3/8 -161, 1/8 -429):
1. Failed-step memory 8 -> 20 rounds: 3/8 -165, 2/8 -267. Kept (one more win vs e06-05).
2. move_towards_closest: among steps with equal field distance pick the one farthest from visible enemies (before:
   first in scan order): 4/8 -128, 2/8 -239. Kept.
3. move_away_from: among equally safe retreat steps pick the one closest to our spawn: 6/8 +35, 1/8 -287
   (total 7/16, -126 vs parent 4/16, -295). Kept.
Regression check vs e03-07, e04-06, e01-01 on 00road-of-riches, 01islands, 00corners, 01fracture: 22/24 for both
candidate and parent; margins lower (+402/+508/+680 vs +442/+615/+828). 0 faults, 0 caught exceptions. 112 games.

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
- (e05-05) B's camp code gated on `fuel_remaining() > 80000` inside A: A spends most fuel before farming, so the choice
  never re-ran, state went stale and champions idled (-1000 gems on most maps). Gate by map area instead.
- (e05-05) B's sturdy draft everywhere: lost A's strong maps (01elk-arena -775 vs A). Spell draft everywhere: 01fracture
  -47 vs B (sturdy +216..+360). Hence the map-dependent draft.
- (e05-05) Pack 2.5 everywhere: 00lanes -278 vs A (pack 1.0: -35..-250). Pack 1.0 on 00gemfight: lost side A -750.
- (e05-05) Danger damage / min(helpers, 2) instead of / helpers did not fix 00split (golem group drains HP); kept as safer.
- (e06-04) None of my own failed this generation; untested ideas: skip a group whose respawn timer stays 0
  (blocked by an occupant) for many rounds; vary banking under threat.
- (e07-07) Nothing reverted. Bad-step memory 20 alone was mixed (vs e05-05 3/8 -164, vs e06-05 2/8 -267 vs parent 3/8, 1/8);
  kept only as the base for the two tie-breaks below.

## Known weaknesses
- Side A loses big fights the parent also loses: 01elk-arena/00rushbait-v2 side A vs e05-05 (-960), 29 deaths vs 7 in
  the same mirror draft; engage logic, not pathing, is the likely cause.
- 01broken-curve and 00lanes still lose to e06-05 on both sides; 00rushbait-v2 side A loses to everyone tested.
- Gem margins vs weaker bots dropped ~10-20% with the tie-breaks (wins unchanged).
- From parent: 00road-of-riches vs e05-01, respawn-blocked groups, Golem groups, 01starvation not re-tested.
