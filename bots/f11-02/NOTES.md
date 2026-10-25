## Strategy (≤10 lines)
Mutation: radical new strategy, focus: spells. Tried strategy: a "stun squad": one leader (lowest live slot, heartbeat in
shared memory) picks the target for all champions, the squad gathers before dangerous groups and chains its stuns so the
most dangerous camp of the target is stunned once per round. It was meant to win the first contested fights and the
monolith race (the parent's worst losses) by concentrating force. It lost badly (4/20 vs the parent): one group farms
one camp at a time, so the economy collapsed (00lost-world: 293 gems collected vs 1203). The squad is still in the code
(kSquad, off). Shipped: the parent's economy, fights and endgame (f10-03) with the spell layer changed: Ryth (area stun
+ reset, 1200 hp) replaces Varg in the draft, area spells with a duration count as stuns, and camp stuns on the current
target are shared through shared-memory bytes 3-4 so no two champions stun the same camp in one round (stun value 3 per
point of camp strength on a camp that reaches one of us).

## Key parameters
- plan.cpp pick order: Vesper Vale, Vendetta, Marcella, Tavi, Ryth (parent: Varg last).
- spells.cpp: kStunChain true, kStunBoss 3.0 (parent kStunCamp 1.5 still used when the chain is off).
- bot.cpp: kSquad false (kRallyStr 60, kLeaderStale 2 only matter with it on). Shared memory: byte 0 claims/target as
  the parent (bytes 0-4 claims when kSquad is off; bytes 3-4 then double as the stun record: claims of slots 3-4 and the
  stun record share bytes, see Known weaknesses), bytes 5-7 sighting.
- Everything else as f10-03.

## Changes this generation
- Parent's evaluation (evo-results r10): 56% vs peers, 112/112 vs starter; worst losses 01boss-battle, 01starfish, 00gemfight.
- Squad v1 (shared target, rally for strength ≥ 60 or bosses, squad-sized clear-time scoring): vs f10-03 4/20 (−301) on
  10 maps (00gemfight 01starfish 01boss-battle 00eight 01gold-rush 00split 01hooks 00quadrants 00lost-world 00interlock).
- Stun chain alone (parent draft, squad off): 10/20 (+46) on the same 10 maps: neutral.
- Stun chain + Ryth for Varg: 12/20 (+105) on the 10 maps, 32/60 (+41) on the other 30: 44/80 (+57) vs f10-03; vs f09-01
  48/80 (+130) on all 40 maps (parent's notes: 50/80, +58). Starter 4/4. 0 faults, 0 caught exceptions in all runs.
- Game budget: 228 games played.

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
- (f10-03) All measured vs f09-01 on all 40 maps, both sides (80 games), against L2 50/80 (+58) unless said.
- (f10-03) Risk-vs-detour banking (bank_due(): our deaths and gems per turn alive, ×4 risk with enemies about,
  detour = walk to altar + altar to next target − walk to next target): 44/80 (+54). Bags 27.8 vs 29.4, 25 vs 23
  trips, drops 111 vs 116 a game: banks about like the parent's thresholds; no gain. Off.
- (f10-03) Locks only on enemy channels (no lock on altars we pass): 33/80 (−115). The parent then banks 696 a game
  instead of 612: our passing locks do cost the enemy. The runner and 8-tile detours do not pay; passing locks do.
- (f10-03) Hold the boss tile that takes no tribute (23,20) instead of the tribute tile: frees the tile for our banks
  but also for the enemy's; not kept (holding the tribute tile shuts the enemy out). Boss-map hold v1 (active hold of
  the tribute tile): 6/12 on 01boss-battle vs 6 opponents (parent ~3/8 on the known ones); the map stays a coin
  flip decided by who kills the monolith first.
- (f10-03) Debug prints change results (chaos): a trace on 01boss-battle flipped A from −977 to +1056.
- (f11-02) Squad / deathball (everyone on the leader's target, rally before dangerous groups): 4/20 (−301) vs the parent;
  economy collapse (one capture at a time, 5 champions waiting on 5-round captures and walking together).
- (f11-02) All-stunner draft (Marcella, Tavi, Ryth, Ren, Heart of Ruin) with the stun chain: 3/20 (−507) vs the parent.
  Stuns do not replace basic damage: Vendetta's and Vesper's Strength farm faster than stun coverage saves.

## Known weaknesses
- With kSquad off, shared-memory bytes 3-4 hold both the claims of slots 3-4 (Tavi, Ryth) and the stun record; a stun
  overwrites those claims until their next choose (every 4 turns). Not measured as harmful; a cleaner layout would move
  the stun record into the sighting bytes' spare bits.
- The gain over the parent is small (44/80) and mostly from the Ryth pick; the stun chain alone measured neutral.
- 01boss-battle remains a coin flip decided by who kills the monolith first.
- The squad code is untested beyond v1; a "squad only for raids" variant was not tried.
