## Strategy (≤10 lines)
Scaling economy from f01-07 … f08-04 (unchanged outside the map plan): draft Vesper Vale, Vendetta, Marcella, Tavi,
Varg; home regions per champion; groups scored by value per champion-turn; raids for bosses; gold into shards;
focus-fire fight simulation; f04-05's fuel subsystem; f05-03's endgame; f08-04's spells.
This generation (mutation: subsystem rewrite, focus: map-adaptive): plan.cpp/draft.cpp rewritten behind the same calls
(analyse_map, pick_order, slot_of, assign_homes) plus map_plan(). At draft time the map is classified (altar tiles,
area, team size) and a MapPlan goes into draft memory (bytes 512+: picks, flags, lock detour, runner slot, class),
so every champion follows it. Lock policy: runner and 8-tile lock detours only with ≤ 2 altar tiles (the enemy walks
on to an open altar otherwise). Home regions: the balanced k-means regions are ranked by how far forward they lie
(value-weighted d_ally − d_enemy) and given to picks by sturdiness: Marcella the most forward, Vesper Vale the safest.
The holder of the most forward region never runs locks (3 picks: Vendetta runs instead of Marcella).

## Key parameters
- plan.cpp: kRunnerAltars 2 (most altar tiles for the runner flag and lock detours), kLockDetour 8 on those maps,
  0 otherwise; kRoleHomes on; kBackRunner on; sturdiness Marcella 6, Vendetta 5, Varg/Ryth/HoR 4, Ren 3, Tavi 2,
  Vesper Vale 1; region front = value-weighted mean of clamp(d_ally − d_enemy, −40, 10).
- Draft memory: bytes 0–253 as the parent (fuel.cpp reads them); 512 'Q', 513 flags, 514 lock detour, 515 map class
  (small < 1300 tiles, large ≥ 3000; stored, unused), 516–520 picks, 521 runner slot. Missing plan → made from the map.
- bot.cpp reads map_plan() only in the lock-runner line (runner flag, runner slot, lock detour).
- Rest of the bot: parent values.

## Changes this generation
- Baselines for free: the parent's games in evo-results r08 reproduce exactly (checked 01gold-rush vs f07-02).
- Offline replica of the home assignment (python, from the map files) predicts which maps a home variant changes;
  it matched the engine exactly (maps it marked unchanged replayed identically), so tests ran only where it mattered.
- Lock policy (on the parent's 10 worst 3+ altar maps, vs f05-02/f06-05/f07-02): 39/60 vs parent 23/60.
- Role homes on top: same 10 maps 45/60 vs 39/60; 8 ≤ 2-altar maps 30/48 vs parent 24/48 (flips side-B snowball
  losses on 01hooks, 01starfish, 00swirl).
- Final build over all 40 local maps vs f05-02, f06-05, f07-02: 134/198 (mean +208) vs parent 114/198 (+104);
  3+ altar maps 65 vs 47/100, ≤ 2 altar maps 69 vs 67/98; 3 picks 27 vs 18/36, 4 picks 27 vs 25/40, 5 picks 80 vs
  71/122. Caveat: on the 22 maps not used for tuning alone it is 56/88 vs 65/88 (the gains sit on the parent's
  worst maps, partly regression to the mean).
- Head-to-head vs parent f08-04: 43/80 (40 maps, both sides; 00collision with the pre-final runner rule). vs f04-05 26/40, f03-05 12/16; starter 8/8, f00-03 7/8,
  f01-07 7/8, f02-01 8/8.
- Unusual maps (made with tools/mapgen.mjs): 100×100 with 3 picks and 30×30 with 5 picks and 2 camps: no faults.
- 0 faults, 0 `bot error:` in every run; one inherited `warn: flee move: engine error 21` (01yellow-brick-road).
- Game budget: 742 games played (all tools).

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

## Known weaknesses
- The map plan only steers locks and home regions; aggression and the draft are the same on every map (draft
  variants and frontline orders measured worse or neutral).
- Held-out maps (not used for tuning) are below the parent (56/88 vs 65/88): the role-home gain is map-dependent.
- 01boss-battle (single altar behind a boss) farms far too slowly against f04-05; 00great-wall lost both sides.
- Most deaths still come in retreats; snowball maps are decided by the first fights.
- Our own tribute channel is never cancelled; lock memory is per champion; no wards or teleport crystals.
- One inherited warn: a flee move attempted when the champion cannot act (engine error 21, caught).
