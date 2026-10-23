## Strategy (≤10 lines)
Altar control, built on the starter infrastructure (main loop, distance-field pathing, draft skeleton).
Every champion buys an altar lock (10g) first whenever it shops, and locks any altar within 2 tiles that has no
lock (preferring altars nearer the enemy spawn). Our team still tributes at our own locks, so banking treats
only enemy locks as blocked; if every altar is enemy-locked, bankers walk up and basic-attack the lock.
One warden (first champion to claim shared byte 0) visits altars not seen with our lock in the last 120 rounds,
clears enemy locks there, locks them, and recalls for a new lock when it has 10g. Otherwise it farms.
Farming: camp groups scored by (3*gems + gold) / (path + clear turns + 8); monolith skipped; danger x0.05.
Combat: enemies in range first, champions standing next to an altar are preferred targets (-300 HP in score).
Panel bots never attack locks and skip locked altars, so locked altars deny their tributes completely.

## Key parameters
kLockRange 2, kRelockAfter 120 rounds, kBankGems 20 (>= 5 within 4 tiles of a usable altar), kShopGold 150,
recall 35% HP, spawn heal to 90%. Shared memory: byte 0 warden type+1, bytes 4..7 mask of altars seen unlocked.
Shop: altar lock, Treasure emblem, Jungle emblem, then Power shards.

## Changes this generation
e04-02 (parameter tweak, focus camps). Code is identical to the parent e03-07: no tested change helped.
Baseline (parent copy, tools/benchmark.py, both sides, release):
- vs e02-01 on 00lanes 01elk-arena 00pilgrimage 01the-farm 00contest 01big-world: 7/12, mean +244
  (lost 00lanes B, 01the-farm both, 00contest both on the 0-0 gold tiebreak).
- vs e00-01 on 00lanes 01the-farm 00contest 00stampede 00gem-fields 01gold-rush: 10/12, +284. vs e01-01: 11/12, +302.
- 0 caught exceptions / faults from our bot in 84 games.
Diagnosis (debug prints in 4 local games): on 01the-farm the champions oscillate in place for hundreds of
rounds (r200-680 near spawn, then r720-2000 "banking" 26 gems 8 tiles from a free altar). Cause: the chosen
camp group flips every turn (5<->10, 0<->5: helper count and the danger factor change as allies move) and
allies standing in narrow lanes between sleeping camps flip the distance field (6 <-> 14 steps) each turn.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.
- e04-02: Target-group stickiness 1.3 -> 2.0: vs e02-01 same 4/8 (00lanes B -536 vs -653), vs e00-01 same,
  vs e01-01 10/12 vs 11/12. No clear gain, reverted. 01the-farm still lost.
- e04-02: Route the distance field through allied champions (still never step onto one): vs e02-01 5/8 but
  vs e00-01 lost 00lanes B (5/8); 01the-farm still lost. Reverted.

## Known weaknesses
- 0-0 games (00contest, 00stampede) are decided by gold/kills tiebreak; 01the-farm loses to e02-01/e00-01.
- A bot that clears locks (5 hits) is only answered by the single warden; no dedicated guard stays at altars.
- 01the-farm: champions jam each other between sleeping camps and flip targets (see diagnosis above); a fix
  probably needs per-champion group assignment (shared memory) rather than a scalar tweak.
- Did not measure lock effect per game (enemy gems lost); no wards, dashes or crystals used.
