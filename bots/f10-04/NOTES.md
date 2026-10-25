## Strategy (≤10 lines)
Scaling economy from f01-07 … f09-02 (structure unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home
regions per champion from the draft-time map plan (f09-02); groups scored by value per champion-turn; raids for
bosses; gold into shards; focus-fire fight simulation; f04-05's fuel subsystem; f05-03's endgame; f08-04's spells.
This generation (mutation: parameter tweak, focus: defense): carried gems are banked sooner and risked less.
A bag of 20 gems goes to the altar at once (was 45), and a champion carrying 20+ gems (was 40) refuses a fight in
which the simulation has it die unless the fight is worth more than twice its bag. Carried gems change hands on
death, so this cuts what the enemy collects from our deaths (gems lost ~15–25% lower in the measured runs).

## Key parameters
- endgame.cpp kBankGems 20: bag size that is banked at once (other bank triggers unchanged: d ≤ 3, 20 gems within 10,
  10 gems with enemies about, race, clock).
- bot.cpp kBagCaution 20: carrying this many gems, an engage where we die needs fight value ≥ 2·gems.
- bot.cpp kHurtHp 0.2 (parent value, now a named constant): below it, leave camp range and recall.
- Rest of the bot: parent values (kSimTurns 10, kEngageBias −150, kRecallDist 7, kFleeHp 0.5, kHelpHp 0.6, …).

## Changes this generation
- Free baselines: f09-02's games in evo-results r09 reproduce exactly (00eight B 96–1025 r1715, 00interlock A
  75–1002 r1599 replayed identically). Test set T = the 16 local r09 maps × both sides; parent on T: vs f06-05
  21/32, f08-04 16/32, f07-02 19/32 (56/96, mean +95).
- Trace (00interlock A vs f06-05): every early death came while retreating; bags of 22–74 gems (captured in one go)
  are lost to kills on 01shortcut/00eight; ~130 carried gems a game go to the enemy.
- Each change alone vs the parent on T (f06-05 + f08-04, 64 games; parent 37/64, +84):
  bank at 30 gems 38/64 (+146); bag caution 20 41/64 (+131); hurt recall 0.3 45/64 (+242, but 17/32 vs f07-02).
- Combination (hurt 0.3 + bag 20 + bank 30) on T, 96 games: 57/96 (+151) vs 56/96 (+95); deaths −10%, gems lost
  −25% vs the bank-30 run. Bank 20 on top: 56/96 (+169), gems lost a further −15%, submitted-gem margin up: kept.
- Other 24 local maps vs f08-04: that build 27/48 (+94), hurt-0.3-only 22/48 (−5).
- Head-to-head vs the parent f09-02 (benchmark.py, T, with hurt 0.3): 18/32 (+40).
- Panel check found a regression from hurt 0.3: vs f02-01 5/10 where the parent gets 10/10 (00eight, 00lanes, 01hooks,
  00gemfight, 01starfish). Final build (hurt 0.2 back): f02-01 10/10 (+902), f00-03 6/6 (hurt 0.3: 4/6), f03-05 4/6
  (hurt 0.3: 2/6; parent 3/6 per f09-01) on 00split/01starfish/01boss-battle. So the final build is the parent plus
  bank 20 and bag caution 20; its T result was not measured separately (budget).
- Sanity (hurt 0.3 build): starter 6/6, f01-07 5/6, f04-05 3/6, f05-02 3/6 (01boss-battle lost), f09-01 4/16 (parent
  1/4 on 00eight/01hooks). 0 faults, 0 `bot error:`, 0 `warn:` lines from our side in every checked run.
- Game budget: 895 games played (all tools).

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

- (f10-04) All vs the parent's free r09 games on T unless said.
- (f10-04) Badly-hurt recall at 0.3 health (was 0.2): best single change vs f06-05/f08-04 (45/64 vs 37/64) but vs
  f07-02 17/32 vs 19/32, and it loses to old bots: f02-01 5/10 vs 10/10, f00-03 4/6 vs 6/6, f03-05 2/6 vs 4/6
  (with the other two changes in both). Reverted. 0.35: 57/96 (+97); 0.4: 33/64 (+42).
- (f10-04) kFleeHp 0.65 (retreat from enemies out of reach while below 65%): 38/64 (+109) vs 37/64; not kept.
- (f10-04) Gem losses and deaths per game are far less noisy than wins on T; wins flip on ~20% of games for any
  change (chaos), so a ±4/64 difference is noise.

## Known weaknesses
- Snowball maps are still decided by the first fights; most deaths come while retreating from same-speed chasers.
- f09-01 beats us badly on 00eight, 00interlock and 01hooks (the parent too); 01boss-battle lost to f03-05/f04-05/f05-02.
- Our own tribute channel is never cancelled; lock memory is per champion; no wards or teleport crystals.
- One inherited warn path: a flee move attempted when the champion cannot act (engine error 21, caught).
