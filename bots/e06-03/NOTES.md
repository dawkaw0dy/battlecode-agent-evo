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
Pathing (e06-03): kRouteUnitRadius 2 (only champions within 2 tiles are walls in the step field), camp-passable
fallback field, kUnreachableSkip 300 (group with no route even through camps), kChooseFields 2 (re-choose the
target on spare fuel only when 2 whole-map fields fit).

## Changes this generation
e06-03 (parameter tweak, focus pathing). Parent baseline on 00road-of-riches/01islands/01hooks/00lanes/00split/
01fracture, both sides: vs e03-07 7/12 (+1308 gems total), vs e04-06 5/12 (-1657).
Root cause found on 01islands: champions sat on spawn for the whole game. observe_groups costs ~40k fuel there,
so choose_group fell back to Chebyshev distances and picked group 24 on a hole-ringed island; the step field
(correctly) found no route and returned no step, and the 12-turn stuck skip was undone because choose_group's
low-fuel early return handed back the skipped target every time. Kept, in order:
- A: champions farther than 2 tiles do not block the step field (they move). Alone ~neutral (12/24 -> 11/24
  wins, -349 -> -15 gems); with C/D it is essential (ablation: radius 200 with B/C/D: 7/24, -6290 total).
- B: re-choose on spare fuel only when kChooseFields=2 fields fit, so the step field is not starved on 60x60.
  Ablation (kChooseFields 1, with A/C/D): 14/24 +1779 vs 15/24 +2971 with it.
- C: if the step field misses us, retry with camps passable (walking up to them clears them); if still no
  route, set goal_unreachable and farm_camps skips the group for 300 rounds.
- D: choose_group's low-fuel early return never returns a skipped target.
Final (A+B+C+D): vs e03-07 10/12 +3302, vs e04-06 5/12 -331 (same 6 maps); vs e05-05 (parent) 8/8 +1897 on
01islands/00lanes/01big-world/01starvation; e02-01 5/8 +1917, e01-01 4/8 +1086, starter 6/6 +3800,
e00-01 3/5 valid +1446 (loses 01islands; its own bot error on 00road-of-riches A). 0 faults, 0 bot errors from this bot.

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
- (e06-03) Terrain-only fallback field instead of camp-passable (so allies near us never make a goal
  "unreachable"): 15/24 but +2393 vs +2971; 00road-of-riches side A -978/-979 instead of -615/+32. Reverted.
- (e06-03) The radius-2 unit rule alone (without the unreachable skip/early-return fix) did nothing for 01islands:
  the problem there was target choice under fuel starvation, not the route.

## Known weaknesses
- 01islands still loses to most panel bots (-150..-1000): observe_groups costs ~40k fuel per turn on its ~45
  groups, champions often suspend mid-turn and act every other round; most groups need a dash or teleport
  crystal (neither is used for travel).
- 00lanes loses to e02-01/e01-01 by 200-500; 00road-of-riches side A vs e04-06 -615; 01hooks side B vs e04-06 -968.
- Golem groups: no coordination, champions drain HP on them (00split).
