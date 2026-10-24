## Strategy (≤10 lines)
Map-adaptive territory farming on top of e06-05's infrastructure (pathing, spells, combat, banking, locks, draft).
Root cause of the parent's worst losses: `choose_group` built a path field and then ran out of fuel after scoring
~2 groups, returning the old target forever. On 00road-of-riches all five champions sat next to one 22-gem group
for 1300 rounds (44 gems collected). On 01islands, straight-line fallbacks chased groups on other islands.
New plan at draft time (stored in draft memory after the Plan struct, one byte per camp group):
- reachability: groups with no walking path from our spawn are marked and never targeted;
- territories: on spread maps (pack 1.0, at least 3 reachable groups per champion) groups are sorted by angle
  around our spawn and cut into one equal-gem-value arc per champion (cut at the widest gap); each champion
  (draft-order index in `champions(ally)`) scores its own groups x1.8 (large) / x1.4 (medium), others x0.8 beyond 6 tiles.
Runtime scan: current target first, rotating start, path field only when the scan still fits after it.

## Key parameters
Path field used when fuel > field_fuel + 15000 + 150 x groups (gem-starved maps: field_fuel only, like the parent).
Fuel-cut scan returns the best group scanned so far (it always includes the current target); gem-starved maps
(plan camp_gems < 20, e.g. 01elk-arena) keep the current target so the pack stays together.
Spread: pack_x10 <= 10 and reachable groups >= 3 x team size. zone_x10 18 large / 14 medium. Plan magic 0xE7.
Everything else as e06-05 (spell planner weights, dash gates, bank thresholds, sturdy/spell draft split).

## Changes this generation
- plan.cpp/h: `assign_zones` (reachability + territories), `group_zone`, `group_unreachable`, `my_zone`; draft memory
  holds Plan + per-group bytes.
- bot.cpp `choose_group`: fuel-safe rotating scan, unreachable groups skipped, territory multipliers.
- Results (benchmark.py, both sides, release; ~193 games total, 0 faults / 0 bot errors on our side):
  worst-loss maps vs e03-07: road-of-riches -1024/-983 (parent) -> -97/+852, islands +462/-392 -> +722/+792.
  vs e06-05 (parent) 11/16 +308 on 8 maps (road-of-riches +922, islands +1017, split +919; elk-arena -648 before the
  starved rule, after it +900/-120); large maps vs parent after the final threshold: 6/12 -4.
  vs e05-05 11/16 +243, e04-06 10/16 +222, e02-01 16/16 +640 (8 maps, pre-final build).
  Final build, 6 mixed maps (3-champion fragment/packs/gold-rush, corners, lanes, yellow-brick-road):
  e03-07 12/12 +469, e01-01 11/12 +588, e00-01 11/12 +617, starter 9/11 valid +613.

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
- (e07-05) Strict fuel gate without reachability: Chebyshev fallback targets other islands; 01islands 30-60 gems
  tributed (-236/-472 vs e03-07). Fixed by the draft-time reachability bytes.
- (e07-05) Sticky target on every pack map (pack 2.5) when the scan is cut: fixes elk-arena but 00road-of-riches
  -852 side A vs parent. Only gem-starved maps keep the target.
- (e07-05) Territories with >= 2 groups per champion: 01starvation -544 vs parent (zones off: 0). Now >= 3 per champion.
- (e07-05) Softer territory bonus x1.3 everywhere: vs parent 2/8 -114 on fracture/big-world/expanse/corners
  (corners -328). Keep x1.8 on large maps.

## Known weaknesses
- Large farming-race maps vs the parent are about even (00expanse -212, 01fracture -73); territories are not tuned.
- 00gemfight / 01elk-arena / 00contest stay side-dependent snowballs in near-mirror matches.
- 01yellow-brick-road: everyone tributes under 110 gems; decided by small margins.
- Champions never use teleport crystals, so groups the draft marks unreachable are ignored for the whole game.
