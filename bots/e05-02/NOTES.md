## Strategy (≤10 lines)
Parent e04-06 (spell-first pack: Vesper Vale, Ryth, Tavi, Marcella, Willow; spell planner; Lanchester engage;
defense with altar locks) with the camp subsystem (choose_group / farm_camps) rewritten. Groups are scored by
reward per turn: (3 x gems + gold) / (path + clear turns + 5 capture + 3). Clear turns and damage use the LIVE
health/strength of the group's visible camps (table share for spawn tiles out of sight), so half-cleared groups
are finished first, and a group whose reward is available (monsters dead, uncaptured) costs 0 clear turns.
Damage per champion is shared with allies within 8; groups costing >75% HP get x0.05. Respawning groups are
skipped (occupied spawn tiles delay the respawn). Team target x2.5, own target x1.3. A champion standing on a
spawn tile while monsters of its group still stand out of reach walks over to them (parent waited forever).
Spawn-tile lists are cached at startup: the parent's camp code was close to the 100k fuel limit per turn.

## Key parameters
gem weight 3, gold weight 1, capture term 5 + 3, danger cut 0.75 x HP -> x0.05, jungle damage x0.7,
helpers = allies within 8, stickiness 1.3, team target 2.5, stuck skip 12 turns -> 50 rounds.
Shared memory unchanged: 0..1 team target, 2..5 focus target, 6..7 enemy-lock mask.

## Changes this generation
Subsystem rewrite (camps): choose_group and farm_camps rewritten, GroupInfo caches spawn tile lists.
Local results (deterministic, 216 games used in total):
- vs e04-06 (parent), final build: 21/24. 18/18 on 00lanes, 01gold-rush, 00elkring, 01big-world, 00monolith,
  01islands, 01fracture, 00road-of-riches, 01broken-curve (+327 mean); 00gemfight 2/2, 01the-farm 1/2,
  00quadrants 0/2. The parent's worst maps (01fracture, 00road-of-riches, 00gemfight) are now all wins.
- vs e03-07 7/8 (+223), e02-01 5/8 (+251), e01-01 7/8 (+335), e00-01 6/7 + 1 game where e00-01 printed
  `bot error` (+270), starter 7/8 (+398) on 00road-of-riches, 01the-farm, 00lanes, 01gold-rush.
0 faults, 0 `bot error:` lines from this bot in any checked replay.

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
- (e05-02) FUEL: the bot is near the 100k/turn limit. Calling `vision::visible_tiles()` and `bitboard::positions()` per
  camp group per turn, plus a few small per-group ally/enemy loops, made a choose_group with the parent's logic go
  1/6 (-149) vs the parent; with the calls hoisted it was an exact mirror, 3/6 (turns were being suspended). Hoist SDK calls out of loops; cache position lists.
- (e05-02) Clusters-only groups farmed solo (value / allies closer, no team target for them): 2/12 vs parent (-268);
  the pack splits up and farms slowly.
- (e05-02) Walking to a respawning group whose timer ends before arrival: champions stand on its spawn tiles and
  delay the respawn. Removed.
- (e05-02) Contest penalty (score / (1 + enemies within 3 of the group)): 7/12 (+176) vs 9/12 (+301) without.
- (e05-02) Extra allies leave a cleared group one ally already stands on + team target counts >= 2 workers:
  8/12 (+149) vs 9/12 (+301). Gold weight 0.5 after round 1200: 8/12 (+267). Not kept.

## Known weaknesses
- 00quadrants loses to the parent on both sides (-315..-345); 01the-farm is still a coin flip against most bots.
- Fuel headroom is small: any per-turn addition to the camp or combat code must be measured, not assumed free.
- Golem groups are only avoided through the damage estimate; pick_target never wakes one below 90 strength.
