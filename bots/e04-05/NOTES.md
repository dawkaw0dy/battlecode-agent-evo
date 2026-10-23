## Strategy (≤10 lines)
Altar control, built on the starter infrastructure (main loop, distance-field pathing, draft skeleton).
Every champion buys an altar lock (10g) first whenever it shops, and locks any altar within 2 tiles that has no
lock (preferring altars nearer the enemy spawn). Our team still tributes at our own locks, so banking treats
only enemy locks as blocked; if every altar is enemy-locked, bankers walk up and basic-attack the lock.
One warden (first champion to claim shared byte 0) visits altars not seen with our lock in the last 120 rounds,
clears enemy locks there, locks them, and recalls for a new lock when it has 10g. Otherwise it farms.
Farming: camp groups scored by (3*gems + gold) / (path + clear turns + 8); monolith skipped; danger x0.05.
Combat: enemies in range first, champions standing next to an altar are preferred targets (-300 HP in score).
New: when healthy (> 60% HP), step toward an enemy within 3 tiles below 30% HP before banking/farming.

## Key parameters
kLockRange 2, kRelockAfter 120 rounds, kBankGems 20 (>= 5 within 4 tiles of a usable altar), kShopGold 150,
recall 35% HP, spawn heal to 90%. Shared memory: byte 0 warden type+1, bytes 4..7 mask of altars seen unlocked.
Chase: kChaseRange 3, kChaseOwnHealth 0.6, kChaseEnemyHealth 0.3.
Shop: altar lock, Treasure emblem, Jungle emblem, then Power shards.

## Changes this generation
Parent e03-07, mutation: parameter tweak, focus combat. All via tools/benchmark.py (both sides, release).
Test set A: 00lanes 01elk-arena 00pilgrimage 01the-farm 00contest 00stampede (12 games per opponent).
- Baseline (parent copy) on set A: vs e02-01 5/12 +214 mean, vs e00-01 10/12 +280, vs e01-01 11/12 +280,
  vs e03-07 6/12 0 (mirror).
- Reverted: chase hurt enemies (e02-01's rule: range 4, enemy < 50%, own > 60%): vs e02-01 4/12 +110,
  vs e03-07 6/12 +92.
- Reverted: hurt (< 50%) and outnumbered within 4 tiles -> step away instead of fighting: vs e02-01 5/12 +256,
  vs e03-07 5/12 -42; on 00contest A deaths rose 55 -> 92 (chasing enemies get free hits on a fleeing unit).
- Reverted (alone): below recall HP with an enemy champion already in attack range, fight back instead of
  stepping away: vs e02-01 5/12 ~+198, vs e03-07 7/12 +69. One caught engine error 5 on 00lanes (see weaknesses).
- KEPT: chase with stricter thresholds (range 3, enemy < 30%, own > 60%): set A vs e02-01 5/12 +266,
  vs e03-07 7/12 +16 (wins 00contest both sides on gold), vs e00-01 10/12 +294, vs e01-01 11/12 +271.
  Set B (00corners 01hooks 01islands 00gemfight 01big-world 00split): vs e02-01 12/12 +701 (parent 12/12 +654),
  vs e03-07 6/12 +41.
- Reverted: kept chase + fight back at low HP: set A vs e02-01 8/12 +204, vs e00-01 11/12 +368, vs e01-01 10/12
  +264, but vs e03-07 4/12 -113; set B vs e02-01 12/12 +811, vs e03-07 4/12 -128. Loses to the parent.
- 0 own engine faults / caught exceptions in the final version's 72 games (2 errors seen were e00-01's).
  277 games in total.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.
- e04-05: e02-01's chase (range 4, enemy < 50% HP) is worse for this bot than the stricter range 3 / < 30% version.
- e04-05: retreating when hurt and outnumbered gives chasing opponents free kills (deaths 55 -> 92 on 00contest).
- e04-05: fighting back below recall HP instead of stepping away beats e02-01 more often but loses to the parent
  (8/24 over two map sets).

## Known weaknesses
- Fuel: on 00lanes 15-25% of our turns hit the 100k fuel limit (full-map distance fields in choose_group and
  pathing). A suspended turn resumed next round once raised "engine error 5" (in a variant, not the final bot).
- 0-0 maps without altars (00contest, 00stampede) are decided by earned gold; e02-01 still wins most of them.
- 01the-farm loses to e02-01/e00-01; a bot that clears locks is answered only by the single warden.
