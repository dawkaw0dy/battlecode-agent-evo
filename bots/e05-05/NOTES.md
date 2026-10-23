## Strategy (≤10 lines)
Parent A e04-06 (spell pack, Lanchester engage, lock-aware banking) crossed with parent B e04-04's camp subsystem,
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

## Changes this generation
- plan.cpp/plan.h: draft-time map analysis, Plan stored in draft memory (draft reads it from a static during draft).
- Ported B's observe_groups/choose_group/target_invalid/farm_camps (respawn-aware), kept A's team-target packing.
- Fixed a port bug: a champion standing on a cleared, respawning group's spawn tile blocked the respawn forever;
  now it holds only while the reward is waiting, else steps off. observe_groups runs every farming turn.
- bank_gems: cheap trigger check before the whole-map field; Chebyshev estimate when fuel is short.
- Results (benchmark.py, both sides, release; final build): vs e04-04 on 01fracture/00corners/01islands/01big-world
  4/8 +96, earlier final-draft run on 00gemfight/00road-of-riches/00corners/00split/01elk-arena/01fracture 9/12 +315;
  vs e04-06 on 01fracture/00corners/01islands/01big-world 6/8 +306; vs starter 6/6 +710, e00-01 4/5 valid +617,
  e01-01 4/6 +523, e02-01 4/6 +489, e03-07 4/6 +442 (01fracture, 00lanes, 00fragment). 0 faults, 0 bot errors
  from this bot (e00-01 throws its own on 00lanes). Parent A vs B baseline on 10 maps was 6/20, -133.

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

## Known weaknesses
- 01islands loses to A (-521) and narrowly to B; 00lanes loses to every panel bot by 100-400; 00split side B and
  00road-of-riches side A lose big vs B (side asymmetry not investigated).
- 01starvation (gem total 132, not "starved" by the plan) loses narrowly to A.
- Golem groups: no coordination, champions drain HP on them (00split).
