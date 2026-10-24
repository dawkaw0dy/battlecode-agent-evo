## Strategy (≤10 lines)
Split farm (parent f00-03, defense-focused parameter tweak). The draft splits the camp groups on our side of the map
into one angular sector per champion and writes picks + group→slot owners into draft memory. Each champion farms
alone: it scores every group by value / (travel + respawn wait + clear time), prefers its own sector, and wears down
groups too strong for one trip over several trips, retreating out of camp range and recalling to heal. Gems are
tributed at the cheapest altar by walking distance (an altar known to carry an enemy lock costs 15 extra steps; locks are
remembered and shared in shared-memory bytes 6-7, and broken with basic attacks from attack range). Shared memory bytes
0-5 carry the latest enemy sighting: nearby healthy allies answer small invasions, everyone avoids reported packs, a
champion facing a stronger force (or below 45% health, whatever the team estimate says) runs home and recalls, and a
champion hit by an unseen enemy walks home instead of recalling in place.

## Key parameters
- Picks: Vendetta, Marcella, Vesper Vale, Tavi, Varg (dash maps, where <60% of camp value is walkable:
  Vendetta, Vesper Vale, Heart of Ruin, Ren, Marcella).
- Target score: (gems + 0.35·gold) × factor / (eta + wait + clear + 12); factor 1.0 own sector, 0.7 unassigned,
  0.45 another champion's sector, ×0.7 enemy side, ×1.3 reward waiting, ×1.3 current target, ×0.3 near a
  reported enemy pack. Re-chosen every 3 rounds.
- Multi-trip: allowed if ≥6 turns of fighting per trip and ≤6 trips; each trip costs 10 + distance from spawn.
- Retreat from camps when hp < 20% or hp < incoming·(steps out of range + 2) + 40; heal first when hp < 50% and
  the fight would not finish; wait at most 2 turns on spawn to heal.
- Tribute: ≥30 gems, ≥15 within 10 tiles, ≤4 tiles away, ≥10 after a recent enemy report, or end of game;
  allowed under camp fire if we survive ceil(gems/10)+1 turns of it. Altar = min(path distance + 15 if locked).
- Fight: fight when ours ≥ theirs × (1.15 + 0.01·gems carried, gems counted from 20, capped at 100); flee when
  weaker and (hp < 90%, 2+ enemies or ours < 0.8 theirs), and always when own hp < 45% (off spawn).
  Recall once 6+ tiles clear with gems carried. Hidden attacker (hp lost, no visible enemy, no awake camp in
  reach): walk home for 4 rounds.
- Shop trip when gold ≥ 250 between fights; build jungle(str) → power(hp) → monster(str) → power(str) →
  durability(hp) → alternating power shards.
- Fuel: act only in the round the turn started; host BFS fields cached per group and per altar (altars after groups).

## Changes this generation
Measured with tools/benchmark.py against f00-03 (mirror, so the starting point is 40/80) on a 12-map fight-heavy set
(00contest 00eight 00gemfight 00swirl 00split 01hooks 01corridors 01starfish 01shortcut 01boss-battle 01broken-curve
01yellow-brick-road, 24 games) and on the evidence maps against the bots that beat the parent.
- Kept: fight edge grows with carried gems (1.15 + 0.01·gems from 20 gems): 13/24 vs 12/24, +475 gems, gems lost
  to kills 4593 → 3301.
- Kept: enemy altar locks are remembered/shared, altars chosen by path cost (+15 if locked), locks broken from
  attack range. The parent flip-flopped for 300+ rounds between two tiles in sight / out of sight of a locked altar.
  vs f00-01 on 01broken-curve: 0/2 (102 and 4 gems) → 2/2 (1147–266, 1190–303). Mirror set 12/24 (noise; f00-03
  never locks).
- Kept: flee below 45% own health even when the team estimate says we are stronger (allies' hp counted towards
  "ours" while the enemy focused one of us down to 31/1000). 0.35: 16/24, +1279, deaths 398 → 364; 0.45: 16/24,
  +6841, deaths 284; vs f00-07 01starfish 0/2 → 1/2.
- Kept: altar fields computed after the group fields (neutral: 3/12 peaceful maps either way, 16/24 fight set).
- Kept: hidden attacker → walk home for 4 rounds (champions re-started cancelled recalls in place until dead).
  Fight set 15/24 (−1) but deaths 303 → 204; 6 other fight maps 6/12 vs 5/12, deaths 184 → 127; first side-A win
  vs f00-07 on 01starfish.
- Kept: bank at 30 carried gems (was 40): 16/24 vs 15/24, +7957 vs +5786, gems lost 899 → 360, deaths 204 → 178.
- Fixed a caught exception (engine error 5 on 00lanes vs starter): after a fuel-suspended turn resumed, farm() read
  a stale position for a bitboard query; now checks live() and bounds first. live() also survives a failing query.
- Full 40 maps vs f00-03 after the first three kept changes: 42/80, +7214 gems, deaths 768 → 642, gems lost 6222 → 2771.
- Final, full 40 maps both sides: vs f00-03 46/80 (parent mirror 40/80), +12055 gems, deaths 768 → 450, gems lost
  6222 → 1899; vs starter 80/80 (parent 79/80, lost 01yellow-brick-road as B), deaths 197 → 93.
  Evidence maps: vs f00-01 01broken-curve 2/2 (parent 0/2), vs f00-07 01starfish 1/2 (parent 0/2),
  vs f00-02 01yellow-brick-road 0/2 (unchanged, a capture/pathing problem).

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
- Tribute before fighting/fleeing when an enemy appears beside an altar and we outlive the channel: 12/24 and
  −294 gems vs 13/24 and +374 without it.
- Fleeing champion that cannot step home hits back instead: 15/24, +153 vs 16/24, +1279; 0/2 vs f00-07.
- Low-health flee at 55%: 10/24, deaths 415 (vs 16/24 at 45%).
- Recall when fleeing at low health without gems (parent only recalls with gems): 10/24, deaths 410.
- Answer ally invader reports from 16 tiles (vs 12): 9/24, deaths 437.
- Steeper gem-aware fight edge (0.02·gems from 10 gems): 12/24 vs 16/24; 0/2 vs f00-07.
- Hidden-attacker retreat for 8 rounds (vs 4): 2/2 vs f00-07 on 01starfish, but mirror 12/24 + 5/12 vs 15/24 + 6/12.
- Bank at 25 gems: 14/24 vs 16/24 at 30.
- Treat an ally's report as danger for banking for 60 rounds (vs 30): 13/24 vs 16/24.
- Debug prints to stdout cost enough fuel to change games; remove them before measuring.

## Known weaknesses
- f00-07-style packs (focus fire, ambush at altars) still kill lone farmers 25–45 times a game on 01starfish.
- 01yellow-brick-road vs f00-02: champions hit the giant gold-cluster group but never capture it (pathing/camps).
- 01boss-battle as team B never tributes (monoliths beside the only altar); huge bags (900+ gems) are lost at the altar.
- Mirror results on single maps flip chaotically with any change; 24-game sets have about ±3 wins of noise.
- No wards, no altar locks of our own.
