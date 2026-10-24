## Strategy (≤10 lines)
Split farm. The draft splits the camp groups on our side of the map into one angular sector per champion
(equal value, hardest sector to the strongest pick) and writes picks + group→slot owners into draft memory.
Each champion farms alone: it scores every group by value / (travel + respawn wait + clear time), prefers its
own sector, and wears down groups too strong for one trip over several trips (camps never regenerate),
retreating out of camp range and recalling to heal. Gems are tributed at the nearest reachable altar
(sooner when allies have reported enemies). Shared memory carries the latest enemy sighting: nearby healthy
allies answer small invasions, everyone avoids reported packs, and a champion facing a stronger force runs
home and recalls instead of fighting alone. Dash champions plan through one-tile holes.

## Key parameters
- Picks: Vendetta, Marcella, Vesper Vale, Tavi, Varg (dash maps: Vendetta, Vesper Vale, Heart of Ruin, Ren, Marcella).
- Target score: (gems + 0.35·gold) × factor / (eta + wait + clear + 12); factor 1.0 own sector, 0.7 unassigned,
  0.45 another champion's sector, ×0.7 enemy side, ×1.3 reward waiting, ×1.3 current target (hysteresis).
- Multi-trip: allowed if ≥6 turns of fighting per trip and ≤6 trips; each trip costs 10 + distance from spawn.
- Retreat when hp < 20% or hp < incoming·(steps out of range + 2) + 40; heal-first when hp < 50% and the fight
  would not finish.
- Tribute: ≥40 gems, or ≥15 within 10 tiles, ≤4 tiles, ≥10 with a recent enemy report, or end of game.
- Shop trip when gold ≥ 250 between fights; build jungle(str) → power(hp) → monster(str) → power(str) →
  durability(hp) → alternating power shards.
- Fuel: act only in the round the turn started; host BFS fields cached per group; reserves 30–50k.

## Changes this generation
- New bot (immigrant, split-farm archetype) built on the starter's main loop.

## Tried and failed (keep entries from the parent's notes; add yours)
- Linear bitboard buffers are bit-packed (not one byte per tile as the API doc says).
- Every bitboard/field query is a host call (~100+ fuel); loops over tiles must use exported local copies.
- Turns that run out of fuel resume next round on stale state: moves into newly occupied tiles and attack
  resets throw. Guard every action with "still the same round".
- Recalling to shop at 150 gold between fights slowed farming; 250 gold with the "not engaged" check is fine.
- Requiring no incoming camp damage before tributing stalled champions next to altars; tributes are not
  interrupted by damage.
- Diagonal dashes can be refused by the engine ("unreachable within movement range"): try candidates in order.
- Tavi standing on spawn did not heal for us; never wait on spawn more than two turns.

## Known weaknesses
- Lone champions still lose to a grouped enemy that catches them; only basic fight/flee rules.
- Maps where a boss blocks the only altar or the main gold is contested at spawn (yellow-brick-road) are weak.
- No teamwork against big groups beyond multi-trip attrition; no wards, no altar locks of our own.
