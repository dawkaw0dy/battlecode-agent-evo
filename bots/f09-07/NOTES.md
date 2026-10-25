## Strategy (≤10 lines)
Split farm (immigrant, built on the starter's main loop only). The draft picks Vendetta, Vesper Vale, Marcella, Tavi,
Varg (on maps where < 60% of camp value is walkable: Vendetta, Vesper Vale, Ren, Heart of Ruin, Marcella), splits our
side's camp groups into equal-value angular sectors around our spawn (hardest sector to the first pick; on maps of
≤ 1300 tiles with 4–5 champions, half as many sectors so champions farm in pairs) and writes picks, owners, a
dash-map flag and the region count to draft memory. Each champion scores groups by value per turn (gems + 0.35·gold
over walking distance from a host BFS + respawn wait + simulated clear time + heal trips + 0.4·distance from spawn),
prefers its own region, claims its target in shared memory, clears alone, retreats out of camp range and recalls,
captures, and tributes at the nearest reachable altar (breaking enemy locks with basic hits). Fights: hp·dps ratio of
nearby allies vs enemies; flee home and recall when weaker; fled-from ground is avoided for 40 rounds. Only invaders
of our own region are answered (shared-memory sighting).

## Key parameters
- Value w(gold) 0.35 (0.1 after round 1400); own region ×1.0 (middle 0.8), other slot 0.7, middle 0.6, enemy side
  0.25/0.45 (enemies seen in the last 150 rounds or not), current target ×1.25 (×1.5 after 8 rounds), claimed by a
  non-partner ×0.3 (partner ×1.2), fled-from ground ×0.6 for 40 rounds, reward waiting only if seen ≤ 10 rounds ago.
- Multi-trip: ≤ 3 trips (enemies seen within 100 rounds) or ≤ 6; each trip costs home_dist + 10.
- Retreat from camps at hp < 22% or hp < 3·(camp + enemy dps) + 8%; recall at < 35% when out of range.
- Fight need 1.25 + gems/100 (+0.5 under 50% hp, ×0.6 on our spawn, ×1.6 while fleeing); flee when the nearest
  enemy is within max(6, its range + 4).
- Tribute: ≥ 25 gems, ≥ 10 within 10 tiles, ≥ 6 after enemies were seen (80 rounds) or reported (20), or a long
  respawn wait; a tribute walk without progress for 14 rounds bans that altar for 150 rounds.
- Shop recall at ≥ 180 gold between fights (Tavi buys anywhere); build jungle(str)×5 → monster(str)×5 (Varg:
  durability(hp)) → power(hp)×5 → alternating power str/hp; Vesper starts with treasure(str)×5.
- Fuel: staged start (turn 1 static data, turn 2 home field, turn 3 enemy field); one host BFS per move; target
  choice every 5 rounds with a cheap upper-bound prune; packed bit exports instead of per-tile host calls.

## Changes this generation
- New bot (immigrant, split-farm archetype). Dev set: 12 maps × 2 sides (00swirl 01big-world 00lanes 01hooks
  00ambush 01starfish 00quadrants 01boss-battle 00corners 01gold-rush 00split 01islands).
- Final version (benchmark.py, 24 games each): vs f05-02 5/24 (mean −314), vs f00-03 9/24 (−114); vs f08-04
  3/24 (−377, previous commit: shop at 250 gold); unseen 14 maps vs f05-02 6/28 (−388); vs starter 16/16 (+776).
  First working version: vs f08-04 0/16 (−859), vs f00-03 7/16, vs f05-02 1/24 (−809).
- What moved results most (vs f05-02 mean margin): breaking enemy altar locks instead of skipping the altar
  (−759 → −624); banking/shopping during long respawn waits (−624 → −500); milder danger memory (−500 → −432);
  pairs on small maps (−432 → −375); thin-hole dash jumps on island maps (01islands −935 → +40); nearest-altar
  tributes (home-altar detours walked past closer altars); skipping unreachable/stuck targets.
- 0 engine faults and 0 `bot error:` lines in all runs (checked 200 replays' stdout). ~845 games played.

## Tried and failed (keep entries from the parent's notes; add yours)
- (task list, from f07-02:) host calls ~30–40 fuel, bitboard temporaries ~600, local loops ~50 per element; turns over
  100k resume next round; enemy memory + hysteresis worse; unknown groups blocking routes stall champions; ranged
  stall on range-3 tiles without sight (shrink the approach radius); debug prints distort fuel; judge on full runs.
- Per-tile unpacking of exported bitboards (loop over w·h) cost ~200k fuel per turn: every turn was cut and resumed;
  use packed bits (one export, bit tests) instead. map::camp_groups() ~25k fuel: load once.
- Hole-passable routing for every dash champion on normal maps: big-world −99 → −631 (waiting at holes); only on
  maps the draft flags, and only one-tile ("thin") holes count as crossable.
- k-champion damage model (1/k² damage for shared clears) for all big groups: 0/24 vs f00-03 (−606); boss raids
  gated on a partner's claim, waiting outside boss range: −224 → −344 vs f00-03. No raids in the final bot.
- Danger memory ×0.25 for 120 rounds: −339 vs f00-03; off: −300; ×0.6 for 40 rounds: −133.
- Pairs up to 1700 tiles: −392 → −444 on medium maps; one region for all 3 champions on small 3-champion maps: worse.
- Lower tribute thresholds (20 / 8 within 12 / 4 under threat): neutral (−635 vs −624).
- Gold weight 0.3/0.15/0.05 instead of 0.5/0.3/0.1: lanes worse; 0.35 flat kept.

## Known weaknesses
- Lineage bots out-farm us ~2× in monster damage; champions still walk ~50% of their turns on 100×100 maps.
- No boss raids: 01boss-battle's only altar is reachable only from a monolith's tile (we bank nothing as side A),
  and monolith maps (01gold-rush, 00monolith, 00contest) are lost to teams that kill the boss.
- Snowball fights on small maps (01hooks, 00swirl, 01starfish) still flip whole games; deaths with carried gems.
- No wards, no own altar locks, no teleport crystals.
