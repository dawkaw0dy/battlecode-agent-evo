## Strategy (≤10 lines)
Scaling economy from f01-07 … f07-02 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f05-03's endgame (altars, locks, banking); f06-05's free shots;
f07-02's draft-time map plan (contest/boss/runner flags in draft memory).
New (f08-02, focus camps): where the only altar can only be reached over a boss's spawn tiles (01boss-battle: the
one tribute tile is a monolith tile), champions with full bags no longer walk into the boss alone. They gather on
the ring just outside its reach until 3 raiders are there (or someone has woken it), at most 35 rounds, then farm
for 25 rounds and try again. Everything else plays exactly like the parent (verified: identical games).

## Key parameters
- kPlugRaiders 3 (bot.cpp): raiders gathered before a boss on the only altar is attacked; 1 restores the parent.
  Uses kRallyRounds 35 for the gather and the parent's altar_wait_until_ (+25 rounds) on timeout.
- Unchanged from the parent: kSimTurns 10, kEngageBias −150, kChaseTurns 8, kShareTurns 15, kTripTurns 8 (+ walk
  ≤ 60), kRallyRounds 35, kGatherTurns 10, gold weight 0.35 (Vesper 0.5) × max(0.4, 1 − round/1500), enemy-side
  factor 0.7, other-home factor 0.6, shop trip at 240 gold, kBossTurns 8 (altar boss: 4).

