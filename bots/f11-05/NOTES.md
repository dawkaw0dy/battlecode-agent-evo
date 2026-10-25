## Strategy (≤10 lines)
f10-01 unchanged in structure (mutation: parameter tweak, focus: camps): draft Vesper Vale, Vendetta, Marcella,
Tavi, Varg; home regions per champion; groups scored by value per champion-turn with a camp damage model; raids
for bosses; gold into shards; a turn-by-turn focus-fire simulation decides fights (engage / retreat / recall);
f04-05's fuel subsystem; f08-04's spells; f09-01's defense; f10-01's fight thresholds.
This generation: one camp-farming parameter in choose() (bot.cpp). A group another champion already claims is
shared only when its solo clear would take more than 30 turns (was 15) or needs trips home; shorter clears are
left to the claimer, so champions spread over more groups instead of doubling up on medium ones (single elk).

## Key parameters
- bot.cpp choose(): kShareTurns 30 (was 15). Unchanged: gold weight 0.35 (Vesper 0.5) × max(0.4, 1 − r/1500),
  home 1 / shared 0.8 / other's home 0.6, enemy side ×0.7, reward ×1.3, current target ×1.25, sighting within 7
  (≤ 24 rounds) ×0.5, claimed short clear ×0.3 per claimer, solo needs cap ≥ 5 × peak and ≤ 4 trips, raids ≤ 3
  trips, kCaptureTurns 6, kTripTurns 8, kRallyRounds 35, kGatherTurns 10, kBossTurns 8.
- bot.cpp enemies() (f10-01): kFleeHp 0.7, kHuntPad 2, kEngageBias −150, kSimTurns 10, kAllyDelayMax 3.

## Changes this generation
- Baseline for free: evo-results r10 holds the parent's games on the 40 local maps vs starter, f07-02, f08-04,
  f09-01, f10-03; my copy reproduced them exactly (4/4 games checked). Screens:
  S1/S2 = each local map, both sides, against one of f10-03/f09-01/f08-04/f07-02 in rotation (80 games each,
  different map–opponent pairs; parent 51/80 +223 and 50/80 +223); starter speed = mean rounds to win vs starter
  on 20 maps both sides (parent 1138 on the even-indexed maps, 1100 on the odd ones).
- KEPT kShareTurns 15 → 30: S1 49/80 (+125), S2 58/80 (+281): 107/160 vs the parent's 101/160 (margin +203 vs
  +223). Starter speed: 1111 vs 1138 on the even half, but 1113 vs 1100 on the odd half: neutral overall.
  Head-to-head vs f10-01, all 40 maps both sides: 40W/40L (+8). Older panel (f00-03 … f06-05, 00ambush/01fracture/
  00gemfight, both sides): 37/42. Weak evidence: the only change that was not worse on any screen.
- 0 engine faults and 0 `bot error:` lines in all games (2 caught "warn: Champion cannot act." lines from parent
  code in 442 team-games). Game budget: 852 games played.

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
- (f09-01) Judged on half 1 (20 maps, both sides) vs f05-02 (parent 30/40, +257) unless said.
- (f09-01) Retreat score with chase margins (−1.5 per tile beyond every chaser's reach up to 3, +1000 on a tile
  where the next enemy turn kills us, ties keep moving), recall timing from the first turn a chasing enemy can hit
  (≥ 5) and recall cancelled when one can hit before it lands, recover() (hurt < 45% with an enemy seen in the last
  12 rounds: go home): 22/40 (+91). Parent recall rules back: 24/40 (+159); parent retreat score back: 27/40 (+202).
  Deaths per game unchanged (802 vs 819): the retreat geometry is not what kills us.
- (f09-01) Bag caution (kBagNeed 3) + fleeing carriers run for a nearer altar: 26/40 (+152); gems lost on death
  (01boss-battle excluded) 82 vs 87 a game. Kept (cheap), but not a measurable gain alone.
