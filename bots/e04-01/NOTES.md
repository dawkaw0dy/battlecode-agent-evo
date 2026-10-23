## Strategy (≤10 lines)
Altar control (parent e03-07), plus a map-adaptive plan computed in the draft and stored in draft memory.
Draft: compute_plan() reads map size, altar count, team size and the walking distance from our spawn to the
nearest altar; the Plan struct (plan.h) is written to draft memory, and every champion reads it via plan()
(recomputed from the map if draft memory is missing or has another version).
Every champion buys an altar lock first when shopping and locks any altar within 2 tiles without our lock.
One warden (first champion to claim shared byte 0) re-locks altars not seen with our lock recently; others farm.
Farming: camp groups scored by (3*gems + gold) / (path + clear turns + 8); monolith skipped; danger x0.05.
Combat: enemies in range first; champions next to an altar are preferred targets (-300 HP in score).

## Key parameters
Plan (per map): bank_gems = clamp(10 + spawn-to-altar distance, 20, 30); relock interval 120 rounds, 180 on
maps with width*height >= 4000; shop at 150 gold; warden on for every team size.
Fixed: kLockRange 2, bank >= 5 gems within 4 tiles of a usable altar, recall 35% HP, spawn heal to 90%.
Shared memory: byte 0 warden type+1, bytes 4..7 mask of altars seen unlocked. Draft memory: Plan struct, version 1.
Shop: altar lock, Treasure emblem, Jungle emblem, then Power shards.

## Changes this generation
All via tools/benchmark.py (both sides, release). 106 games total.
- Plan infrastructure (draft memory round trip verified with a debug print, since removed).
- Kept: bank threshold grows with altar distance (floor 20, so near-altar maps keep the parent value).
  The first version (floor 14) was better vs e02-01 on 00lanes/01elk-arena/00pilgrimage/01the-farm/
  01big-world/01starvation (10/12, +331 vs parent 9/12, +287) and vs e00-01 on six small/3-champion maps
  (12/12, +792 vs +778), but lost 2/7 (-196) to the parent on 00lanes/01elk-arena/00pilgrimage/00rushbait-v2,
  so the floor was raised back to 20.
- Kept: relock interval 180 instead of 120 on big maps: vs e02-01 on 00ambush/00expanse/01big-world/
  01starvation 7/8, +402 (before: 7/8, +354).
- Final build vs e03-07 (parent) on 00lanes/01elk-arena/00pilgrimage/00rushbait-v2: 3/8, -112; this build
  plays like the parent on those maps, so the mirror is decided by small timing differences (see weaknesses).
  vs starter on 00elkring/01hooks: 4/4. 0 faults and 0 caught exceptions from our bot in all games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.
- e04-01: no warden on 3-champion maps (everyone farms): vs e02-01 on 00collision/00fragment/00packs/01broken-curve/
  01gold-rush 9/10 and +443 avg vs 10/10 and +754 with the warden, plus 01the-farm 0/2. Reverted.
- e04-01: bank threshold below 20 on near-altar maps (clamp(10 + dist, 14, 30)): helped vs e02-01/e00-01 but lost
  2/7 to the parent. Reverted to floor 20.

## Known weaknesses
- Mirror and near-mirror games swing on small fuel/timing differences: a build that plays like the parent still
  went 3/8 against it, so 8-game samples cannot resolve small changes.
- e03-07 (parent) printed one `bot error: engine error 5` on 00lanes (seen in its team's stdout); not investigated
  in this bot, which shares the code path (possibly bitboard/tiles_around near the map edge).
- 01the-farm still loses to e02-01; 0-0 games are decided by gold/kills tiebreak.
- Draft picks are still fixed; the plan does not yet change the draft or the farming order.