## Changes this generation
- Kept: altar-boss gather (above). Only 01boss-battle among the 40 local maps has an altar walled in by a boss
  (checked every altar's neighbours), so all other maps replay the parent's games exactly (checked on 6 games vs
  f04-05 and 46 vs f06-05). 01boss-battle, both sides, vs parent: f04-05 0/2 (parent 1/2), f06-05 2/2 (1/2),
  f05-02 2/2 (2/2); gem margin over these 6 games +1128 vs parent −238; vs starter 1046/1060 rounds vs 1405/990.
  Cause found in the stats: the parent's champions died one by one to the monoliths at rounds 506, 616, 645
  (each with 45–53 gems, destroyed by camp kills) and banked nothing until round 1066.
  kPlugRaiders 4: 5/8 incl. starter, vs starter 1676/1094 rounds; 2: 4/6 panel, margin −228. 3 kept.
- Final check: vs f06-05 on the 24 local evaluation maps 28/48 (parent 27/48; only 01boss-battle differs),
  0 engine faults, 0 `bot error:` lines from our side.
- Game budget: 880 games (all benchmark runs and debug runs counted).
- Tools I wrote (scratch, not committed): a map renderer on the map-editor wasm, per-group capture counts from the
  stats files' campClear events, death/camp-death counts. Useful facts: captures are logged at a group spawn
  tile; deaths carry context "camp" when a camp killed the champion; fuelExhausted events are ~1 per champion.

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

- (f07-02) Pack raids on contest maps (contested gem share ≥ 0.6: the whole team claims each contested group with
  ≥ 10 gems, value not split): 13/30 and 11/30 (−240) vs f05-02 on 15 contest maps, parent 17/30 (+121). The pack
  reached the 3-elk group of 00stampede at round 55 at base stats; the tank fell to the hurt rule (hp < incoming ·
  (exit + 2) + 40) and recalled, the rest followed. Nobody takes that group before round ~850 in the parent's games.
- (f07-02) Full-team boss raids on boss maps (boss share ≥ 0.4; raid size = living allies instead of visible ones,
  ×1.5 score, ≤ 1 trip home each): 9/28 vs f05-02 + f03-05 on 7 boss maps, parent 18/28. With the old ≤ 3 trips
  the team rallied at the monolith at round 10. Parent raids are incoherent (n = visible allies + 1, so 2–3; two
  monoliths alternate as targets) and monoliths die only after round ~950, but committing everyone costs more.
- (f07-02) Parent B's spells on maps ≥ 3000 tiles (B beat A 12/20 there head to head, A beat B 14/18 on small maps):
  vs f05-02 11/20 (+76) vs parent 8/20, vs f04-05 9/20 vs 13/20: 20/40 vs 21/40, no gain.
- (f07-02) "First strike": out of reach and not trading blows, wait instead of walking into the enemy's reach;
  confounded first run 14/30, then only against enemies that closed in since last round: 44/80 vs f05-02, parent 45/80.
- (f07-02) Durability emblem first, Jungle last, on contest maps (same shards, 10% less damage from everything
  instead of 30% less from camps): 52/90 vs f05-02/f04-05/f03-05 on contest maps, parent 59/90.
- (f07-02) Facts found on the way: map files hold half the camps (the engine mirrors them); first victim's team
  loses 58% of 132 games; 1v1s like Vesper Vale vs Vendetta are engaged at sim value −14 (nobody dies within 10
  turns) and then flip-flop engage/retreat until the ranged one dies; the lock runner (Varg) earns ~800 gold a game
  against ~1500 for the others.

- (f08-02) Noise level: a change that touches every map flips ~30% of panel games either way, so 48–96 panel games
  only resolve effects of ±5 wins; vs starter, 24-game mean rounds have a standard error of ~15 rounds. All the
  global camp tweaks below landed inside that noise. Even switched-off experiment code (constants left in the
  source, one extra struct field) changed 4 of 8 games through fuel use, so reverted experiments were removed.
- (f08-02) Solo farming of golem groups (bosses without a monolith): vs starter on 4 golem maps +237 rounds over
  8 games (slower); panel 13/24 vs parent 12/24. A champion needs ~130 dps before a golem takes ≤ 4 trips.
- (f08-02) Raids planned with living allies out of sight (up to 2 raiders, claim tells them): all groups: vs starter
  +25 rounds/game on 16 maps (boss maps +200–400: pairs start monolith raids); non-boss only: +4 on 11 maps, +59 on
  5 slow maps (00pilgrimage, 00reverse-spiral, 01shortcut, …). Golem groups only, up to 3: +73 on 6 golem maps.
- (f08-02) kRallyRounds 35 → 15: most games identical (the cap is rarely reached), +23 rounds/game on 10 maps.
- (f08-02) Full-team boss plan (the parent's kBossPlan) vs starter on the 7 boss maps: +294 rounds/game; with every
  raider on the same boss (nearest our spawn): +406. Traces: raiders pick different monoliths at different times
  and each waits alone at its rally ring ("near 1 need 4"); rally_goal turns cost 70–86k fuel.
- (f08-02) Gold weight in group value ×0.7 / ×1.4 / ×2.0: vs starter +2.5 / −15 / +9 rounds per game (24 each,
  not monotonic); ×1.4 vs f04-05 + f06-05 on 24 maps: 55/96 vs 54/96. Noise.
- (f08-02) Enemy-side group factor 0.7 → 0.5: vs f04-05 + f06-05 on 24 maps 56/96 vs 54/96 (f04-05 +6, f06-05 −4).
- (f08-02) Trip cost walk ≤ 60 → ≤ 30: +14 rounds/game vs starter. Other-home factor 0.6 → 0.4 / 0.8: +14 / +16.
  Shop trip at 240 gold → 150 / 360: +22 / +2.
- (f08-02) No raids on non-boss groups (solo only): vs starter −10 rounds/game on 24 maps (48 games; 00stampede and
  01corridors faster, 00reverse-spiral and 01shortcut slower), but panel 72 vs 73 wins of 120 vs f04-05, f05-02,
  f06-05 and −31 gems of margin per game. Reverted.
- (f08-02) Facts: vs starter the parent never captures the monolith groups of 00rows (470 gems), 01corridors (428),
  00great-wall (414 ×2 and the 4-golem group, 256), 01big-world (228 ×2) within 1100–1600 rounds; 2-elk groups are
  first taken at rounds 500–1200; on 00pilgrimage the 88-gem central group first at 726 and the 33-gem elk group in
  our half never, while 0-gem gold pairs are captured 10+ times each. ~33% of turns walking, ~16% waiting at
  respawning groups, ~13% capturing, 5–10% at rally rings. We die ~24 times a game vs the panel, 4% of it to camps.

## Known weaknesses
- Monolith and other heavy groups are taken late or never; every raid variant tried (pairs, golem trios, the full
  team, one shared boss) farmed slower. Raid coordination (who goes, when, which boss) is the open problem.
- 01boss-battle vs f04-05 is still lost on both sides: its lone attacks still wear the monoliths down first sometimes.
- Most deaths still come in retreats and in 1v1s the 10-turn simulation cannot resolve; snowball maps (00swirl,
  01hooks, 01starfish, 01yellow-brick-road, 00stampede) are decided by the first fights.
- Islands (01islands) camps behind hole rings are never farmed: no teleport crystals; no wards.
- The altar-count rule ignores where the altars are; our own tribute channel is never cancelled.
