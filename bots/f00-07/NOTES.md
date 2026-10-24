## Strategy (≤10 lines)
Vision-ambush immigrant built from the starter's infrastructure only (main loop, build layout).
Draft: Vesper Vale, Ryth, Tavi, Vendetta, Marcella (stuns and burst for picks, sturdy strength farmers).
Farming: cached static distance fields per camp group (about 2.3 fuel per tile), group score = (gems + 0.3 gold) /
(path or respawn wait + clear time + 8), soft camp-danger penalty, Monolith only with 3+ allies in sight; shared
team-target group (x2.5 on maps <= 2500 tiles, x1.2 on larger ones). Stall guards ban a group for a while.
Sightings: 8-byte shared memory carries the most vulnerable enemy seen (flags: count, weak, at altar), a call to arms
(the fight target, so the team focuses it) and the team camp group. Champions answer calls within 14 tiles, and
answer "enemy tributing at an altar" sightings within 16 (channelling, full bag: the ambush the archetype is about).
Combat: Lanchester balance (sum HP x sum DPS), engage at >= 0.9, finish weak targets, never dive into the enemy
spawn zone (and ignore enemies resting there), stun first then damage spells then basic attack + attack reset.
Wards: placed on hidden bushes near the group we farm or capture, each champion type on its own tile class.

## Key parameters
kEngage 0.9 (1.3 tested: worse). kTeamBonus 2.5 small / 1.2 large (area 2500). kGoldWeight 0.3. kHealRecall 0.5
(0.4 neutral). Shopping recall at 400 gold (300 neutral), or 150 gold and < 60% HP. Recall < 35% HP when no enemy
within 5 and no awake camp within 4. Bank at 35 gems, 12 within 6 of an altar, 10 with enemies seen, any after 1880.
Group skipped when respawn wait > 30; banned 40 rounds after 9 contested capture turns or a blocked path, 100 rounds
after 90 rounds without income on the same target. Field built only when fuel > area*3 + 15000.
Items: 5 power shards of the main stat, 5 durability health, 5 main stat, then health/main alternately.

## Changes this generation
New bot (immigrant, generation 0). Benchmarks (bench2, both sides): vs starter 77/80 on all 40 maps, 0 faults,
0 errors; vs e07-07 (run-1 champion) 27/80 on all maps; vs e03-07 10/12, vs e06-04 5/12 on 6 other maps.
Big fixes along the way: dead camps stay in camp_tiles() (use awake | asleep), dead champions have no position
(check is_alive first), static fields were never built on large maps (fuel guard 12/tile, real cost 2.3/tile),
islands need reachability checks, contested captures and blocked paths need bans.

## Tried and failed (keep entries from the parent's notes; add yours)
- (f00-07) No-step-back rule against two-tile oscillation: 0/8 vs e07-07 (blocks legitimate moves). Oscillation
  detector that forces static-field movement: 1/8. Both reverted; Ryth still oscillates near walls sometimes.
- (f00-07) Disabling attack resets entirely: 3/16 vs 10/16 with them. Resets are kept; the engine's rare rejection
  ("an available attack cannot be reset") is caught locally around ability use and treated as not cast.
- (f00-07) Ranged "hold and let them walk into range" rule: 7/16, -229 (vs 9-10/16). Reverted.
- (f00-07) Jungle then Treasure item order: 5/16. Willow drafted 4th: 8/16 and her ally heal threw engine error 30
  ("target relationship is not allowed"), so heal casting is removed. Ally radius 7 in the fight balance: -177 margin.
- (f00-07) Teammates' wards are invisible: placing on their tile throws error 117; tiles are split by champion type
  ((x + 5y) mod 13). Placing on enemy spawn/walls throws error 64.
- (f00-07) Outcomes are chaotic: removing debug prints (fuel) flipped several games vs e07-07; 16-game A/B tests are
  mostly noise, use 40+ games.

## Known weaknesses
- Side-dependent early fights on small maps (00packs, 01hooks, 01elk-arena side B vs e07-07/e06-04): one lost
  first fight snowballs into 0-1000.
- Farming throughput on large maps (01fracture) is about half of e07-07's; gold income about 60% of theirs.
- 00rushbait-v2 side B vs starter: low-HP champions next to the central spawns keep restarting a recall that never
  completes; 01fracture B vs starter is close.
