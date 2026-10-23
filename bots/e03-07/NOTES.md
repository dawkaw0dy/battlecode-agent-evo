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
New immigrant bot (archetype altar-control), all tests via tools/benchmark.py (both sides, release):
- vs e02-01 on 00contest 01the-farm 00corners 01hooks 00lanes 01islands: 7/12, mean +404
  (00corners/01hooks/01islands near shutouts; lost 00contest 0-0 tiebreak, 01the-farm, 00lanes B).
- vs starter on 00contest 01the-farm 00stampede 01hooks: 8/8. vs e00-01: 6/8. vs e01-01: 7/8.
- 0 engine faults, 0 caught exceptions in 36 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.

## Known weaknesses
- 0-0 games (00contest, 00stampede) are decided by gold/kills tiebreak; 01the-farm loses to e02-01/e00-01.
- A bot that clears locks (5 hits) is only answered by the single warden; no dedicated guard stays at altars.
- Did not measure lock effect per game (enemy gems lost); no wards, dashes or crystals used.
