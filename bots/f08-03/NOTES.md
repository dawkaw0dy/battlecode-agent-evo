## Strategy (≤10 lines)
Scaling economy from f01-07 … f06-05 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f05-03's endgame (altars, locks, banking); f06-05's free shots.
New: a map plan made at draft time and stored in draft memory (bytes 256+: 'P', flags, one byte per group), so every
champion plays the map the same way. It classifies the map (contested gem share, boss gem share, size, altar count)
and the groups (contested or not). The rule kept: altar locks are worth walking for only where the enemy has few
altars to choose from (it walks past a locked altar to an open one). With ≤ 2 altars the last pick is the lock
runner and everyone detours up to 8 to lock altars, as before; with 3+ altars nobody runs or detours for locks
(locks are still dropped on altars within reach and on enemy tributes), so the last pick farms (Varg's gold ~2×).

## Key parameters
- Map plan (plan.cpp): kRunnerAltars 2 (most altars for the runner and lock detours); contested group = walking
  distances from the two spawns within kContestGap 4; contest map = ≥ kContestShare 0.6 of gem income (gems/respawn)
  in contested groups; boss map = ≥ kBossShare 0.4 held by bosses; large map = ≥ kBigMap 3000 tiles.
- Switches left off (measured worse or neutral, see below): kPackContest, kBossPlan, kBSpells, kFirstStrike,
  kFightBuild, kLockDetourAll.
- Unchanged from the parent: kSimTurns 10, kEngageBias −150, kChaseTurns 8, kFreeShots on, kLockDetour 8, runner
  detour 60.

## Changes this generation
- f08-03 (parameter tweak, focus: spells): **no change kept**. The source is byte-identical to f07-02, so every game
  replays the parent's exactly (games are deterministic). Five spell tweaks were measured (below, under Tried and
  failed); none beat the parent, so none were kept.
- Method: candidate vs the parent on all 40 maps, both sides (the parent's mirror is exactly 40/80, so that baseline
  costs no games), then vs f05-02 (hall of fame) on all 40 maps, against the parent's 58/80 (+274 mean gems) there.
- Inherited from f07-02: map plan in draft memory, lock-runner rule by altar count; see Strategy.
- Game budget: 820 games (9 candidate runs of 80, one 80-game parent baseline vs f05-02, one 20-game check).
- Final build: 0 engine faults and 0 caught exceptions in every run (the parent's own run vs f05-02 is the final bot's).

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

- (f08-03) Spell results are dominated by fuel chaos. With every new switch compiled off, one leftover host call
  (`can_move()`) and slightly different code changed the fuel profile, and only 3 of 10 maps replayed the parent's games.
  So a variant's measured result is mostly a fresh draw. Across the 5 variants: vs f05-02 44–53/80 (parent 58),
  vs the parent 35–42/80. Judge spell tweaks on several opponents; the parent's 58/80 vs f05-02 is a high draw.
- (f08-03) Exclusive camp spells (Tavi stun, Marcella damage) only from a tile in basic range (cast + hit together
  instead of losing a step at range 4): 35/80 vs parent (−62 mean gems).
- (f08-03) Camp spell target = the basic attacks' focus target (landed damage ×20, no overkill, + str/hp ×1e4, +50 if
  it reaches us) instead of min(hp,400)+str: 37/80 vs parent (+0); clean on the parent source 53/80 vs f05-02 (+236).
- (f08-03) Varg shields as soon as camps hit him (health gate 0.9 → none): 42/80 vs parent (+30), 48/80 vs f05-02 (+149).
- (f08-03) No travel dash for 10 rounds after an enemy sighting (keep it for escapes; fights taken still dash):
  41/80 vs parent (−16), 44/80 vs f05-02 (+132).
- (f08-03) Retreating Tavi does not stop to stun a chaser within 3 (still casts for kills and cancelled channels):
  39/80 vs parent (−66), 51/80 vs f05-02 (+171).

## Known weaknesses
- Most deaths still come in retreats and in 1v1s the 10-turn simulation cannot resolve (it engages at small negative
  values, then flees too late); horizon, hysteresis and bias changes measured neutral or worse in past generations.
- Snowball maps (00swirl, 01hooks, 01starfish, 00contest, 00gemfight, 00stampede, 01gold-rush) are decided by the
  first fights; mirror games are often won by whichever side strikes first in the first contact.
- Boss raids: raid size counts only visible allies and targets alternate between bosses; monoliths die late (~950+).
- The altar-count rule ignores where the altars are (an enemy-side cluster of 3 altars counts as "many").
- Our own tribute channel is never cancelled; lock memory is per champion; ally positions outside vision unknown.
- No wards or teleport crystals.
