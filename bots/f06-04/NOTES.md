## Strategy (≤10 lines)
Scaling economy from f01-07 … f05-03 (parent A, unchanged outside the opening): draft Vesper Vale, Vendetta, Marcella,
Tavi, Varg; home regions per champion; groups scored by value per champion-turn with a camp damage model; raids for
bosses; gold into shards; focus-fire fight simulation; f04-05's fuel subsystem; f05-03's endgame (altars, locks, banking).
New this generation, the opening: the draft turn also plans every slot's first camp group and the team that clears it
(plan_opening in plan.cpp, written to draft memory bytes 256-266). Greedy over groups x team size 1-3, strongest free
slots first, the smallest team that clears the group from full health without a trip home, rate = value / n /
(walk + clear + capture + bank walk / n). Only groups clearly on our side (walk from us at least 3 shorter than from
the enemy) are planned; slots whose best plan rates below half the first plan's are left free. Champions follow
the plan (target, raid size, rally for teams) until the group is cleared, blocked or round 150, then choose() as before.

## Key parameters
- kOpenTeam 3 (largest planned team), kOpenCut 0.5 (share of the first plan's rate a later slot needs), contested =
  d_ally - d_enemy > -3 (not planned), kOpeningRounds 150 (plan dropped after this round).
- Planning stats per pick (plan.cpp base_of): Vesper 1000 hp / 62 dps, Vendetta 1000 / 98, Marcella 1200 / 66,
  Tavi 900 / 70, Varg 1250 / 50; value = gems + 0.35 gold; bank walk = gems / 45 x min(group→altar, 6 + spawn→altar).
- Draft memory: 'E' magic, size, ngroups, home slot per group (bytes 0-253, as before); 'O' at 256, first group per
  slot at 257-261 (255 = free), team size per slot at 262-266.

## Changes this generation
Crossover note: the focus area was identical in both parents (draft.cpp, plan.cpp, choose(), farm(), the draft memory
layout are byte-for-byte the same in f05-03 and f05-02); B's edge over A comes from its spells rewrite, outside the
focus, not ported. The opening plan idea follows f05-01 (gen 5), re-fitted to A's code and then changed (cut, contested).
All runs bench2.py, release, 40 maps x both sides unless noted; 0 faults, 0 `bot error:` and 0 `warn:` lines of ours.
| version | vs f05-02 (B) | vs f05-03 (A) | gems collected by r200 (us/them, vs B; vs A) |
|---|---|---|---|
| start (= A) | 34/80, -50 | 40/80 (mirror) | 47/52 |
| v1: opening plan | 39/80, +10 | 50/80, +120 | 49/45; 52/43 |
| v2: + cut at 0.5 of the first plan | 40/80, +21 | 43/80, +35 | 55/45; 56/44 |
| v3: + contested groups not planned (submitted) | 42/80, +30 | 47/80, +92 | 52/45; 53/47 |
- v2 vs f03-05 52/80 (+208), vs f04-05 42/80 (+42). v3 vs f04-05 44/80 (+84) (parent's notes: 43/80).
- v3 on the 10-map dev set x 2 sides (00ambush 00collision 00contest 00gemfight 00lanes 00swirl 01boss-battle
  01hooks 01starfish 01yellow-brick-road): starter 20/20, f00-03 18/20, f01-07 18/20, f02-01 17/20.
- v1 → v2: the cut changes 15 maps; on those, gems by round 200 52 → 62 (vs A) and 49 → 64 (vs B), outcomes noise.
  Trace that motivated it: 01big-world, the weakest pair planned onto a 15-gem group 76 steps away.
- v2 → v3: 01starfish (evidence map): the weakest trio was planned onto the contested centre (4 elk, equal walks);
  gems by r200 were 8/24, 16/52 vs the parents (A itself: 58/39). v3: 24/24, 32/20, 24/8, 20/12.
- Game budget: 898 games played.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
  (f04-05: read the field in place instead; no export needed.)
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
- Decision-identical fuel refinements (f04-05, reverted): only shift fuel thresholds and opening timing, which
  reshuffles chaotic games: vs f03-05 45/80 (+135) against 59/80 (+300) before.
- 00gemfight is lost vs the parent in the mirror by ~1000 both sides: the first contested fight decides it.
- Host reads are not slower than reads in place for a single tile (~30 fuel each).
- (f05-03) Choosing the bank altar by per-altar cost (Chebyshev lower bound until a per-altar field is filled, +8 for
  an enemy lock, +6 where enemies were seen): slower vs starter (1145 vs 1125 mean rounds); the union goal with the
  host route search is better.
- (f05-03) "No progress for 12–20 turns → give that altar up for 40 rounds": thrashed between altars when the road
  bends away (Chebyshev distance does not drop on detours) and on single-tile altars: 1155 vs 1125 rounds. Removed.
- (f05-03) Every-turn endgame init on the first turns (enemy-spawn field + altar rebuild): +25% fuel-exhausted turns
  (rounds 3–4). Deferred to round 6 with ≥ 85k fuel and the unused field dropped.
- (f05-03) Locks are not a win condition: f00-03/f01-07/f02-01 break them with attack(altar_lock_at(a)) (it does
  not throw for them), f03-05/f04-05 by tributing at the altar. On 01hooks (one altar) our lock held 85→347,
  then they broke and re-broke it every 100–250 rounds. Only the starter never breaks one.
- (f05-03) Contest with "kill before the channel ends": channels seen are mostly 0–4 turns on 500–2000 hp, so it
  rarely fires; it fires on the big ones (272 gems, 25 turns on 01boss-battle). Kept; no measured effect.
- (f06-04) Opening plan without a cut (every slot planned): long walks for the leftovers (01big-world: 19 gems by r100
  against 56 for the parent); with the cut, gems by r200 rose on the 15 maps it changes. Kept the cut.
- (f06-04) Opening plan onto contested groups with a mild discount (x0.9, x0.95 for teams): weak trios walked into
  the enemy at the centre of 01starfish. Contested groups are now left out of the plan.
- (f06-04) Measuring with debug prints: one std::cout per turn turned 00stampede B from 0-1120 into 960-0. Use
  --rounds 150-200 single games for traces and judge only on clean 80-game runs; compare runs on the maps whose
  plan changed (unchanged maps replay identically).

## Known weaknesses
- Snowball maps (00swirl, 01hooks, 00contest, 00gemfight, 01yellow-brick-road) are still decided by the first fights;
  the plan avoids contested groups but does not help win the fights there.
- The plan never sends 4-5 champions (golems, monoliths) in the opening: 00split's 64-gem golem group goes to
  whoever rallies first (f05-02 often has it by round 100).
- The first turn still spills into round 2-3 on many-camp maps (initialisation ~130-170k); the group table is not
  cached in draft memory (f05-01 found that a timing shift with no measured gain).
- Our own tribute channel is never cancelled; lock memory is per champion; ally positions outside vision unknown.
- No wards or teleport crystals.
