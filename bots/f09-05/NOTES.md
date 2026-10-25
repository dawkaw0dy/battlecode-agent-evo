## Strategy (≤10 lines)
Scaling economy from f01-07 … f08-05 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights (allies within 3 turns); f04-05's fuel subsystem; f05-03's endgame;
f08-05's dash reserve. This generation (parameter tweak, camps): the last pick no longer runs altar locks to far
altars (kLockRunner off). It farms camps like the others, so all 3–5 champions farm. Every champion still carries a
lock, drops it on enemy tributes and on altars within kLockDetour 8 of its way.

## Key parameters
- kLockRunner false (constexpr in bot.cpp; was true): no lock runner, one more farming champion.
- Camp choice (unchanged): gold weight 0.35 (Vesper Vale 0.5) × max(0.4, 1 − round/1500); home factors 1 / 0.8
  (shared) / 0.6 (an ally's home); ×0.7 for groups nearer the enemy spawn (d_enemy + 3 < d_ally); ×1.3 reward;
  ×1.25 current target; ×0.5 near a sighting (≤ 24 rounds, ≤ 7 tiles); kCaptureTurns 6, kTripTurns 8,
  kShareTurns 15; solo when cap ≥ 5·peak and ≤ 4 trips; raids when cap·n ≥ 3·peak and ≤ 3 trips.
- Fight, spells, endgame: parent values (kSimTurns 10, kEngageBias −150, kAllyDelayMax 3, kDashReserve 10, …).

## Changes this generation
- Baseline for free: the copied source has the parent's digest, so its evo/run2 cache rows (40 local maps, both
  sides, vs f05-02, f06-05, f08-04, starter; 16 maps vs f07-02) are the parent's local results. Screen set S =
  f08-04 on the even-indexed half of the sorted map list + f06-05 on the odd half (80 games, parent 39/80, −45);
  confirmation set C = the other halves (parent 47/80, +150).
- Diagnosis (debug counters, 3 games): the lock runner (last pick) spent ~45% of its turns in the bank/lock mode
  (00expanse Varg 468/1000, 01the-farm Marcella 436/1000). Champions also spend 20–50% of turns travelling and
  30–45% dead in the f08-04 mirror.
- Kept, kLockRunner off: S 46/80 (+129) vs 39/80 (−45); C 54/80 (+210) vs 47/80 (+150). Per opponent:
  f08-04 48/80 vs 41/80, f06-05 52/80 vs 45/80. But f05-02 47/80 (+145) vs 55/80 (+231), f07-02 16/32 vs 19/32
  (those bots may break locks more slowly, so the runner's locks paid off there). Net +3 over 272 games; kept
  because it wins against the strongest and newest panel bots, and games against the starter end sooner.
- Final build vs panel (8 maps × 2 sides: 00split 01starfish 01boss-battle 00quadrants 00collision 00expanse
  01the-farm 00lanes): starter 16/16, f00-03 15/16, f01-07 16/16, f02-01 15/16, f03-05 11/16, f04-05 10/16.
  0 faults and 0 `bot error:` in all games.
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
- (f08-05) All measured against the kept dash reserve (v1) on 20 maps (odd half of the sorted list) × 2 sides
  vs f07-02 + f05-02 = 80 games; v1 there: 49/80 (f05-02 29/40 +284, f07-02 20/40 −0).
- (f08-05) kDashReserve 20 instead of 10: 44/80 (+55, +19). Not kept.
- (f08-05) No damage/stun spells on camps for 10 rounds after an enemy sighting (save Tavi's stun and Marcella's
  spell for the fight): 47/80 (+134, +41). Not kept.
- (f08-05) Walk-excluding camp spells only once a camp is in basic range (casting from range 4 costs the approach
  step): 43/80 (+282, −159). Not kept.
- (f08-05) No dash during chases either (pure reserve while enemies are near): 40/80 (+194, −154). The chase dash matters.
- (f08-05) Varg's camp shield at any health (was below 90%): 49/80 on the odd half, but 24/40 (+16) vs v1's
  29/40 (+284) vs f07-02 on the even half. Not kept.
- (f08-05) Marcella's stunning reset before her damage spell when an unstunned enemy is in basic range: 50/80
  (+222, +45) vs 49/80 (+283, +34) with the shield change. Neutral; not kept.
- (f09-05) Gold weight 0.5 (Vesper Vale 0.65) instead of 0.35 (0.5): S 38/80 (+18) vs 39/80; C 41/80 (+20) vs
  47/80 (+150); vs starter 1123 vs 1137 mean rounds. Not kept.
- (f09-05) Enemy-side group factor 0.5 instead of 0.7: S 40/80 (−47) vs 39/80 (−45). Neutral; not kept.
- (f09-05) Softer home factors (shared 0.9, an ally's home 0.8): S 43/80 (+50) but C 43/80 (+61) vs 47/80 (+150):
  86/160 both. Not kept.
- (f09-05) kLockDetour 3 instead of 8, on top of the kept change: S 45/80 (+92) vs 46/80 (+129). Not kept.
- (f09-05) About 40% of games flip winner when any parameter changes (17 of 40 for the gold weight). 80 games
  only detect effects of 5+ wins.

## Known weaknesses
- Gains are small against chaos: most single changes flip ~30–40% of games both ways; judge on ≥160 games.
- Without the runner, enemy altars are locked only when a champion passes within 8 tiles. This costs games vs
  f05-02 and f07-02. A runner only on maps or against opponents where locks hold would be a better rule.
- Most deaths still come in fights (16 of 18 deaths on 00collision B vs f08-04 were champion kills); retreats
  from same-speed melee chasers are the main cause.
- Travel takes 20–50% of farming turns on big maps (00expanse Vesper Vale 499/1000).
- Snowball maps (01gold-rush, 00swirl, 01hooks, 01starfish, 00contest, 00gemfight, 00stampede) are decided by the first fights.
- Our own tribute channel is never cancelled (turn() returns early while channelling).
- Lock memory is per champion (shared memory is full: 5 claim bytes + 3 sighting bytes).
- Ally positions outside our own vision are unknown; no wards or teleport crystals.
