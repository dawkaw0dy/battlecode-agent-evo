## Strategy (≤10 lines)
Scaling economy from f01-07 … f06-05 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem; f05-03's endgame (altars, locks, banking).
This generation (parameter tweaks, combat): the fight simulation counts only allies that can join within 3 turns
(was: every ally within 7 of us or 8 of an enemy, however far from reach). Allies 4–6 turns out were usually farming
elsewhere and never came, so a lone champion took fights it could not win.

## Key parameters
- Fight: kSimTurns 10, kEngageBias −150, kStick 0, value of a death 150 + 2·gems, damage term 60·(their lost − our
  lost health fraction); engage needs value > bias and not (we die and they don't). kAllyDelayMax 3 (new): allies
  whose delay (turns to reach range of the nearest enemy) is above 3 are left out of the simulation.
- New named constants, parent values: kKillNow 0.85 (take a burst kill), kRecallDist 7 (recall from a retreat),
  kDieNeed off (−1e9), kReachPad 3 / kFleeHp 0.5 (ignore far enemies while healthy), kHuntPad 3, kHelpHp 0.6.
- Chase: kChaseTurns 8, then no chase for 15 rounds. Retreat: kFreeShots on; kAllyPull 0; kCaughtFights/kDoomedFights off.

## Changes this generation
- Baseline for free: the parent's r06-B games in evo-results reproduce exactly here (checked 4/4). Test set: the 24 local
  maps of the r06-B panel, both sides (48 games), unselected (losses-only sets regress to the mean under any change).
  Parent: vs f05-02 25/48 (+58), vs f04-05 29/48 (+139), vs f03-05 34/48 (+296).
- Trace (01gold-rush A vs f05-02, lost 33–1053, 41 deaths to 2): a hurt Vesper Vale engaged a 1v1 counting on two
  allies with delay 4–6; they walked away farming and it died. Fights at value ≈ 0 (1-for-1 trades) flip-flop E1/E0.
- Kept, kAllyDelayMax 3: vs f05-02 29/48 (+131), vs f04-05 36/48 (+300). 65/96 vs parent 54/96.
- Retreat swing: check vision::has_champion() before attacking (one caught "warn: swing" engine error 98 in 136 games).
- Final build (benchmark.py): vs f06-05 (parent) 25/48 (+84), vs f03-05 35/48 (+242), vs starter 16/16, vs f00-03 8/8,
  f01-07 7/8, f02-01 7/8. 0 faults, 0 `bot error:` and 0 `warn:` lines from our side.
- Game budget: 866 games played.

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

## Known weaknesses
- Most deaths still come in retreats: same-speed melee chasers hit a fleeing champion every turn; fighting back
  (kDoomedFights, kCaughtFights) measured worse, so the fix has to be engaging less or earlier exits, not trading.
- Fights at simulated value ≈ 0 (one-for-one trades) flip between engage and retreat turn to turn; hysteresis on
  that value measured worse (parent notes).
- Snowball maps (01gold-rush, 00swirl, 01hooks, 01starfish, 00contest, 00gemfight, 00stampede) are decided by the first fights.
- Our own tribute channel is never cancelled (turn() returns early while channelling).
- Lock memory is per champion (shared memory is full: 5 claim bytes + 3 sighting bytes).
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
