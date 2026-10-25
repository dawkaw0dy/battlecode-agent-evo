## Strategy (≤10 lines)
Scaling economy from f01-07 … f09-02 (unchanged outside pathing): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg;
home regions per champion from the map plan; groups scored by value per champion-turn; raids for bosses; gold into
shards; focus-fire fight simulation; f05-03's endgame; f08-04's spells; f09-02's map plan.
This generation (mutation: subsystem rewrite, focus: pathing): the movement code (go, step_on, try_dash, go_home,
recall_or_walk, walk_static) is rewritten from scratch in path.cpp behind the same calls. Routes come from the
host's distance fields; champions block only within 2 tiles; awake camps' reach is avoided when the detour is ≤ 4;
ties go to safe tiles with more ways on; a move refused by a hidden unit is retried once. New: dashers route over
"thin" holes (one dash crosses them) when that beats walking by more than the cooldown, and their eta fields use
the same walls (01islands' interior becomes farmable); the others buy a teleport crystal for such shortcuts.

## Key parameters
- path.cpp: kUnitRadius 2 (champions closer than this block the route), kDangerScan 10, kTraceSteps 10,
  kSafeSlack 4 (extra steps accepted to avoid awake camps), kDashFrom 3 (travel dash), kJumpMargin 1 (jump route
  must beat walking by cooldown + 1), kJumpMinWalk 4, kJumpForce 6 (jump even when the caller wants no travel
  dash), kCrystals on, kCrystalGain 6 (steps a crystal route must save), kCrystalRange 3, kJumpBan 12 (rounds
  without jump routes after a refused crossing).
- Thin hole = hole with walkable (not wall/hole/altar/enemy spawn) tiles on two opposite sides (4 axes); jump
  walls = walk walls without thin holes. Offline check against an exact dash BFS: equal on every local map
  (01islands: 8 camps off by 1 step).
- fuel.cpp: fill_fields() builds dashers' group fields on jump walls (one-line change); fuel.h declares jump_walls().
- Rest of the bot: parent values.

## Changes this generation
- Free baselines: the parent's r09 games reproduce exactly (00eight vs f08-04), so new runs compare game by game.
- Plain rewrite (no jump routes) on the 16 r09 local maps: vs f08-04 21/32 (parent 16/32), f06-05 23/32 (21),
  f07-02 18/32 (19): 62/96 vs 56/96; rounds to beat the starter 1013 vs 1022 (no farming change).
- Jump routes: the first version had dashers stand still beside holes when the caller passed dash=false (close
  to the goal); fixed (jump when walking cannot get there or goes ≥ 6 round). Rounds to beat the starter on 10
  hole maps: 1015 vs parent 1159 (01islands 773/839 vs 1046/993, 01shortcut 1007/1168 vs 1541/1534).
- Crystals: first version refused every landing (asked for 5 steps of progress; a hole crossing gives 2) and
  banned jump routes in a loop (Marcella stood at one hole ~140 rounds); fixed: 998 vs 1015 without crystals.
- Looser thresholds (kJumpMargin 2→1, kCrystalGain 10→6): 988 vs 998.
- Final build on the 16 r09 maps: vs f08-04 21/32 (+228) vs parent 16/32 (−7); vs f07-02 19/32 (+281) vs 19/32
  (+116); jump version vs f06-05 25/32 (+359) vs 21/32 (+175). Consistent flips on 01concentrated-wealth,
  01shortcut, 01starvation (the parent's worst maps). Rounds to beat the starter on the 16 maps 961 vs 1022.
- Head-to-head vs parent f09-02 (crystal version, 40 maps, both sides): 44/80, mean +50; big wins on hole maps
  (00road-of-riches, 01shortcut, 01fracture, 01concentrated-wealth, 00stampede), both sides lost on 8 maps without
  shortcuts (00collision, 00fragment, 00swirl, 01elk-arena, 01yellow-brick-road, 00eight, 01gold-rush, 00lanes).
- Older panel bots (4 maps, both sides): f00-03 8/8, f01-07 8/8, f02-01 7/8, f03-05 7/8, f04-05 4/8, f05-02 5/8,
  f09-01 2/8 (parent 2/8, −315; new −288).
- 0 faults, 0 `bot error:`; the only warn line is the inherited `flee move: engine error 21` (01yellow-brick-road).
  No turn over budget except first turns (2x fuel), as the parent.
- Game budget: 735 games played (all tools).

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
- (f06-05) Simulation horizon 10 → 15 / 20 turns (1v1s of ~1000 hp at ~78/turn do not resolve in 10): S1 vs f05-02
  19/40 (−97) and 17/40 (−69) vs 17/40 (−15). No gain.
- (f06-05) "Caught" rule: fight back when melee chasers within reach would deal sustain × min(walk home, 12) (× 0.6
  for a melee victim) ≥ our hp: 11/40 (−258). Fighting back loses more than running, as the parent's notes found.
- (f06-05) Answer allies' sightings from 20 tiles instead of 14: 17/40 (−88) vs 21/40 (−14).
- (f06-05) Retreat towards the nearest fighting ally (kAllyPull 1 per tile): 21/40 (−17), no change.
- (f06-05) kChaseTurns 12: 26/40, 21/40, 24/40 (71/120, mean margin +93) vs 8: 70/120 (+124). Equal; kept 8.
- (f06-05) Hunt enemies within attack range + 5 instead of + 3: 17/40 (+4) vs 23/40 (+109).
- (f06-05) After a dropped chase, chase again after 8 rounds instead of 15: 23/40 (+47) vs 23/40 (+109).
- (f06-05) Target choice: carried gems weighted 3 instead of 0.5 in strike(): 21/40 (+97) vs 23/40 (+109).
- (f07-03) All on top of kAllyDelayMax 3 unless said; base vs f05-02 29/48 (+131), vs f04-05 36/48 (+300).
- (f07-03) kAllyDelayMax 2: vs f05-02 28/48 (+98); 4: 23/48 (+95). 3 is best.
- (f07-03) kDieNeed 0 (engage only for a positive value when we die in the simulation), without the ally cap: vs f05-02
  26/48 (+86) vs 25/48 parent; not kept.
- (f07-03) kKillNow 1.0: 29/48 (+125), no change.
- (f07-03) kReachPad 2 (ignore enemies more than reach + 2 away): vs f05-02 31/48 (+123) but vs f04-05 29/48 (+169).
- (f07-03) kRecallDist 6 (recall from a retreat sooner): 26/48 (+39).
- (f07-03) kHelpHp 0.8 (answer sightings only when healthier): 25/48 (+76).
- (f07-03) kEngageBias −100: 27/48 (+108).
- (f07-03) kFleeHp 0.35 (keep farming while hurt if enemies are not near reach): 24/48 (+37).
- (f07-03) kHuntPad 2 (hunt only enemies within range + 2): vs f05-02 35/48 (+236) but vs f04-05 31/48 (+250):
  66/96 vs 65/96, noise; not kept. Worth a re-test with more games.
- (f08-04) All measured vs f05-02 + f04-05 on the 20-map set (80 games); kept version 56/80 (+160), parent 50/80.
- (f08-04) Without the fight-nuke focus term (kFocus 0: every target with hp ≥ damage scored alike): 53/80.
- (f08-04) Hold camp nukes/stuns for 10 rounds after seeing an enemy (kHoldRounds 10): 52/80 (+131).
- (f08-04) Hold the dash for escapes 10 rounds after seeing an enemy (kDashHoldRounds 10): 53/80 (+166).
- (f08-04) Ranged walk-reset on camps (walk into range, reset, shoot the same turn): 49/80 (+77).
- (f08-04) Retreat stuns only on chasers we cannot step away from (melee beside us or a dash away, ranged within
  range − 1) instead of any unstunned enemy within 3: 50/80 (+127).
- (f09-02) All vs f05-02/f06-05/f07-02 against the parent's own r08 games (deterministic, so free baselines).
- (f09-02) Vendetta ahead of Marcella in the frontline order: 42/60 vs 47/60.
- (f09-02) Varg ahead of Vendetta in the frontline order: 35/54 vs 36/54 (neutral, not kept).
- (f09-02) "Forward" = closeness to the enemy spawn (−d_enemy) instead of d_ally − d_enemy: wins 00collision 6/6 but
  loses 01starfish/01hooks/00swirl: 39/60 vs 42/60.
- (f09-02) No lock runner with 3 picks (runner flag only for ≥ 4): 8/20 vs 8/20 on 00collision/00fragment.
- (f09-02) Runner = least sturdy pick except Vesper Vale on every map (Tavi on 4–5 picks): 3-pick maps +4 wins, but
  01starfish −5 (5 picks): only the 3-pick case kept (the frontline holder never runs).
- (f09-02) 3-pick draft Tavi, Vendetta, Marcella (no Vesper Vale): 16/36 vs 22/36.
- (f09-02) 01boss-battle vs f04-05 is lost 0/2 by the parent too (identical with role homes off): endgame, not plan.
- (f10-05) Danger avoidance off (awake camps' reach not avoided): rounds to beat the starter 962 vs 961; vs f06-05
  23/32 (+289) vs 25/32 (+359); on the 8 maps lost both sides vs the parent 7/16 vs 0/16 (selected maps, regression
  to the mean). Neutral; kept.
- (f10-05) All visible champions as walls (parent's rule) instead of within 2 tiles: same 8 selected maps 4/16.
  (A first try with a radius of 99 scanned ~40k tiles per step and ran out of fuel: never loop a large box.)
- (f10-05) Jump routes only when the caller allows a travel dash: dashers froze beside a hole whenever the goal
  was ≤ 3 away on the jump field (Vesper Vale: 968 of 1000 turns "capture" on 01islands).
- (f10-05) A hole chain (hole → hole) never shortens a route on the local maps, so the thin-hole field needs no
  exact dash search.

## Known weaknesses
- The 8 maps lost both sides to the parent have no hole shortcuts: there the rewrite changes little but routing
  details, and the games are decided by fights (chaotic).
- Non-dashers never target camps that only a jump reaches (their eta stays on walking fields); a crystal crossing
  is used only on routes that walking also completes.
- Jump eta ignores the dash cooldown (routes with several crossings wait at each hole).
- Retreat and hold-tile steps (bot.cpp, endgame.cpp) still move on their own rules, not through path.cpp.
- 01elk-arena and f09-01 remain bad matchups (as for the parent).