- (f09-01) Ambush detection that only subtracted incoming_: false alarms on camp hits (a camp that hit us and died
  since), and each alarm turned recalls into long walks home: 0/8 (−812) on the parent's worst maps vs f08-05.
- (f09-01) Evading an ambush by walking away from unseen bushes, without the ward: 2/8 (−510) there; the shooter
  follows. The ward reveal is what works (4/8, +49).
- (f09-01) Spawn defence: step off our spawn into a fight only at fight value > 0 (instead of > −150): vs f08-04
  24/40 (+208), f05-02 27/40 (+296) vs 28/40 (+306) and 28/40 (+300) without. Reverted.
- (f09-01) Gem losses as a metric: exclude 01boss-battle (its only altar sits on boss spawn tiles, so bags of
  400–1100 gems circulate through kills and swamp every other map).
- (f10-01) Screens on set M (64 games; parent 33/64 +30, regression to the mean inflates every variant):
- (f10-01) kBagNeed 3 → 1: 36/64 (+64). Not kept (no better than other noise-level variants).
- (f10-01) Simulation damage term 60 → 120 per health fraction lost: 33/64 (+40).
- (f10-01) kSimTurns 10 → 7: 37/64 (+140).
- (f10-01) kRecallDist 7 → 8: 39/64 (+194); combined with kFleeHp 0.6: 39/64 (+196). Not kept.
- (f10-01) kFleeHp 0.6: 43/64 (+227) on M but on set N vs f08-04 35/48 (+334) vs parent 38/48 (+425); 0.7 kept.
- (f11-05) Screens as in "Changes"; each 80-game peer screen has a noise of about ±4 wins, so most of these are
  "no evidence of a gain" rather than proven losses. Diagnosis: on 00road-of-riches about half of all captures
  are 0-gem gold-cluster groups (Vesper 9 of 11), and travel is the largest share of turns (Vesper 609/1200).
- (f11-05) Vesper gold weight 0.5 → 0.35 (farm gems, not gold clusters): S1 46/80 (+164) vs parent 51/80 (+223).
- (f11-05) Another champion's home group factor 0.6 → 0.8 (less territorial): S1 45/80 (+154).
- (f11-05) kRallyRounds 35 → 20 (shorter raid wait): S1 51/80 (+250), starter speed +7 rounds (median 0): neutral.
- (f11-05) On top of kShareTurns 30 (starter speed base 1111): kShareTurns 50: 1129; gold weight
  max(0.3, 1 − r/1000) instead of max(0.4, 1 − r/1500): 1123; claimed short clear ×0.15 instead of ×0.3: 1140.
- (f11-05) Groups near a recent enemy sighting ×0.3 instead of ×0.5: S1 43/80 (+58) vs 49/80 (+125).

## Known weaknesses
- Gains are small and measured on mirrors of the same family (every panel bot drafts the same five and shares the
  camp logic): the kept change is level with the parent head-to-head (40/80) and only +6 wins over 160 peer games.
- Lock runner: on 00road-of-riches Varg spends ~490 of 1200 turns in the bank/lock mode carrying locks (f10-03 turned
  the runner off); outside this generation's camp focus, not changed.
- Champions farm 0-gem gold-cluster groups often; no gold-weight variant tested better (here or in earlier gens).
- Ambushes are sensed only with no visible enemy within 5: a hidden shooter during a visible fight is not modelled.
- A hidden enemy is revealed only after it hits us, or by a scouting ward (only while enemies were seen lately).
- Ward vision is the owner's alone; nothing about revealed enemies is shared beyond the 3-byte sighting.
- Fleeing deaths unchanged: same-speed melee chasers still hit every turn of a retreat; ~50 deaths a game in mirrors.
- f03-05 (locks on every altar) still wins 01boss-battle and one side of 01starfish and 00split.
- 00gemfight is lost both sides vs the parent; snowball maps are decided by the first fights.
- Our own tribute channel is cancelled only when we would die in it; lock memory is per champion.
