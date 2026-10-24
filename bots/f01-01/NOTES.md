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
- Picks: Vendetta, Marcella, Vesper Vale, Tavi, Varg (dash maps, where <60% of camp value is walkable:
  Vendetta, Vesper Vale, Heart of Ruin, Ren, Marcella).
- Target score: (gems + 0.35·gold) × factor / (eta + wait + clear + 12); factor 1.0 own sector, 0.7 unassigned,
  0.45 another champion's sector, ×0.7 enemy side, ×1.3 reward waiting, ×1.3 current target, ×0.3 near a
  reported enemy pack. Re-chosen every 3 rounds.
- Multi-trip: allowed if ≥6 turns of fighting per trip and ≤6 trips; each trip costs 10 + distance from spawn.
- Retreat when hp < 20% or hp < incoming·(steps out of range + 2) + 40; heal first when hp < 50% and the fight
  would not finish; wait at most 2 turns on spawn to heal.
- Tribute: ≥40 gems, ≥15 within 10 tiles, ≤4 tiles away, ≥10 after a recent enemy report, or end of game;
  allowed under camp fire if we survive ceil(gems/10)+1 turns of it.
- Shop trip when gold ≥ 250 between fights; build jungle(str) → power(hp) → monster(str) → power(str) →
  durability(hp) → alternating power shards.
- Fight: stronger (hp·(str+0.3·focus), allies within 6 included) by 15% → spell first, then basic attack +
  reset; otherwise flee home (melee hits back first), recall once 6+ tiles clear. Enemies on their own spawn
  and enemies more than 9 walking steps away are ignored.
- Fuel: act only in the round the turn started; host BFS fields cached per group; goal sets by bitboard
  dilation; reserves 30–50k before optional work.

## Changes this generation
- New bot (immigrant, split-farm archetype) built on the starter's main loop. Local tests vs starter on the
  final version: 48/48 wins (16 + 32 games); previous version 80/80 on all 40 maps, mean 1155 rounds.

## Tried and failed (keep entries from the parent's notes; add yours)
- Linear bitboard buffers are bit-packed (not one byte per tile as the API doc says).
- Every bitboard/field query is a host call (~100+ fuel); per-tile loops (add_box with set(), is_set/get in
  loops, camp_at for every visible camp) blew the 100k budget on maps with 30-camp groups. Use exported
  local copies and whole-board shifts.
- Turns that run out of fuel resume next round on stale state: moves into newly occupied tiles and attack
  resets throw. Guard every action with "still the same round".
- Recalling to shop at 150 gold between fights slowed farming; 250 gold with the "not engaged" check is fine.
- Requiring no incoming camp damage before tributing stalled champions next to altars; tributes are not
  interrupted by damage.
- Diagonal dashes can be refused by the engine ("unreachable within movement range"): try candidates in order;
  only straight one-tile holes are treated as jumpable when planning.
- Mixing a static field and a unit-aware BFS made champions flip-flop between two tiles; after a blocked static
  step use the dynamic route for 12 rounds, and clear a camp that plugs a gap instead of detouring.
- Tavi standing on spawn did not heal for us; never wait on spawn more than two turns.
- Altar tiles occupied by camps (boss beside the only altar) must not be planned as tribute spots.
- Raising the tribute threshold (25 gems within 8) was neutral; kept the lower one for safety.
- Region weight for another champion's sector: 0.6 was clearly slower (+5% rounds on 8 maps), 0.3 slightly
  slower; 0.45 kept — the split itself pays.

## Known weaknesses
- Lone champions still lose to a grouped enemy that catches them; only basic fight/flee rules, untested
  against strong opponents.
- 01boss-battle as team B and 01corridors as team B stay weak (boss blocks the altar / walls of elk camps).
- No teamwork against big groups beyond multi-trip attrition; no wards, no altar locks of our own.
