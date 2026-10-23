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
New: failed step tiles stay blocked for 8 rounds; group reachability computed once when fuel > field_fuel + 20000.

## Changes this generation
Crossover e05-05 (A) x e05-01 (B), focus defense. Both parents share the same defense code (guard_life, bank_gems,
locks); B differs only in fixed bank thresholds, so B's defense was kept as A's. The real cause of A's worst losses
(0 tributed gems) was champions stalling while carrying gems, fixed in A's movement/camp code:
- Failed-step memory: a step that returns false (ally or enemy hidden in a bush) is blocked for 8 rounds in
  move_towards_closest/move_away_from. Before, a champion re-tried the same step every turn while standing on a
  respawning group's spawn tile, so the group never respawned and four allies waited beside it all game holding
  their gems (00road-of-riches A: 0 - 1024).
- Group reachability: computed once from the champion's position (turn 1 has double fuel) and unreachable groups
  are skipped. With fuel short on 60x60 the Chebyshev fallback picked groups on other islands and the whole team
  idled on spawn (01islands: 0 monster damage).
- Results (bench2, both sides, release): vs e05-01 on 01islands/01hooks/00road-of-riches 3/6 +268 (parent A 1/6
  on islands+hooks, -584); vs e03-07 00road-of-riches 2/2 +234 (parent 1/2, -494); vs e05-05 on
  00corners/01fracture/00gemfight 3/6 +52. 0 faults, 0 bot errors. 28 games used.

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

## Known weaknesses
- 00road-of-riches still loses both sides to e05-01 (~-450); 01islands side A narrowly loses to e05-01.
- Side A loses to parent A on 00corners/01fracture/00gemfight (side B wins); side asymmetry not investigated.
- A group blocked by an enemy standing on its spawn tile is still waited on (timer 0) until the stuck rule.
- 00lanes, Golem groups and 01starvation weaknesses from e05-05 not re-tested.
