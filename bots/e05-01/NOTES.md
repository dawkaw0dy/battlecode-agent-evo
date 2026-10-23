## Strategy (≤10 lines)
Parent e04-06 (spell-first pack: Vesper Vale, Ryth, Tavi, Marcella, Willow; spell planner; Lanchester engage; altar
lock/defense subsystem) with the camp subsystem (choose_group/farm_camps) rewritten. Each group is scored as
(gems x 3, x5 after round 1200, + gold) / (path + clear turns + 5 capture phases + 3). Clear turns and damage use
live camp health whenever a spawn tile is in vision (half-cleared groups become cheap), and a cleared group whose
reward is waiting costs only the walk and capture. Damage is simulated per camp, lowest health first
(sum strength_i x kill_time_i, shared by the allies within 8); over 75% of our HP -> x0.05 last resort. Respawning
groups are skipped until their remembered timer ends. Soft packing: every champion writes its pick as team target
(x2.5 for the others), current group x1.3. Monoliths are never farmed.

## Key parameters
kCaptureTurns 5, kSlack 3, kDangerShare 0.75, kFocusShare 0.2 (spell dps share of Focus), kTeamBonus 2.5,
kStickiness 1.3, gem weight 3 (5 after round 1200), stuck skip 12 turns -> 50 rounds. Shared memory unchanged:
0..1 team target, 2..5 focus target, 6..7 enemy-lock mask. GroupInfo precomputes spawn tile lists for fuel.

## Changes this generation
Subsystem rewrite (camps): new choose_group (live health, reward-ready groups, per-camp kill-order damage model,
late gem weight, cheap scan) and farm_camps (team target may be cleared to -1). Rest of the bot unchanged.
Results (local, deterministic): vs e04-06 (parent) 16/20 (+145) on 00ambush, 00lanes, 00packs, 01broken-curve,
01concentrated-wealth, 00road-of-riches, 00gemfight, 01fracture, 00quadrants, 01the-farm (parent's worst maps
01fracture/00road-of-riches/00gemfight now 2/2, 2/2, 1/2). On 00split, 01starvation, 01elk-arena,
00road-of-riches: vs e03-07 8/8 (+713), vs e02-01 8/8 (+795) (parent: 6/8, 7/8). On 01gold-rush, 00elkring,
01big-world: vs starter 6/6, e00-01 5/6, e01-01 5/6. 0 faults, 0 bot errors. 312 games used in total.

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
- (e05-01) First camp rewrite scanned every group with bitboard ANDs, per-group vision fetches and ally/enemy
  distance loops: turns ran out of 100k fuel (fuel log full of 100000), 9/28 and worse vs the parent. Keep the scan cheap.
- (e05-01) Solo farming of clusters/wolves (ally closer within 2 takes it, x0.25) plus team target only for groups needing
  a pack: champions split up and died to invading packs (00packs B: 22 deaths, 0 kills). Soft packing stays.
- (e05-01) Team target written only when the old one is captured (hysteresis): no gain; contested-group penalty x0.4
  (enemies within 4 of spawns): lost the contested centre groups (01elk-arena, 00ambush). Neither kept.
- (e05-01) Waiting beside respawning groups (timer <= 30 on arrival) was never shown to help; respawning groups are skipped.


## Known weaknesses
- 00gemfight A (-800), 01broken-curve A (-550), 00quadrants B and 01concentrated-wealth B still lose to the parent.
- 01big-world: 4/6 vs older bots (long walks; groups far from the pack are rarely taken).
- Golem groups are scored like any other; pick_target will not wake a golem below 90 Strength, so a pack can
  stall next to one until the stuck rule skips it.
- No explicit split of the team for cheap cluster groups (tried, see above); farming is serial on cluster-rich maps.
