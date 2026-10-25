## Strategy (≤10 lines)
New opening (mutation: radical, focus: opening): the draft turn (10x fuel, whole map) plans every champion's
opening route and the live chooser follows it. plan_routes() (plan.cpp) takes the groups one champion clears
on one health bar on our side of the map, measures walking distances between them (one host BFS per group),
and assigns them greedily (the champion free earliest takes the best value per turn: walk, clear, capture).
The routes go into draft memory (bytes 256-463). In play, choose() weighs the next group of our own route x2.2,
later ones x1.2, groups planned for an ally x0.5 and raids x0.5 until our route is used up; then the parent's
chooser runs as before. The lock runner is gone (it cost Varg ~100 opening turns). The draft picks before it
plans, so a draft turn out of fuel cannot leave a pick to chance. The parent's infrastructure is unchanged:
pathing, fight model, spells, defense, endgame, fuel subsystem, picks (Vesper Vale, Vendetta, Marcella, Tavi, Varg).

## Key parameters
- bot.cpp: kRouteNext 2.2, kRouteMine 1.2, kRouteOther 0.5, kOpenRaid 0.5, kForceRoute false, kLockRunner false.
- plan.cpp plan_routes(): kHorizon 400 rounds, kMaxTrips 0 (single-bar clears only; a clear needs ≤ 75% of
  health), kContested 2 (groups at most 2 steps nearer the enemy spawn), kMaxCand 48, kGoldW 0.35,
  kCapture 6, fuel reserves 300k (BFS rows) / 40k (routing loop). Slot stats: hp, camp dps, range per pick.
- Draft memory: 256 'R', 258-262 route length per slot, 264 + 40·slot route group indices.

## Changes this generation
- Why: the parent's champions wasted the opening. Activity counts, first 200 rounds on 00road-of-riches: four
  champions walked to and rallied 20-92 turns at a far golem raid; Varg spent 70-107 turns running altar locks.
- All games vs f10-03 (current best) on all 40 maps, both sides (80 games). "Econ" = our/their collected gems
  at round 300, summed over the 80 games (tools: stats JSON timelines).
- v1 forced route (walk the plan, then the chooser), no lock runner: 42/80 (+47), econ 6988/7144; vs the
  parent 44/80 (+86). Leads at r100, behind by r200: routes were 1-2 groups long on small maps.
- v2 + up to 3 heal trips per planned clear: 40/80 (+15), econ 6657/7625. Activity over 6 maps x 400 rounds
  (vs the parent): travel 3540 vs 2429 turns, rally 54 vs 321, bank 551 vs 1084. The plan walked too much.
- v3 plan guides the chooser (x1.6 / x1.2 / x0.7, raids x0.5) instead of dictating: 40/80 (+39), econ 7461/6700.
- v4 = v3 with single-bar clears only: 45/80 (+121), econ 7829/6190, gold earned at r800 +17%, deaths -13%.
- v5 = v4 with x2.2 next / x0.5 others: 52/80 (+231), econ 7273/6413. vs the parent f10-01: 41/80 (+60),
  econ 7265/6289 and r800 gems 27118/23992 (+13%).
- v6 = v5 with horizon 260 → 400 (kept): 53/80 (+236), nearly the same games (routes end when candidates run
  out, not at the horizon).
- v7 = v6 planning groups up to 6 steps nearer the enemy: 52/80 (+174), econ 7446/6246: no gain, reverted.
- Final build (v6): vs starter 40/40 and vs f03-05 33/40 on 20 maps. 0 engine faults, 0 caught exceptions
  (`bot error:`) in all games. Game budget: 841 games played.

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
- (f11-04) Forced opening route (champions walk the draft's plan, the chooser takes over after it): 42/80 vs
  f10-03 and gems at r300 below theirs; guiding choose() with score factors beats dictating the target.
- (f11-04) Heal trips inside planned solo clears (up to 3): plan sends champions on long trip-heavy clears,
  travel turns +45%: 40/80 (+15) vs 45/80 (+121) with single-bar clears only.
- (f11-04) Planning contested groups (d_ally ≤ d_enemy + 6): 52/80 (+174) vs 53/80 (+236); no gain.
- (f11-04) Horizon 260 vs 400 rounds: no difference (routes are short; candidates run out first).
- (f11-04) Draft-turn fuel: analyse_map() costs 260-410k of the ~975k draft fuel; a planning loop that
  evaluated 80 candidates per step ran the draft turn dry. Pick first, then plan under fuel guards.

## Known weaknesses
- The gem lead is consistent (+13-26% at r300, +11-16% at r800 vs f10-03 and the parent) but turns into wins
  only part of the time: vs the parent 41/80. Fights and map specifics still decide many games.
- Maps where every group needs a raid (00contest, 00stampede, 01elk-arena) get no route: the opening there is
  the parent's chooser with raids discounted.
- Routes are static: no re-planning when an ally dies or the enemy takes a planned group (it is just skipped).
- 00gemfight (-852 both sides vs f10-03), 01islands, 00road-of-riches, 00contest still lost on both sides.
- The draft picks are the parent's fixed five; the planner does not change them per map.
