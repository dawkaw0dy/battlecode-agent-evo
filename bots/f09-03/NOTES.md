## Strategy (≤10 lines)
Scaling economy from f01-07 … f08-04 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
f04-05's fuel subsystem; f05-03's endgame; f08-04's spells. This generation (mutation: subsystem rewrite, focus:
combat) rewrote fights from scratch in combat.cpp behind the same calls (enemies, strike, retreat, threat_at, camp_fire).
Turn order decides even fights: champions act one at a time, alternating teams, ranked by draft order, and the starting
team swaps each round, so a champion and the enemy of the same rank (the mirror pick) take turns in pairs. The fight
model plays the fight hit by hit in that order (has_completed_turn says who already acted; who starts is learnt from
the first equal-rank enemy and shared in two spare sighting bits). Threats count the enemy's 0/1/2 turns before ours;
the approach stops just outside the enemy's reach so it walks into ours (first hit). Retreats route around camps.

## Key parameters
- combat.cpp: kSimRounds 10 (ordered model horizon), kEngageBias −150, kHurtValue 60, death value 150 + 2·gems.
- Engage: value > −150 and not (we die, no kill); kMarginal: if the allies win without us, also value − without > −150.
- Approach (kSafeApproach) within range + 3: tile score 60·gap-to-range + hits the enemy lands before our next turn
  (melee champions ignore ranged enemies there); holding the edge at most kHoldMax 3 turns, then farm on.
- hits_before_us: melee first hit on turn max(1, steps) (dash −2 steps), ranged on steps + 1; hits = moves − first + 1.
- Retreat: field from our spawn with walls + known camps (compute_to, cut at 60), score 4·danger + walk home; dash out
  when it scores 2 better; cornered (no better tile) → strike (kStandAndFight); free shots when threat_at(here) = 0.
- Off (wired in, measured no gain): kSmooth, kCaughtShots, kPressRunners; kEngageDash on but almost never fires (the
  dash is spent on travel before fights). Unchanged: kChaseTurns 8 / 15, kAllyDelayMax 3, kKillNow 0.85, kRecallDist 7.

## Changes this generation
- Parent results for free: evo-results r08-A/B/C hold f08-04's games (deterministic; reproduced 4/4 here) vs f05-02,
  f06-05, f07-02, f08-0x, starter on all maps. Parent's local record there: 60–67% vs f05-02…f08-05.
- Traces (01gold-rush, 00stampede vs f06-05): engage/retreat flipped every turn in even fights (a 10-turn pooled sim
  reads a mirror duel as "both die" or "we die" depending on who hit last); retreats stood still in camp pockets
  (walking field ignores camps) without shooting; a ranged champion caught by melee read "no death in 10 turns" until
  its health dropped, then fled while adjacent. Probed the engine: turns interleave A0 B0 A1 B1 …, equal ranks alternate
  double turns, has_completed_turn works on visible enemies only, the enemy list is in draft order.
- New subsystem (combat.cpp) vs parent, same games: 01gold-rush/00packs/… set vs f06-05 6 maps 9/12 (+488) vs parent
  6/12 (−24); 12 maps vs f05-02 16/24 vs 14/24, vs f07-02 15/24 (+223) vs 10/24 (+18); 28 other maps vs f07-02 29–32/56
  vs parent 35/56 (those maps lean to the parent's strong ones: on all 40 maps vs f07-02 44/80 vs 45/80). Final build on
  every 4th map (10 maps, both sides): vs f06-05 17/20 (+408; parent 9/20, −92), head-to-head vs f08-04 11/20 (+108),
  12-map head-to-head (v6) 16/24. Deaths: 406 vs 676 (f06-05), 445 vs 546 (f08-04).
- Panel sanity (2 maps each, both sides): starter 4/4, f00-03 4/4, f01-07 4/4, f02-01 3/4, f03-05 3/4. vs f04-05
  12 maps 17/24. 0 faults and 0 `bot error:` lines in every run; rare caught `warn:` lines (≈1 per 10–20 games, stale
  state after a fuel pause; live() checks added before every combat action).
- Game budget: ≈849 games played (counted conservatively, partial-round probes included).

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

- (f09-03) All on the 6-map set vs f06-05 (12 games) unless said; parent 6/12, 345 deaths vs 335.
- (f09-03) Price of running (chasers within a move hit once, then 0.5·sustain per round for min(walk home, 12) rounds;
  engage when fight − run > −150): 3/24 and 2/24 on 12 maps vs f06-05/f07-02, deaths 2× theirs. It reads every 1v3 as
  "running is death too" and fights it out. Removed.
- (f09-03) Pooled sim played to the end (30 turns): mirror duels flip between won and lost each turn as hits alternate;
  smoothing the kill value by the winner's health margin (×min(1, margin/0.3)) fixed the flip but not the result: 1/12.
- (f09-03) Ordered model horizon 20 instead of 10 (screen of 12 games vs f06-05 + 12 vs f08-04): 14/24 vs 17/24.
- (f09-03) kEngageBias −250: 14/24 vs 17/24 (better vs f06-05, worse vs f08-04).
- (f09-03) kCaughtShots (ranged, every reachable tile hit by a melee chaser, home > 3: shoot instead of stepping):
  14/24, more deaths vs f08-04. Off.
- (f09-03) kPressRunners (approach ignores the reach of enemies that moved away since our last turn): 13/24. Off.
- (f09-03) Marginal engage (value with us − value of the allies alone > −150): 28/56 vs 32/56 on 28 maps vs f07-02;
  every champion assumes the others stay, so all join a fight they lose together. Kept only as a veto (allies who win
  alone are not joined at a cost).
- (f09-03) Engage dash for melee dashers (dash + step + hit from up to 4 tiles): results identical to without; the dash
  is on cooldown from travel when fights start (f08-05's travel reserve would be needed; spells.cpp is not ours).
- (f09-03) Screening on 24 games cannot separate variants (±3 games); 56-game runs on a map half biased by the selection
  of the other half mislead too (regression to the mean). Compare on all 40 maps or a systematic sample.
- (f09-03) Traces (kTrace/kStats) change fuel and so the game: a traced loss is a different game, not a diagnosis.

## Known weaknesses
- Evidence for the gain is moderate: parity with the parent vs f07-02 over all 40 maps, clear gains vs f06-05 and in
  the head-to-head; fewer deaths everywhere but more turns spent near enemies (holding the edge, approaching).
- The turn-order model assumes draft-order ranks; the starter's random priorities break it (harmless so far: 4/4).
- Until an equal-rank enemy is seen, the starting team is guessed (we start); a wrong guess flips double-turn timing.
- Varg dies most while engaged as the tank under camp plus champion fire in trades the model rates as good.
- Snowball maps (01yellow-brick-road, 01corridors, 00split, 01big-world) are still decided by the first fights.
- No wards or teleport crystals; allies out of our vision are unknown; lock memory is per champion.
