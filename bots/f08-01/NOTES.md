## Strategy (≤10 lines)
Scaling economy from f01-07 … f07-02 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
f05-03's endgame (altars, locks, banking); f07-02's draft-time map plan (contest/boss/runner flags).
Combat (this generation, rewritten in combat.cpp behind the same enemies() interface) is built on the turn order:
champions act interleaved by draft index, so an enemy of another index acts once between our turns while our
same-index counterpart acts twice in a row or not at all (has_completed_turn() + last round's flag). Threats on
a tile count each enemy's real actions before our next turn; retreats step to the tile that takes the fewest hits
and keep away from an enemy that will act twice after our next turn; a retreating ranged champion takes a free shot
only when nobody can answer and it can still slip out of reach before the enemy's double turn. Engage/retreat is
still a 10-round focus-fire simulation against bias −150.

## Key parameters
- combat.cpp: kHorizon 10, kEngageBias −150, kChaseTurns 8, kFreeShots on (with the escape check).
- Switches (A/B tested, see below): kTurnOrder on, kNewStrike on (keep a reset when nothing is left in range,
  +2 for enemies about to act); off: kSeqSim, kRunCost, kDuelTactics, kNewSpells, kNewRetreat.
- Unchanged from the parent: map plan, economy, endgame, shopping.

## Changes this generation
- Mutation: subsystem rewrite of combat. All of the parent's fight code (read_fighter, simulate, camp_fire,
  threat_at, retreat, strike, fight_spells, enemies) moved to combat.cpp and rewritten; bot.cpp keeps the rest.
- New facts (probes): turn order is interleaved by draft index with the same-index pair swapping every round
  (0 mismatches over a whole game predicting has_completed_turn from champions(team) order and round parity);
  has_completed_turn() works on visible enemies.
- Measurement: parent vs parent is exactly 20/40 by side symmetry, so the all-switches-off build (parent
  behaviour in the new code) was checked: 18/40 vs f07-02. Win counts swing ±3/40 between near-identical
  builds; the kill/death and damage ratios over ~1700 deaths per 20 games were the steadier signal.
- Final configuration (turn-order threats + escape-checked free shots + strike tweaks):
  vs f07-02: 11/20 on 10 fight-heavy maps (K/D 1.12, damage ratio 1.11; all-off 12/20, 0.99, 1.03) and 19/40 on
  the other 20 maps (K/D 1.10, damage 1.15, margin +95) = 30/60.
  vs f06-05 on 20 maps: 22/40 (parent 23/40), K/D 0.97 (parent 0.88), damage 1.04 (0.97).
  vs f05-02 on the same maps: 21/40 (parent 27/40), K/D 1.02 (parent 1.27), damage 1.06 (1.18).
  vs f04-05: 20/40 (parent 19/40). vs f03-05 13/20, f00-03 9/10, f01-07 10/10, starter 9/9.
  0 `bot error:` lines from our side in all scanned runs (300 games); two caught `warn: flee move` lines (a turn
  resumed after running out of fuel), now guarded by live() before the retreat step.
- Game budget: 898 games played (all tools).

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
- (f08-01) Engage against the estimated cost of running (melee chasers hit all the way home, ranged ones a shot or
  two) instead of a fixed bias: 10/40 vs f06-05 (parent 23/40), deaths 1316 vs 759: hopeless fights get taken
  whenever running looks fatal. The parent's doomed-fight finding holds.
- (f08-01) Fight simulated action by action in turn order (kills land before the victim acts): a mirror duel then
  reads won and lost on alternate rounds (whoever holds the double turn wins), so champions alternated engage and
  retreat. Averaging both orders fixes the flip-flop; still neutral: 9/20 (K/D 0.90) alone, 11/20 (K/D 1.11 vs
  1.12) on top of the final build. Horizon 16 instead of 10 was in the early (confounded) runs.
- (f08-01) Move-by-move tactics (score staying and each neighbour tile: damage now and next turn vs λ·hits taken):
  one-ply for every fight was far too passive (damage dealt 2.33M vs the parent's 2.83M over 40 games; champions
  stood off while enemies farmed or drifted away from a dying enemy). Restricted to duels with our counterpart and
  given a two-action lookahead when we hold the double: K/D 0.785 vs 0.847 and 0.904 vs 1.152 in two runs. Off.
- (f08-01) Free shots whenever no enemy acts before our next turn: K/D 0.847 vs 1.152 once the shot also needs a
  tile out of reach of the enemy's following double turn (the shot cost the tile of distance that turn needed).
- (f08-01) Spell rules (stun value by the target's actions; melee holds rooting spells until adjacent): K/D 0.837,
  damage 0.952 vs 1.152 / 1.089: Marcella's held damage spell is damage lost.
- (f08-01) Retreat rules (recall only when nobody is within 5 + reach; +3 per tile within 2 of a melee enemy):
  K/D 1.062 vs 1.152, 11/20 vs 13/20.
- (f08-01) Engage bias −200: 6/20, K/D 0.86. −100: 15/20 (K/D 1.23) on the fight maps but 16/40 (K/D 0.93) vs 19/40
  at −150 on the other maps: noise; kept −150.

## Known weaknesses
- Against f05-02 (parent B's spells) the new threat model fights worse than the parent (K/D 1.02 vs 1.27 on the
  same 40 games, 21 vs 27 wins); against f06-05 and the parent it fights better. Cause not found.
- Mirror duels are still decided by who holds the double turn first; the duel tactics written for it measured worse.
- Most deaths still come in retreats from same-speed melee chasers (they hit every turn); fighting back measured
  worse twice (f06-05's "caught" rule, this generation's run cost).
- Snowball maps (00swirl, 01starfish, 00contest, 00gemfight, 00stampede, 01gold-rush) are decided by the first fights.
- No team coordination in fights beyond the shared sighting; ally positions outside vision unknown.
- No wards or teleport crystals.
