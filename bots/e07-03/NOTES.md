## Strategy (≤10 lines)
Parent e06-05 (e05-05 camps/banking/engage + map plan in draft memory + rewritten spell planner and travel
dashes) with the fuel subsystem rewritten from measurements: `nav` (nav.h/.cpp) caches terrain-only path
fields per goal set for the whole game (camp group i and altar i in fixed slots, other goal sets once they
repeat), and choose_group / bank_gems / move_towards_closest read them instead of recomputing a field every
call. Fuel is measured, not guessed: a field costs ~14k on 61x71, wasm loops ~50 fuel per iteration, host
calls ~5-100, so per-tile loops and board allocations were cut where they ran every turn. Turns no longer run
dry (e.g. 01fracture: 486 suspended farm/bank blocks per 300 rounds -> 10; road-of-riches moves: 299 -> 29).
A bug fix came with it: wall tiles in an altar's approach square are no longer field roots (we never tributed
on 00road-of-riches: 93 gems carried, 0 tributed; now +600..+1000 vs everyone).

## Key parameters
field_cost = largest measured field cost + 4000 (before the first one: 4 fuel/tile + 4000).
Reserves after computing a field: walk 20k, group scan 40k, altar 30k; fallback search needs 12k.
Cache: 64 unpinned fields (LRU) + pinned slots (groups, altars from slot 4096); a goal set is cached on its
second request (12 pending). Key prefilter: tile count + first tile, then full board compare.
Cached descent only with no champion/camp within 3 tiles (kCrowdRadius); else unit-aware search bounded at
terrain distance + 20. choose_group every 3rd turn (fuel >= 40k), every 6th regardless, or when the target is
invalid; travel = one field read per group (box distance x 5/4 until its field is cached); near-group tests
check the bounding box before the tiles. Everything else (camps, spells, banking, engage, draft) unchanged.

## Changes this generation
- New nav.h/nav.cpp (field cache); move_towards_closest, choose_group's travel, bank_gems' altar distances,
  observe_groups' visibility test, farm_camps' re-choose cadence and dash_along's enemy-within-8 test rewritten
  for fuel; GroupInfo carries a bounding box. Interfaces unchanged.
- Results (benchmark.py, release, both sides, 0 faults / 0 caught exceptions in all games, ~256 games total):
  vs e06-05 (parent) 11/12 +660 on 01hooks/01elk-arena/00road-of-riches/01islands/00gemfight/01fracture and
  8/10 +270 on 00ambush/01big-world/00lanes/01starvation/00expanse.
  vs e03-07 12/12 +675, e05-05 9/12 +520, starter 12/12 +782 (islands/road-of-riches/split/elk-arena/hooks/
  contest); vs e04-06 9/10 +604, e02-01 10/10 +668, e01-01 10/10 +771, e00-01 10/10 +813 (the 5 maps above).

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
- (e06-05) Travel dash on large maps (sturdy draft, pure farming races): vs parent 2/8 -56 with dash vs 6/8 +93 without
  on 01fracture/01big-world/00expanse/01the-farm; vs e03-07 neutral (+370 vs +390). Dash is off on large maps.
- (e06-05) Ungated dash on PvP maps (00contest): champions leap into unseen enemies; 83-102 deaths vs parent. Gated by
  recent enemy sighting (15 rounds) and HP >= 60%; vs e03-07 +249 -> +275.
- (e06-05) Single reversions to the parent's planner pieces (heal rule, area centres on unit tiles only, raw damage
  and worth 200, threshold 5 + camp stun 0.5) did not change 00contest/00ambush vs parent: those maps are
  side-chaotic snowballs in near-mirror matches (parent vs parent: side A wins 100 kills to 6).
- (e06-05) Stronger shield rule (shield as soon as damage is incoming, no HP gate) vs the parent's (< 75% HP): parent's
  gate measured worse on large maps (-78 vs -56 vs parent), so shields go up early.
- (e07-03) Copying each cached field into a local uint16 array (export_linear_buffer + visited.positions()):
  ~75k fuel per field instead of ~14k. Plain wasm loops cost ~50 fuel per iteration (a 1000-step loop: 54k),
  host calls only ~5-100, so reading the engine field (is_set + get) is the cheap option.
- (e07-03) Descending the cached terrain field everywhere (also next to units): 01hooks -850, 01elk-arena
  -700 vs the parent. The cached field is used only with no unit within 3 tiles; near units, the unit-aware search.
- (e07-03) Mixing cached exact distances with Chebyshev estimates under the parent's 11 fuel/tile cost
  estimate: the gate almost never let a field be cached, choose_group ran on estimates, 5/16 vs the parent.
- (e07-03) Re-choosing the camp group every turn while fuel >= 75k (instead of every 3rd turn): no better
  (2/8 vs 5/8 on elk-arena/gemfight/fracture/road); the noise there is large.


## Known weaknesses
- 00contest: 0-0 games decided by tiebreak (nobody tributes); side-dependent vs e05-05.
- 00gemfight / 01elk-arena / 01hooks vs e05-05 and the parent: still side-dependent snowballs on some sides.
- The per-turn fuel still peaks near the 100k limit in turns with fights plus a group scan (p90 ~85k); the
  spell planner's area-centre search (tiles_around per target) was not optimised.
- 01the-farm: nobody tributes in 2000 rounds; decided by gold (not re-tested).
