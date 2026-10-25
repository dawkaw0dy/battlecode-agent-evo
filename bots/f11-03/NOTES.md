## Strategy (≤10 lines)
Scaling economy from f01-07 … f10-03 (unchanged outside the opening): home regions per champion; groups scored by value
per champion-turn with a camp damage model; raids for bosses; gold into shards; turn-by-turn focus-fire simulation decides
fights; f04-05's fuel subsystem; f08-04's spells; f09-01's defense; f10-03's endgame.
This generation (mutation: subsystem rewrite, focus: opening): draft.cpp/plan.cpp rewritten behind the same calls.
The draft analyses the map once (groups, camps, rewards, walking distances from both spawns, home slots) and packs it into
draft memory (13 bytes per group, 3 per camp); champions decode it at init instead of camp_groups() + analyse_map()
(~150k fuel), and a first turn that ran over ends at once. Champions now act from round 1 on most maps (the parent lost
round 1 on 26 of 40 maps, rounds 1-4 on some). Picks: Vesper Vale, Vendetta, Marcella, Tavi, then Ryth instead of Varg.

## Key parameters
- plan.cpp: pick order Vesper Vale, Vendetta, Marcella, Tavi, Ryth (3/4/5 picks take the first n).
- Draft memory: 'G', mode 1 (full groups) or 2 (home slots only when 24 + 13·groups + 3·camps > 1024; then champions run
  analyse_map() themselves), team size, route (≤ 8 steps), u16 group/camp counts.
- bot.cpp kAfterInit 50000: a first turn with less fuel left after initialise() (or one that ran into the next round) returns.
- Off (wired in, measured worse): kRouteSteps 0 (team opening route; 3 = whole team takes the same first groups, ×1000
  value, kOpenEnd 150, kDueSlack 25, kTeamDps 65, kEnemySide 4), kExposureHomes false (forward region to the sturdiest pick).
- Home slots, factors (own 1, shared 0.8, other's 0.6) and everything outside the opening: the parent's.

## Changes this generation
- Parent baselines for free: evo-results r10 holds f10-03's games on the 40 local maps (reproduced 82/82 here).
- Evidence diagnosis (stats files: deaths, clears and fuel-exhaustion events): early deaths decide mirror games (the death
  difference at round 100 predicts the winner in 30 of 38 games), and on symmetric maps the first fights repeat exactly by
  side (same lineup and opening on both teams). Deaths come from lone champions hunting near contested groups, 87 of 125 on
  our own half: fight logic, not the route. Every champion ran out of fuel on its first turn on most maps (init).
- Draft-memory analysis + init guard (F1), Varg: vs f09-01 46/80 (+52) (parent 50/80, +58); maps without fuel trouble
  play move for move like the parent (11 maps identical); the three evidence losses (01boss-battle A, 01starfish B,
  00gemfight A) all turn into wins, others flip the other way.
- Ryth instead of Varg alone (no precompute), 20 five-pick maps: vs f09-01 25/40 (+38) (parent 21/40, −43), vs f08-04
  24/40 (+203) (parent 25/40, +246).
- Final (F1 + Ryth): vs f09-01 53/80 (+195) (parent 50/80, +58; five-pick maps 33/52 vs 31/52), vs f08-04 57/80 (+318)
  (parent 56/80, +348), vs parent f10-03 41/80 (+49), vs f07-02 20 maps 27/40 (+269) (parent 23/40, +180), vs f05-02
  half 1 34/40 (+504) (parent notes 30/40, +388), vs f06-05 half 1 31/40 (+357) (parent notes 30/40, +328), f00-03 20/20,
  starter 11/11 (+1 invalid game: the starter's own `bot error:`). 0 faults, 0 `bot error:` of ours; 1 caught `warn:` in 240.
- Game budget: 854 games played.

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

- (f11-03) All vs f09-01 unless said; parent 21/40 (−88) on the 20 "early fight" maps, 50/80 (+58) on all 40.
- (f11-03) Contested groups (d_enemy ≤ d_ally + 3, beyond 10 of our spawn) ×0.3 for the first 120 rounds: 19/40 (−28) vs
  21/40 (−88). Fewer deaths on both sides, the enemy takes the contested groups: neutral.
- (f11-03) Team opening route (draft plans 3 groups by value per team-turn, all champions take the current one, ×1000):
  9/40 (−298). Parallel farming is worth far more than numbers at the first fight; deaths did not drop.
- (f11-03) Ryth 4th instead of Tavi (Tavi 5th) on the 8 four-pick maps: 16/32 vs 23/32 with Tavi 4th.
- (f11-03) Home regions by exposure (value-weighted d_ally − d_enemy: most forward to Marcella, then Ryth, Vendetta,
  Vesper Vale, Tavi): 45/80 (+76) vs 53/80 (+195).
- (f11-03) The draft memory cannot hold walking fields; the remaining init (Known/bitboards per group ~70k, threats ~30k,
  home/altar BFS) still overruns turn 1 on 01boss-battle, 01big-world, 01islands, 00road-of-riches, 00great-wall.
- (f11-03) Debug prints (iostream, ~2-3k fuel each) moved the first-turn fuel picture: profile with few prints.

## Known weaknesses
- Early fights are still decided by side in mirror games (same fight logic on both teams); the opening only reshuffles which.
- The rest of init (outside the opening) still costs ~120k: on the biggest layouts champions first act in round 2-4.
- Ryth vs Varg is a small, noisy gain (+2 to +4 wins per 52 five-pick games vs f09-01/f08-04, about even vs the parent).
- 3-pick maps keep Vesper Vale, Vendetta, Marcella untested against alternatives this generation.
- 01boss-battle: whoever kills the monolith first wins (parent's note still holds; this generation won it 2/2 vs f09-01).
