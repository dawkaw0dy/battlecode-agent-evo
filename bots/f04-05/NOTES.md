## Strategy (≤10 lines)
Scaling economy from f01-07/f02-02/f03-04 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions per
champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights (f03-04's combat subsystem).
This generation rewrote the fuel subsystem (fuel.h / fuel.cpp, same calls from the rest of the bot): engine bitboards
and distance fields are read in place (8×8 chunk layout) instead of exported or queried per tile, the vision snapshot
is refilled with *_into calls, camp-group state is one host query, ally positions are read without exceptions, fields
stay in engine storage, and every step is routed with the (now cheap) host search. Mean fuel per turn 44k → 19–29k,
capped turns 1–1.5% → 0. On top: altar locks — break enemy locks by tributing at them (the parent's attack() on a
lock always threw), and f03-05's lock layer (carry a lock, cancel enemy tributes, lock altars we pass, lock runner).

## Key parameters
- Raw layout: bitboard word = (y>>3)·chunk_stride + (x>>3), bit (y&7)·8+(x&7); field value index
  ((y>>3)·chunked_width + (x>>3))·64 + (y&7)·8 + (x&7). Both checked against host reads (0 mismatches).
- Measured fuel: host call 35–80, new bitboard 340–530, in-place |= 70–190, camp_group::state() ~4,000, thrown
  exception ~3,100, map::camp_groups() 27–38k, host BFS ~3/tile, field export ~2.3/tile, memcpy ~1/byte,
  local loop 10–50 per element (a local BFS on 100×100 costs >500k: never).
- go(): fuel(25000) guard kept; scratch boards allocated once; andnot_into(a, b, out) replaces a & ~b (alias-safe).
- walk_static off (kWalkStatic = false): it existed to save the route search's fuel.
- Locks (from f03-05): kLockRange 2, kLockDetour 8, runner = last pick (detour 60, before round 1600, no gems,
  hp > 60%); our lock memory 150 rounds; enemy lock memory 200 rounds; break a lock within 4 tiles, or the nearest
  one when every usable altar is locked.

## Changes this generation
- Fuel subsystem rewritten (vision snapshot, map analysis/initialise, observe, camp-block board, group fields, eta,
  route search, dash, home/altar fields). Profile (01boss-battle, 1000 rounds, per champion): observe 15k → 3–5k
  per turn (the parent's view() threw ~3 exceptions per refresh reading allies out of sight), route search 10k → ~5k.
  Whole team: mean 44k → 27k (01boss-battle), 40k → 29k (01big-world: 0 capped turns, f03-05 113), 25k on
  01yellow-brick-road over 2000 rounds (0 capped, p99 69k; f03-05 112 capped). clear_cost() as bit masks (a 20-camp
  group cost ~20k per evaluation, now ~5k); camp tiles' raw words precomputed for observe and the camp-block board;
  initialisation 300k → ~225k on 80-camp maps (reserved vectors, single-pass threat masks).
- Fixed a parent crash path: turn() read k_[plug_] right after setting plug_ = -1 (boss beside the only altar);
  undefined behaviour the parent survived by luck, a memory fault (and restart loop) with the new layout.
- Lock breaking + f03-05 lock layer. Worst maps vs f03-05 (01boss-battle, 00stampede, 00collision, both sides):
  parent 1/6 (−774), fuel rewrite alone 2/6 (−228), + lock breaking 5/6 (+410), final 5/6 (+565).
- Full runs (benchmark.py, 40 maps × 2 sides, 0 faults, 0 caught exceptions in every run):
  vs f03-05 54/80 (+165) with lock breaking only, 59/80 (+300) with the lock layer;
  vs f02-01 67/80 (+546; parent 58/80, +206); vs parent f03-04 38/80 (−22) before the lock layer, 52/80 (+262) after.
- 10-map sets × 2 sides: dev set (00ambush 00collision 00contest 00gemfight 00lanes 00swirl 01boss-battle 01hooks
  01starfish 01yellow-brick-road): vs f00-03 18/20, f01-07 15/20, f02-01 13/20 (parent 12/20), f03-05 14/20,
  starter 20/20; second set (00corners 00eight 00elkring 00lost-world 00packs 00road-of-riches 00rows 01corridors
  01gold-rush 01the-farm): vs f01-07 20/20, f00-03 18/20.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
  (This generation: read the field in place instead; no export needed.)
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: debug prints (fuel) alone moved 00lost-world B from 1069 to 524 rounds. Judge changes on
  full 80-game runs, never on single maps; rounds-to-win vs starter is a less noisy farming-speed metric.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.
- Hysteresis again, on the simulated fight value (±60 in favour of the last decision): 14/32 vs 17/32 without.
- Engage bias −300 (fight almost anything): 16/32 vs 23/32 at −150. Bias 0 or +60 (cautious): 16–17/32.
- Trading blows when the estimated damage of fleeing exceeds our health: most deaths in fights came from it.
- Retreat scoring without camp damage: champions stood under elk fire choosing between two equally "safe" tiles.
- Allies counted only within 7 of us: one step changed the count, so a champion flip-flopped engage/retreat
  beside a camp until it died. Count allies near the enemies too.
- A local C++ BFS instead of the host's: >500k fuel on 100×100 (the host's costs ~30k). Never BFS locally.
- memcpy of engine buffers (~1 fuel per byte) and distance_field export (~2.3 per tile): read in place instead.
- Re-choosing the target every 2 turns instead of 4 and checking reward groups every turn (affordable now):
  rounds-to-win vs starter 1072 vs 1066–1084, no measurable gain; not kept.
- 00gemfight is lost vs the parent in the mirror by ~1000 both sides with the rewrite: the first contested fight
  (round 35–95) decides it and our champions now start 2–3 rounds earlier; not a fuel bug (same result with and
  without every later change).

## Known weaknesses
- Snowball maps (00swirl, 01hooks, 01starfish, 01yellow-brick-road, 00gemfight) are still decided by the first
  fights; 40–60 deaths in lost games. Fights are per champion, no shared engage call.
- Initialisation still costs ~225k on 80-camp maps and ~300k on 100×100 maps (camp_groups() ~36k, analyse_map's
  allocations and two BFS, per-group boards): the first turn spills into round 1, so champions act from round 2
  (the parent from round 4–5). Passing the analysis through draft memory would save another ~100k.
- Host reads are not slower than reads in place for a single tile (~30 fuel each; a bounds-checked raw read costs
  as much): raw reads pay off only where they replace exports, copies, allocations or exceptions.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
