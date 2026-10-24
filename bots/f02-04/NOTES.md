## Strategy (≤10 lines)
Map-adaptive plan on top of the parent's split farm (f01-03 infrastructure: pathing, fights, shop, tribute).
The draft now analyses the map once and writes a Plan to draft memory: boss groups (monolith/golem) ordered by
value/distance, the group that blocks every altar tile (if any), a raid-squad mask per slot and a gather count.
Raiders share one boss target and a "present this/last round" mask in shared memory (report compressed to 4
bytes), gather on a ring just outside the boss's reach, engage together (or join a fight already going),
capture and tribute; while the boss respawns they farm their region. Tribute can use altar tiles under a dead
blocking group. Camp damage is only counted from awake camps for which we are the nearest champion.
Final version: raids are enabled only on maps whose altars are all blocked by camps (01boss-battle), where the
parent never tributed; every other map plays the parent's split farm (general boss raids tested badly).

## Key parameters
- Blocked altar: no free tile within submit range (1) of any altar reachable from our spawn → all champions raid
  the nearest group standing on an altar tile; raid list = blocker first, then bosses with dist_ally ≤ dist_enemy+6.
- Raider order: Marcella, Vendetta, Varg, Ryth, Heart of Ruin, Vesper, Tavi, Ren. need = clamp(raiders-1, 2, 4).
- Gather ring: Chebyshev distance range+2..range+3 from the boss; wait ≤ 40 rounds; heal first below 80% hp.
- Shared memory: [0-3] enemy report (round&255, x, y, count|slot<<4), [4] raid group+1, [5] round stamp,
  [6]/[7] raider bits for this / previous round.
- Picks unchanged from the parent (Vendetta, Marcella, Vesper Vale, Tavi, Varg; dash maps as before).

## Changes this generation
- New plan module (make_plan / write_plan / read_plan), raid_group / raid_step, nearest-aware compute_incoming,
  altar tiles under a dead blocker, cached staging ring, raid fields filled first.
- Engine facts measured: submit_gems_range = 1, capture 5, camp sleep 2, spawn heals fully per round; allied
  champions outside vision are NOT readable (error 97); a monolith hits the nearest champion (Chebyshev; a probe
  showed the target switch when the tank stepped from distance 3 to 4 behind a ranged ally at 3).
- Results (31 games total): vs f01-03 3/8 on 01boss-battle, 00contest, 00stampede, 01gold-rush; vs starter 5/6.

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
- Trading blows whenever fleeing cannot avoid hits (hp ≥ 12%): 0/24 on f00-03's best maps; the kill-reachable
  condition gave 10/24 and +6200 gems vs f00-00 on 16 maps.
- Also trading when home is farther than turns-to-die ("doomed"): worse vs both parents (fleeing champions
  often survive because the chaser gives up).
- Counting enemies seen in the last 4 rounds (out of sight) in the force ratio: worse vs both parents (flees too much).
- Keeping up a retreat out of sight at full health, together with the memory above: -3500 gems vs f00-00.
- Answering ally reports of up to 3 enemies within 15 tiles: +1700 vs f00-03 but -4500 vs f00-00 on the test
  sets; kept 2 / 12.
- No-progress rule (target alive, no move/attack for 25 rounds → skip it 150 rounds) did not fix
  01yellow-brick-road side B vs starter (reverted, untested elsewhere).
- (f02-04) Squad raids on every boss-heavy map (all 5 raid when bosses are ≥60% of the gem rate, 3 when ≥25%):
  0/6 vs f00-03 on 00contest, 01boss-battle, 00monolith. Champions engaged with base stats and no gold, the
  monolith (range 5) kept hitting whoever was nearest while they walked in and out, 22 camp deaths in one game.
  Raids would need gold/jungle emblems first, a fixed tank order and an earlier retreat; disabled except for
  blocked altars.
- (f02-04) Building the staging ring with dilate() every turn cost ~35k fuel and left step_to without fuel
  (champions stood still for 20 rounds): cache it per group.

## Known weaknesses
- 01boss-battle: we now clear the blocking monoliths and tribute a little, but the opponent then tributes its
  big carried bags at the freed altar (vs f01-03: 73-1941, 60-2291). Holding the altar tile after a capture
  (it also stops the respawn) is the obvious next step.
- 00contest (monoliths are the only gems) and other boss maps still rely on the parent's solo multi-trip.
- 01yellow-brick-road as team B vs starter: still 0 gems (capture never completes), as in the parent.
- Everything listed for the parent (lone champions lose to packs, no wards/locks) still applies.
