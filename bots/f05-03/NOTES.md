## Strategy (≤10 lines)
Scaling economy from f01-07/f02-02/f03-04/f04-05 (unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions
per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into shards;
turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem (in-place engine buffer reads).
This generation rewrote the endgame subsystem (src/endgame.cpp; same calls from turn(): deny(), lock_altars(),
tribute(), lock_needed()): an altar model built once (tribute tiles we can actually walk to, lock-range tiles),
lock memory per altar, banking (when, where, by recall), lock breaking, lock placement and the lock runner,
contesting enemy tributes (a channelling champion cannot act and nobody cancels a tribute), holding the only
altar's boss spawn tile, and a race rule for the last stretch to 1000.

## Key parameters
- Bank when: gems ≥ 45, walk ≤ 3, (≥ 20 and walk ≤ 10), (enemy seen and ≥ 10), idle, rounds left < walk + channel
  + 20, our tributed + bag ≥ 1000, or race (ours ≥ 880, or theirs ≥ 880 and ours ≥ 760): bag ≥ 8 or walk ≤ 8.
- Where: union of the open (not enemy-locked, not under living camps) altars' walkable tribute tiles; the route
  search picks the nearest. An enemy-locked altar within 4 (Chebyshev) is broken instead; the nearest locked one
  when all are locked. Recall when the walk exceeds spawn→altar + 10, or when no walk leads to any altar.
- Lock memory: enemy lock 200 rounds, ours 150. kLockRange 2, detour 8, runner (last pick) 60 (in turn(), unchanged).
- Contest: go for a channeller (≥ 5 gems) when reach + turns-to-kill ≤ remaining channel turns and no free enemy
  is within 7 of us or 5 of it; damage spells only (a stun would cancel the channel and free it).
- Hold: only when exactly one altar is reachable and all its tribute tiles are boss spawn tiles (01boss-battle);
  stand on it with an empty bag, step off when an ally with gems is within 2; carriers wait beside an occupied tile.
- eg_init() runs from round 6 with ≥ 85k fuel (rounds 1–4 already cap with the map analysis).

## Changes this generation
- Found with traces: (1) 00collision (mirror): altar (39,20) lies in a sealed pocket; the parent counted it as
  open, never fell back to breaking the enemy lock on the other altar and carried 500–629 gems for 600 rounds
  (434 + 629 > 1005 lost). Now only walkable tribute tiles count (altar_, home_altar_ and bank_d_ rebuilt).
  (2) 01boss-battle: the only tribute tile (24,20) is a monolith spawn tile; bags of 300–3145 gems went unbanked.
  Holding the tile keeps the monolith from respawning; vs f03-05 on that map 0/2 → 2/2 (vs parent 1/2 → 1/2, 0/2 in
  the last smoke test: the hold only helps once we kill the monolith first). (3) 01islands: a champion that dashed
  onto an island had no walk to any altar and never banked (parent too); now it recalls. 01islands A vs f03-05
  978–1008 loss → 1018–948 win. (4) Close race losses (00rows 985+19 carried vs 1006, 01islands 978+24, 00corners
  944+104 vs 1036): race rule added.
- Full runs (bench2.py, 40 maps × 2 sides, 0 faults, 0 caught exceptions in every run): vs f03-05 53, 51, 52/80
  (parent's cached 59/80 — its own notes show 45–59 for decision-identical builds); vs parent f04-05 43/80 twice;
  vs starter 80/80, mean rounds to win 1122 vs parent 1125. The final two commits (race rule, stranded recall,
  cleanup) were checked on 8+6 games only (budget). Fuel-exhausted turns back to the parent's level (407 vs 409/80
  games) after deferring eg_init.
- Game budget: 906 games played (about 6 over the 900 budget: my running count drifted; noticed at the end).

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

## Known weaknesses
- Our own tribute channel is never cancelled (turn() returns early while channelling): a big bag channelled next
  to enemies dies with the bag. The opponents do the same; the fight model does not know channellers cannot act.
- Snowball maps (00swirl, 01hooks, 01starfish, 00contest, 00gemfight) are still decided by the first fights.
- Lock memory is per champion (shared memory is full: 5 claim bytes + 3 sighting bytes).
- 01boss-battle: whoever kills the monolith first still banks; the hold only keeps it once we have it.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
