## Strategy (≤10 lines)
Split farm (unchanged from f00-03). The draft splits the camp groups on our side of the map into one angular
sector per champion and writes picks + group→slot owners into draft memory. Each champion farms alone: it scores
every group by value / (travel + respawn wait + clear time), prefers its own sector, wears down strong groups over
several trips, retreats out of camp range and recalls to heal. Gems are tributed at the nearest reachable altar.
Shared memory carries the latest enemy sighting. New this generation: a rewritten pathing subsystem (same
interface: walk_field / step_to / try_dash / hold_for_dash / go_home) that follows cached static fields while the
way is clear, searches a small unit-aware field to the static field's tiles 5 steps ahead when units are in the
way, remembers occupants vision misses, and falls back to whole-map unit-aware fields when it detects circling.

## Key parameters
- Pathing: detour aims K=5 static steps ahead with ≤6 extra steps (host compute_to, max_distance K+6);
  greedy way-check 5 steps ahead decides between the exact static step and the detour.
- Occupancy memory: allies every round (hidden ones in bushes included, from cambc::champions(ally)), enemies
  seen this round for 2 rounds, tiles where a move/dash was refused for 3 rounds.
- Circling check: 8 step attempts within 24 rounds on ≤2 distinct tiles → whole-map fields for 15 rounds.
  A blocked static way with no short detour: clear the plugging camp group if one champion can (no monolith,
  dps < 120), wait ≤4 turns behind an ally (only with no awake camp or enemy near), else whole-map fields 12 rounds.
- Step choice among equally short steps: outside awake camp range and >2 tiles from enemies, then orthogonal.
- One retry with the next candidate after a refused step (hidden unit); respawn waits never park on any camp tile.
- Everything else as in f00-03 (picks, target score, tribute and retreat thresholds, shop plan).

## Changes this generation
- Pathing rewritten (see Strategy). Fixes found in the parent: static/dynamic flip-flop beside walls of camps
  (01broken-curve: champions carried 20–50 gems for 1000+ rounds), allies hidden in bushes refusing steps,
  vision-dependent blockers, waiting behind enemies while fleeing, respawn waits parked on another group's
  spawn tiles (01gold-rush: three champions blocked each other's groups for 1000 rounds).
- Fuel: map size in plain globals (function statics cost a guard per call), cached own position in the view,
  row-pointer ring scan, lazy occupancy refresh; walking turn ≈13k fuel (first draft was ≈50k and lost turns).
- Local results, final version: vs starter 80/80 on all 40 maps, sum of rounds 91173 vs parent 93570;
  vs f00-03 24/32 (+310 mean gems) on the generation-0 stage-A maps, 44/80 (+76) on all 40 maps;
  vs f00-07 10/16 (+124) where f00-03 gets 9/16 (−11).

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
- (f01-02) A full-map distance field costs ~30k fuel on 100x100 (compute is charged by area); compute_to that
  stops at our tile is cheap for short routes (~2–6k). Exporting a field to a local grid costs ~23k on 100x100.
- (f01-02) A 11x11 per-tile "units near" scan and a per-tile ring scan through Grid8/Grid16::at cost 10–15k each
  (in_bounds → function statics); the first draft overflowed turns and lost 1 of 2 rounds while walking.
  Check only the greedy way ahead instead.
- (f01-02) Falling back to a whole-map unit-aware route as soon as a short detour fails walked champions round
  plugging camps and back (01the-farm +280 rounds); keep the parent's rule (clear the camp or wait).
- (f01-02) Treating visible camps as walls in handle_enemies' walking distance (enemies behind a camp line are
  no threat): 10/20 either way on 10 maps, reverted (outside the pathing focus anyway).
- (f01-02) Mirror games are fully side-determined and chaotic: any change flips ~25 of 80 head-to-head games.
  Judge changes by totals over many maps (sum of rounds vs starter, wins over 80 games), not single maps.

## Known weaknesses
- Decision-level dithering remains: farm steps into sight of an enemy, handle_enemies steps back out, repeat
  (00swirl, 01corridors, 01hooks); the circling check only switches the planner, it does not stop it.
- 01gold-rush and 01hooks are lost on both sides to f00-03 (early deaths at the centre/our altar snowball).
- 01yellow-brick-road as team B stays very slow (the 30-tile cluster group is never captured; unseen tiles count
  as living so champions crowd the lane).
- 01boss-battle: the only tribute tiles are the monolith's spawn tiles; tributes happen only by chance.
- No teleport crystals: camp groups unreachable by walking (and by straight one-hole dashes) are ignored.
