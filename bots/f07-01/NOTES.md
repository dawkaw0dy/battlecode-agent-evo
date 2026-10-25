## Strategy (≤10 lines)
Parent f06-05 (scaling economy: draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions per champion; gold
into shards; focus-fire fight simulation; f04-05's fuel subsystem; f05-03's endgame), with the camp subsystem
rewritten from scratch in camps.cpp behind the same interface (build_threats, choose, farm, estimate_damage,
remaining, escape_camps). Every group gets its best plan for n = 1 .. allies in sight (or its claimers + 1):
walk, respawn wait, clear (1/n of the time), trips home (1/n² of the damage each), capture (one champion, all at
a boss), banking; rate = (gems + weighted gold) per champion-turn. A two-step lookahead adds the best follow-up
group from the end of each candidate (respawn timers included). A fading danger memory (enemy sightings, our
deaths) discounts groups. A reward that cannot be captured within capture time + 10 rounds of arriving is dropped.

## Key parameters
- camps.cpp: kCaptureTurns 6, kTripTurns 8 (+ walk back), kGatherTurns 10, kRallyRounds 35; lookahead on
  (kFirstK 12 candidates × kNextK 8 follow-ups); danger: kDangerRounds 80 fade, kDangerWeight 0.5 per enemy seen
  within reach + 6 of the group, kDeathWeight 2; gold weight 0.35 (Vesper 0.5) × max(0.4, 1 − round/1500).
- Plans: solo only if not a boss and health − margin ≥ 5 × peak fire; n·cap ≥ 3 × peak; trips ≤ 4 (solo) / 3;
  racing a group's claimers alone × 0.3 per claimer; joining a clear its claimers finish before we arrive skipped;
  raid sizes up to 1 + allies in sight; rally (gather outside reach) when boss or peak × 10 > spare health.
- Capture: give up (block 60 rounds, drop the target) capture_time + 10 rounds after coming within 3 of a reward.

## Changes this generation
- Baseline for free: the parent's r06 games in evo-results reproduce exactly (same digest), so every test on the
  24 local maps of r06-B vs f03-05/f04-05/f05-02 (48 games each) and vs starter (rounds to win) has a baseline.
- Diagnosis (debug copies in the scratchpad, position log every 10 rounds): time use vs the starter is ~35% travel,
  15% hitting camps, 5–50% waiting for respawns; vs real opponents "enemy" mode takes 30–40% of all turns and
  close games have 30–60 deaths per side (v4 vs f05-02: 44% of our deaths mid-map, 29% on the enemy half).
- Bug found: a champion kept "capturing" a reward it could not reach (enemy or unit on the region) for ~500 rounds
  (00corners vs starter, Vesper 370–419 capture turns): the timeout only ran while standing on a spawn tile.
- Final (v4) = rewrite + lookahead + danger memory + unified n-champion plans + capture timeout:
  vs f05-02 30/48 (+147; parent 25/48, +58), vs f04-05 31/48 (+238; parent 29/48, +139), vs f03-05 27/48 (+49;
  parent 34/48, +296), vs f06-05 (mirror) 25/48 (−16), vs starter 40/40 (mean rounds to win 1120 vs 1116),
  vs f02-01 14/16, f01-07 14/16, f00-03 16/16 (8 maps). Overall parity with the parent (113/192 vs 112/192 on the
  three baselined opponents + mirror). 0 faults, 0 `bot error:`, one caught `warn: spell` in 192 games.
- Game budget: 893 games played.

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
- (f07-01) All tests below: 24 local maps × 2 sides vs f05-02 and vs f04-05 (96 games; parent 54/96, final 61/96).
  Variants that only change decisions now and then still move results by ±10 wins per 96 games (chaos):
  differences of that size are not resolvable with a 900-game budget.
- (f07-01) Raids planned with every living ally (sdk_champion_is_alive works out of vision; positions do not, engine
  error 97) instead of the allies in sight: champions converged from across the map, rounds to win vs starter
  1210 vs 1116 (01big-world 1889 vs 1373, 01starvation timed out). Plan raids with nearby allies only.
- (f07-01) Forget stale knowledge (a damaged group unseen for respawn + 40 rounds reset to full health, a reward
  unseen for 40 rounds reset to unknown): 46/96 (−70 mean margin) vs 61/96 (+192).
- (f07-01) Contested groups (d_enemy ≤ d_ally + 4): lone champion's value ÷ 2 and pairs gather before engaging:
  41/96 (−129). Avoiding contested ground cedes it; the fights happen anyway, on our side.
- (f07-01) Parent's sighting rule (× 0.5 within 7 of the latest shared sighting, 24 rounds) instead of the danger
  memory: 46/96 (−20).
- (f07-01) Gold weight falling faster (0.35 × max(0.1, 1 − round/1200)): 51/96 (0).
- (f07-01) Unified n-champion plans alone (pairs for elk groups) without the other fixes: rounds to win vs starter
  1134 vs 1099 with lookahead + danger only (noise level ~±20).
- (f07-01) The deathball lineage is weaker: parent f06-05 vs e06-05 15/20 on 10 maps (e06-05 won 00gem-fields).

## Known weaknesses
- Fights still decide close games: 30–60 deaths per side, "enemy" mode 30–40% of all turns; camp choice cannot
  avoid them (avoiding contested ground measured worse).
- The last pick runs altar locks for 450–700 turns of a game (Marcella in 3-champion maps): a third of a small team.
- 01boss-battle: the only altar sits beside the monoliths; side A never tributed vs f04-05 (0 of 1886 gems banked).
- Raid sizes count only allies in sight, so big shared groups (00stampede's 160-gem elk pack) are raided late;
  a strong tank (8800 hp) kept farming gold clusters there.
- Vesper (gold weight 0.5) farms gold-only cluster groups for most of a game on maps with many of them.
- Our own tribute channel is never cancelled; no wards or teleport crystals; ally positions outside vision unknown.
