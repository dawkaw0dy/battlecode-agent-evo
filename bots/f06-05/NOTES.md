## Strategy (≤10 lines)
Scaling economy from f01-07 … f05-03 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem; f05-03's endgame (altars, locks, banking).
This generation (parameter tweaks, combat): retreating ranged champions take a free shot when nobody can hit the
tile they stand on next turn, and a chase is kept up for 8 approach turns without a hit (was 5).

## Key parameters
- Fight: kSimTurns 10 (simulation horizon), kEngageBias −150, kStick 0, value of a death 150 + 2·gems, damage term
  60·(their lost − our lost health fraction); engage needs value > bias and not (we die and they don't).
- Chase: kChaseTurns 8 approach turns without a landed hit, then no chase for 15 rounds; hunt only enemies within
  attack range + 3 unless the simulation expects a kill.
- Retreat: kFreeShots on (ranged, can attack, has not moved, no camp fire, threat_at(here) == 0 → strike instead
  of stepping); recall when every enemy is ≥ 7 away; kAllyPull 0 (off); kCaughtFights off; kDoomedFights off.
- Help an ally's sighting: age ≤ 4, within 14, our hp > 60 %, bag < 30.

## Changes this generation
- Baseline for free: the parent's games in evo-results (results/r05-*.json, same source digest) are deterministic and
  reproduced exactly here (8/8 games). Screening set S1 = every other map alphabetically (20 maps × 2 sides), S2 = the
  rest; parent on S1/S2 vs f05-02 17/40 (−15) and 17/40 (−85), on S1 vs f04-05 21/40 (−47), all 80 vs f03-05 57/80.
- Trace (00stampede vs f05-02, lost): 31 of 42 deaths came while retreating (last decision "flee"), 11 while engaged;
  in the won mirror side 17 vs 14. Retreating melee is chased and hit every turn; ranged retreaters never shoot.
- Kept 1, kFreeShots: S1 vs f05-02 21/40 (−14), S2 18/40 (−143), S1 vs f04-05 26/40 (+124): 65/120 vs parent 55/120.
- Kept 2, kChaseTurns 5 → 8 (on top of 1): S1 vs f05-02 23/40 (+109), S2 22/40 (+113), S1 vs f04-05 25/40 (+149):
  70/120 vs 55/120 (vs f05-02 45/80 vs parent 34/80).
- Final build: vs f03-05 all maps 57/80 (+281; parent 57/80, +281, 79 of 80 games differ), vs starter 20/20 on 10 maps.
  0 faults, 0 `bot error:` and 0 `warn:` lines from our side in all checked runs.
- Game budget: 792 games played (counted conservatively).

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

## Known weaknesses
- Most deaths still come in retreats: same-speed melee chasers hit a fleeing champion every turn; fighting back
  (kDoomedFights, kCaughtFights) measured worse, so the fix has to be engaging less or earlier exits, not trading.
- Our own tribute channel is never cancelled (turn() returns early while channelling).
- Snowball maps (00swirl, 01hooks, 01starfish, 00contest, 00gemfight, 00stampede) are decided by the first fights.
- Lock memory is per champion (shared memory is full: 5 claim bytes + 3 sighting bytes).
- 01boss-battle: whoever kills the monolith first still banks; the hold only keeps it once we have it.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
