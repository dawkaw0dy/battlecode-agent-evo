## Strategy (≤10 lines)
e05-05 unchanged (spell pack / sturdy draft from a draft-time map plan, Lanchester engage, lock-aware banking,
respawn-aware camp groups) except the fuel subsystem, rewritten as paths.h/paths.cpp: terrain never changes, so a
distance field towards each goal set (camp group spawn tiles, tiles next to each altar, our spawn) is computed once,
cached (LRU, 96 entries, keyed by a hash of the goal bitboard) and read in O(1) afterwards. Fields are precomputed at
turn end with leftover fuel (altars, then groups nearest first), and the real cost of a field is measured with
fuel_remaining() so the gate adapts to the map. choose_group and bank_gems read cached distances (Chebyshev x5/4
until computed); move_towards_closest follows the cached field for goals > 6 tiles away and falls back to the
parent's compute_to search (near goals, uncached goals, path blocked by units).

## Key parameters
paths: field cost = measured max + 1/8 + 4000 (estimate area*11 + 12000 before the first measurement);
96 cached fields; kWarmReserve 8000 fuel left after a turn-end precompute; kNearGoal 6 (closer goals use the
local search); a mid-turn field is computed only if 20000 fuel would remain. Everything else as e05-05.

## Changes this generation
- New paths.h/paths.cpp (cached terrain fields, measured field cost, step selection along a cached field).
- move_towards_closest, bank_gems, choose_group use the cache; Bot::warm_fields() runs after every turn.
- Results (benchmark.py, both sides, release), final build vs e05-05 on 00lanes/01big-world: 0/4, -747.
  Previous build (choose_group every turn when fuel allowed) vs e05-05 on 00road-of-riches/01hooks/01big-world/
  01fracture/00lanes: 3/10, -306. 0 faults, 0 bot errors. The rewrite is WORSE than the parent; not understood yet.

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
- (e06-01) Bounding the local compute_to by an estimated fuel cost (limit^2 x 50) and falling back to a greedy
  Chebyshev step: champions never left spawn on 01big-world / 01fracture (field unaffordable, greedy step stuck on
  walls), -1000 on most maps. Side-stepping along equal cached-field values made champions oscillate between two tiles.
- (e06-01) Cached-field rewrite overall: -300..-800 vs e05-05 on 00lanes/01big-world even with the parent's fallback
  search restored and the parent's every-4th-turn choose_group cadence. Suspects not checked for lack of time:
  bitboard operator== on goal sets may compare padding (cache misses -> a whole-map field recomputed per call, fuel
  drained); the turn-1 whole-map field mid-turn (~93k measured on 01big-world) leaves little for the rest of turn 1;
  cached step picks the first strictly-better neighbour (ties differ from the parent's search).
  Next agent: log cache hits/misses and fuel per turn before tuning anything else.

## Known weaknesses
- Loses to its parent e05-05 on 00lanes and 01big-world (see above); e05-05's weaknesses (01islands, 00lanes,
  00split side B, golem groups) remain.
