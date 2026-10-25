## Strategy (≤10 lines)
Scaling economy from f01-07 … f09-01 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem; f08-04's spells; f05-03's altar locks.
This generation (mutation: subsystem rewrite, focus: defense): defense.cpp rewritten from scratch behind the same
calls (retreat, tribute, deny/contest, guard_channel, sense_ambush/scout/evade, bag_need, recover). New: a foe memory
(every enemy seen in the last 3 rounds, keyed by handle, with hit_turn()/foe_eta() timing from the rules), a carrier
flees to an altar only when it outlives the walk and the channel there, no banking walk to an altar an enemy guards,
a ranged champion caught by an adjacent melee chaser shoots back instead of a futile step. Kept from the parent's
ideas (reimplemented): ward reveal of hidden shooters, scouting wards, bank under fire, recall timing by distance.

## Key parameters
- defense.cpp: kMemory 3 rounds; recall when every visible foe ≥ 7 away (kRecallDist), cancel when one ≤ 5
  (kCancelDist); kEtaRecall/kLateCancel/kStunSafeStart off (measured worse, below); kSafeAltarFlee: altar only when
  channel_damage(walk + ceil(g/10) turns) < 0.8·hp and no stun reaches us in it; kAltarWatch: a foe within its reach
  + 2 of the nearest altar and channel_damage there ≥ 0.8·hp → keep the bag; kCaughtShoot (home > 3); kBagNeed 3;
  kFleeAltarGems 10; kAmbushRounds 4; scouting 40 rounds / 3 bush tiles / ward within 5.
- kRetreatStats (off) prints retreat outcome counts every 400 rounds.
- Rest of the bot: parent values (kSimTurns 10, kEngageBias −150, kAllyDelayMax 3, kLockRunner on, …).

## Changes this generation
- Parent results for free: evo-results r09 holds f09-01's games on all maps vs f08-04, f09-03, f09-05, f07-02,
  f06-05, starter (reproduced 00eight vs f09-03 exactly). Parent on local maps: f08-04 56/80 (+286), f09-03 56/80.
- Diagnosis (death trace + channel trace, 16 games vs f08-04): 240 deaths, 30% with a bag (avg 24 gems, 11% of
  collected gems lost); 108 deaths while retreating. Tributes: 415 landed, 3 broken, none died in. Recalls: 602
  landed, 74 broken, 9 died in. Retreat outcomes (8 games): 2232 steps, 242 boxed, 177 swings, 148 dashes, 67 recalls.
  00collision side B (lost by the parent vs everyone): carriers fled to the central altar, could not bank under
  the chasers' fire (bank_under_fire refused) and stood beside it until dead. Fixed by kSafeAltarFlee.
- Results vs f08-04 (80 games each): v1 eta recall gate 48 (+146); parent recall back 54 (+234); + loose recall
  (start at 6, cancel at hit next turn) 56 (+286); + safe altar flee 60 (+356); + bag need 6 58→56 (+323, reverted);
  + altar watch 58 (+345); final (recall 7/5 back, stun-safe start off) 50 (+206). vs f09-03 (altar watch build)
  56/80 (+313) vs parent 56/80 (+249). Head-to-head vs f09-01: altar-watch build 34/80 (−85), final 43/80 (+64).
  Every variant is within noise of the parent (≈ ±4.5 wins per 80 games); the final one was chosen on the rules
  (a recall started 6 from a melee chaser breaks on its last tick; refusing tributes near any stunner left
  carriers wandering with their bags).
- Panel sanity (final build): starter 8/8, f00-03 6/6, f03-05 11/12 (00split 01starfish 01boss-battle 00quadrants
  00collision 01the-farm), f04-05 1/6 on 00rows/01gold-rush/00ambush (parent 3/6 there; close games).
  0 faults, 0 `bot error:` and 0 `warn:` lines in every run.
- Game budget: 882 games played.

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
- (f10-02) Recall gated on foe memory (start only when no enemy seen in the last 3 rounds can hit us within the 5
  ticks, fuel.cpp walks home otherwise; cancel when a visible foe hits before it lands): 48/80 vs 54–56/80 with the
  parent's distance rules. Walking home instead of recalling costs far more than the broken recalls it saves.
- (f10-02) Looser recall (start when every foe ≥ 6, cancel only when one hits next turn): 56/80 vs f08-04, but a
  melee chaser starting 6 away breaks it on the last tick; reverted with the stun rule below.
- (f10-02) No tribute of 2+ ticks while a stun-capable enemy reaches us within the channel: the carrier then
  walks off with the bag; head-to-head 34/80 with it (and loose recall) vs 43/80 without.
- (f10-02) kBagNeed 6 (twice the engage margin per carried gem): 56/80 vs 60/80 at 3; gems lost on death
  −25% but no more wins.

## Known weaknesses
- Most deaths are lost fights against stronger (better-itemed) enemies: no retreat saves a champion caught by a
  same-speed melee chaser with twice its health; snowball maps (01yellow-brick-road, 01starfish) are decided early.
- 00collision side B is still lost vs f08-04 (339–1042 instead of 119–294): both teams bank at the central altar.
- 01boss-battle: bags of 500–1000 change hands through kills at the single altar tile.
- Foe memory is used only for timing; nothing is shared beyond the 3-byte sighting. Results are within noise
  of the parent; the gain, if any, is small.
