## Strategy (≤10 lines)
Scaling economy from f01-07 … f08-05 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem; f05-03's endgame (altars, locks).
This generation (subsystem rewrite, focus defense): defense.cpp rewrites threat_at(), retreat(), recall_or_walk() behind
the same calls and adds guard(). Enemies hit only what they see: threat on a tile counts only enemies whose shadowcast
(visible_tiles_from(their tile), ~450 fuel) covers it, so bushes and walls are cover. Retreats take the least-threat
tile, pulled towards a bush no enemy sees into and no living camp reaches; awake camps cost the steps left to leave
their reach. Recall only when nobody who sees us arrives within 5 turns. guard() cancels our tribute when visible
enemies kill us before it completes and a recall a hit will end; unexplained damage (bush ambush) moves us away from
unseen bushes. On a boss-only altar we hold the boss tile that takes no tribute.

## Key parameters
- defense.cpp: kRecallSafe 5 (turns every enemy that sees us needs to reach us before we recall), kBlindNear 2 (an
  enemy that does not see us must be further than this), kCoverRadius 4 / kCoverWeight 30 (retreat score per tile to
  the nearest unseen, camp-free bush), camp fire in retreats = str × taken × steps to leave its reach (× 4 like
  threat), kAmbushDrop 30 / kAmbushRounds 3 (unexplained health loss → avoid unseen bushes within 3 for 3 rounds),
  kChannelPad 40 (a tribute must leave this much health after the exposure of its channel).
- Tribute start (endgame.cpp): channel_safe(ceil(gems/10)+1) = health > camp fire + visible enemy damage over the
  channel (sustain per turn after they arrive, burst once, enemies blind to us and > 2 away ignored) + 40.
- Rest of the bot: parent values (kSimTurns 10, kEngageBias −150, kAllyDelayMax 3, kKillNow 0.85, kDashReserve 10, …).

## Changes this generation
- Diagnosis (death trace, kDeathTrace in bot.cpp; gem-loss events from the stats JSON): ~93% of our deaths are champion
  kills, ~80% of them carry no gems; most come in chases (adjacent melee hitting every step) and fight/flee flip-flops;
  ~14% had no visible attacker (enemies in bushes, unseen camps). Gem losses are ~120/game, half of them on 01boss-battle.
- v1 (vision-aware threat + recall_safe + guard + evade): vs f08-05 (parent) 46/80 (+74), deaths 1813 vs 2383;
  vs f08-04 40/80 (+45) vs parent 41/80 (+51), our deaths 1862 vs parent's 2090, gems lost to kills 7396 vs 10011.
- v2 (+ bush cover in retreats): vs f08-05 44/80 (+154); vs f08-04 45/80 (+47), but 01boss-battle lost 2–3k gems per
  game to the monolith: champions hid in the bush beside it (camps see into bushes) and stood still there.
- v3 (kept): cover excludes tiles living camps reach, camp fire counts the steps to leave its reach, boss-tile hold:
  vs f08-05 52/80 (+204), deaths 1518 vs 2523; vs f08-04 48/80 (+165), deaths 1625 vs 2560 (parent there: 41/80 +51).
  Panel, 4 maps (00split, 01starfish, 00quadrants, 01gold-rush) × 2 sides: starter 8/8, f00-03 8/8, f01-07 8/8,
  f02-01 7/8, f03-05 7/8, f04-05 6/8, f05-02 6/8, f06-05 5/8, f07-02 7/8. 0 faults, 0 `bot error:` in all runs;
  a few caught `warn: flee move: engine error 21` lines (≈1 per 30 games, not after our own channel cancels).
- Game budget: 836 games played.

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
- (f08-05) All measured against the kept dash reserve (v1) on 20 maps (odd half of the sorted list) × 2 sides
  vs f07-02 + f05-02 = 80 games; v1 there: 49/80 (f05-02 29/40 +284, f07-02 20/40 −0).
- (f08-05) kDashReserve 20 instead of 10: 44/80 (+55, +19). Not kept.
- (f08-05) No damage/stun spells on camps for 10 rounds after an enemy sighting (save Tavi's stun and Marcella's
  spell for the fight): 47/80 (+134, +41). Not kept.
- (f08-05) Walk-excluding camp spells only once a camp is in basic range (casting from range 4 costs the approach
  step): 43/80 (+282, −159). Not kept.
- (f08-05) No dash during chases either (pure reserve while enemies are near): 40/80 (+194, −154). The chase dash matters.
- (f08-05) Varg's camp shield at any health (was below 90%): 49/80 on the odd half, but 24/40 (+16) vs v1's
  29/40 (+284) vs f07-02 on the even half. Not kept.
- (f08-05) Marcella's stunning reset before her damage spell when an unstunned enemy is in basic range: 50/80
  (+222, +45) vs 49/80 (+283, +34) with the shield change. Neutral; not kept.
- (f09-04) Unseen-camp evade (also step out of reach of living camps we cannot see after an unexplained hit):
  vs f08-05 48/80 (+134), deaths 1654 vs v3 52/80 (+204), 1518. Reverted.
- (f09-04) Stronger cover pull (kCoverWeight 60, radius 5): 45/80 (+123), deaths 1675. Reverted.
- (f09-04) Ranged enemies with a ready attack reset reach range + 1 in threat_at/arrive: 51/80 (+185), deaths 1574;
  neutral, not kept.
- (f09-04) Win counts are near useless at 80 games: 39 of 80 games flipped result between two builds that differ only
  in retreat logic. Deaths per 80 games (our vs theirs) and mean margin move more consistently.

## Known weaknesses
- Chases in open ground: a same-speed melee chaser still hits a fleeing champion every turn when no cover is near.
- 01boss-battle: bags grow to hundreds of gems while the monolith closes the only altar and change hands in fights.
- Fight/flee flip-flops near the −150 engage threshold (combat subsystem, unchanged) still cause many deaths.
- Our own vision only: a hidden attacker is inferred from health loss, never located; allies' sightings are 3 bytes.
- Lock memory per champion; no wards or teleport crystals.
