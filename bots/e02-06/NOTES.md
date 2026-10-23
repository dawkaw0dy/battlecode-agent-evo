## Strategy (≤10 lines)
Scaling economy from e00-01 (unchanged farming, shopping, recall); endgame subsystem rewritten in endgame.cpp.
Draft: Vesper Vale, Marcella, then Vendetta, Varg, Ryth, Tavi, Willow, Ren, ...
Camp groups scored by (3*gems + gold) / (path distance + clear turns + 8); team target in shared memory.
Endgame: altar chosen by real path distance (distance field), enemy-locked altars cost 2 turns per lock hit,
+6 per enemy within 4 of the altar; ally-locked altars are usable (parent skipped every locked altar).
Bank at >= 30 gems, >= 5 within path 4 of an altar, when the bag completes 1000, or when rounds left
<= altar cost + channel + 25 (parent: fixed round 1800). Banking decision is sticky.
Enemy lock on our altar: stand in range and clear it with basic attacks instead of wandering off.
Denial: buy one altar lock on spawn from round 150; place it (range 2) on an altar where a visible enemy is
tributing (cancels the channel) or stands with >= 15 gems / a game-winning bag.

## Key parameters
kBankGems 30, near-altar bank 5 gems at path <= 4, kTimeMargin 25 rounds, lock range 2, lock hit cost 2,
enemy-near-altar cost 6, lock bought from round 150. Rest as parent (shop 150 gold, recall 35% HP).

## Changes this generation
- Rewrote bank_gems (endgame.cpp) + new hooks stock_lock() (on spawn, before shopping) and deny_tributes()
  (each turn after heals). Same interface: play_turn still calls bank_gems().
- Guards for inherited exceptions: camp_group::state() (engine error 97 "camp group is not in vision" on 00lanes),
  fight_best_target / heal_allies (error 98 stale unit). No caught exceptions in the final runs.
- vs e01-01 on 24 maps (48 games): variant kept 26W (+91 mean gems), parent 23W/47 (+20, 3 caught exceptions), variant without locks 23W (-40),
  variant with risk-banking 20W.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- (e02-06) Early "risk" banking (bank at >= 12 gems when an enemy is within 6, and at >= 20 within path 8 of an altar):
  20W/48 vs e01-01 against 26W without it; it cuts farming time more than it saves gems.
- (e02-06) Same endgame without altar locks (no stock/deny): 23W/47, -40 mean gems vs 26W/+91 with locks.
- (e02-06) camp_group::state() can throw error 97 even when one of the group's spawn tiles is visible: guard it.

## Known weaknesses
- 00lanes (0/2, ~-370), 01fracture, 01islands, 00expanse, 00road-of-riches still lose to e01-01: economy, not endgame.
- High game-to-game variance: single-map results flip between sides; 48-game samples differ by ~3 wins.
- Locks are only placed reactively (enemy visible next to an altar); no proactive altar camping, no wards.
