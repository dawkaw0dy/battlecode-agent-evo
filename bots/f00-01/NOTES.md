## Strategy (≤10 lines)
Altar control, built from the starter's infrastructure (main loop, draft skeleton, distance-field pathing).
Every champion buys an altar lock whenever it shops (10g) and locks any free altar within 2 tiles (nearest the
enemy first), including the altar it is about to tribute at: our team still tributes at our locks, theirs must break them.
One warden (Vendetta, else the last champion; teams of 3+) waits beside the altar the enemy reaches first, locks any
altar where an enemy is tributing or standing with gems (cancels the channel), fights the lock breakers, and recalls
for a new lock. Everyone else farms camp groups scored by (5*gems + gold) / (path + respawn wait + clear time + 6),
with a proportional penalty for groups that would out-damage us, banks at 25 gems (or when an altar is close),
breaks enemy locks, stuns tributing enemies, recalls at 45% HP after stepping out of awake camps' reach.
Draft: Vesper Vale, Marcella, Vendetta, Ryth, Varg, Tavi, Willow, ... (sturdy strength champions).

## Key parameters
kRecallHp 0.45, kHealTo 0.7 (leave spawn), kBankGems 25, kRecallGold 180 (shopping recall), kChaseRadius 3,
kGemWeight 5, kDangerExp 1.5 (score *= (0.8*maxHP/expected damage)^1.5), lock range 2, warden for team >= 3,
second warden disabled (kSecondWardenTeam 99). Target re-chosen every 8 rounds; full path scan on maps <= 70x70,
36-step bounded scan + 1.5x Chebyshev estimate on larger maps. Capture timeout 12 rounds -> skip group 150 rounds;
no gold/gems/damage for 30 rounds -> skip target 150 rounds. Shared memory bytes 0..4: each champion's target group.

## Changes this generation
New immigrant bot (archetype altar-control). All tests vs starter with tools/benchmark.py (both sides, release):
- Final full run, 40 maps both sides: 76/79 valid wins, mean margin +654, 0 faults, 0 caught exceptions on our side
  (losses: 00rushbait-v2 A -52, 01islands B -60, 01shortcut A 0-0 tiebreak; the one invalid game is the starter's error).
- Lock every free altar we pass (vs warden-only locking): +454 gems over 16 games.
- Warden on vs off: off lost 00ambush both sides and dropped 00gem-fields B +893 -> +245; warden kept.
- Retreat from awake camps before recalling: +994 over 16 games (most deaths were to camps, losing the gems).
- Gem weight 5 + proportional danger penalty (was x0.05 hard cut): 16/16, +1500 over 16 games (00ambush -4 -> +617).
- Full path scan on <= 70x70 maps: +960 over 12 games (00reverse-spiral A -116 -> +163, 00gemfight A +120 -> +887).
- Leave spawn at 70% HP (was 90%): +860 over 16 games.
- Fuel: cached terrain fields per goal, cached blocked mask, bounded target scan on 100x100 maps
  (01big-world fuelExhausted events 1153 -> 36).

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles: only step to free tiles in `visited`.
- A bitboard computed before `distance_field::compute_to` no longer showed champions it had contained when read
  after the compute: recompute the blocked mask after the field. Allies hidden in bushes are still "known
  occupied": add every ally position from `champions(team::ally)` to the blocked mask.
- Standing on spawn tiles of a respawning group delays its respawn forever (we parked on them): wait 1-2 tiles away.
- 01yellow-brick-road: a 30-tile gold-cluster group spans both spawns, so its capture never completes; capture timeout
  and the no-progress rule fix it (0 -> +900 gems). Moving towards spawn tiles while standing on one never reached
  live camps of the same group: move to the live camp tiles instead.
- Banking by Chebyshev distance to altars: 00great-wall -182/-211 (altars behind the wall). Use path distance.
- Stuck rule counting respawn waits as "no progress": champions hopped between respawning groups (00great-wall).
- Second warden on 5-champion teams: 01corridors/00ambush/01fracture -200..-400 per map vs one warden.
- Recall at 35%/40%: about equal gems to 45%, one more loss on 01corridors; kept 45%.
- Shopping recall at 300 gold: -136 over 16 games vs 180. Leave spawn at 50% HP: -336 vs 70%.
- Bank threshold 40 vs 25: equal vs starter (6991 vs 7004); kept 25 (less to lose to kills).
- Gem weight 8 (vs 5): 7390 vs 7864 gems over the same 16 games. Danger exponent 1.0: 6240; 2.0: 7346 (1.5: 7864).
- 00rushbait-v2 side A: our spawn is boxed in by the enemy spawn tiles, no group is walkable and we start with 0 gold
  (no crystal): nothing to fix there.

## Known weaknesses
- Only tested against the starter (the task's test opponent); combat and spells are simple (no dashes, no wards except
  the warden's, no teleport crystals), so 01islands (8/46 groups walkable) stays near 0 gems.
- 0-0 maps (00contest, 00stampede, 01boss-battle, 01shortcut) are decided by the gold/kills tiebreak.
- Did not measure per-lock gem denial directly (the replay has no lock events); lock value measured only as total margin.
- Lock-breaking opponents lose ~5 attacks per lock, much less than the starter (which skips locked altars entirely).
