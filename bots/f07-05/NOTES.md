## Strategy (≤10 lines)
Scaling economy from f01-07 … f05-02 (unchanged outside fuel.cpp): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg;
home regions per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses;
gold into shards; turn-by-turn focus-fire simulation decides fights; f05-02's spells, f04-05's altar-lock layer.
This generation rewrote the fuel subsystem (fuel.cpp, same calls: view/invalidate/free_tile, initialise, observe,
rebuild_camp_block, eta, fill_fields, go/step_on/try_dash, recall_or_walk). Initialisation is a queue of costed
steps that each wait for a turn with room for them, so no champion runs dry in its first turns; etas are exact
from the first choice; per-turn bookkeeping reads precomputed raw bit positions. The fuel saved is spent on
movement: equally short steps are told apart by danger (awake camps in reach, enemies within 4), and dashers keep
the dash for fights instead of travel while enemies are about.

## Key parameters
- Step costs (fuel.cpp step_cost): team 8k; altars 6k + 3·reachable tiles + 1.5k·altars; field order 2k + 250·groups;
  threat tables 2k + 90·(fighting camp, camp) pairs + 60·camps; kStepMargin 6k on top of each.
- Prewarm: each group's clear cost needs 6k + 1k + 40·n²; the from-here field 6·reachable + 6k; the first choice
  ahead only when 15k + 1.2k·groups + 55k (kGoalFuel) is left, then its rally ring or approach tiles are cached.
- kTurnFuel 75k: the first real turn is played only with this much left after initialisation, else next round.
- eta: own group field (the target's is filled on first use when 40k + 6·reachable is left); else the from-here
  field while ≥ 3 groups lack a field (kHereMin); else cheb·5/4 + 2.
- Safe steps: danger = Σ str of awake fighting camps (within 7 of us) whose range covers the tile + 150 per enemy
  champion within 4 (seen within 6 of us); only breaks ties between steps of equal field distance.
- Hold dash: no travel dash while an enemy (off their spawn) is in sight within 12 or was seen ≤ 8 rounds ago
  (kHoldRounds), unless the goal surrounds a visible enemy (a chase).
- A move into a tile an unseen unit holds (returns false) retries once on the next best tile.

## Changes this generation
- Measured (fuel_remaining(), release): an allocation ~200 fuel, a vector reserve ~200, a bitboard ~200 (+ its clear),
  memset/memcpy 1 fuel per byte (bulk memory is charged per byte), a bounds-checked tile read 40–70, camp_groups()
  36k, static SDK initialisation ~22k of the first turn's 200k. Parent initialisation: ~450k on 01boss-battle
  (analyse_map 118k, of it 68k vector growth; build_threats ~50k), so every five-champion map lost rounds 1–2 and big
  ones rounds 1–4 (fuelExhausted events: 385 per 80 games vs f06-05).
- New init: raw sdk_map_camp_layout into one buffer, vectors sized up front, one walking field from our spawn gives
  home_, d_ally and (mirrored by the map symmetry; verified on the spawns, BFS fallback) d_enemy; GroupInfo checked
  identical to analyse_map() on 4 maps. Unreached field tiles read 65535 (checked).
- observe(): camp tiles as precomputed raw keys, 10–15k → 6–8k per turn on 01yellow-brick-road; camp-block board
  rewritten only for groups whose knowledge changed (10k → ~1k).
- Results, all tools/benchmark.py, release, both sides, 0 faults, 0 `bot error:`/`warn:` lines from our side:
  vs f06-05 (all 40 maps): parent 35/80 (−111); v1 (lean init + yield) 39/80 (−72); v2 (+ raw keys, incremental
  board) 40/80 (−34); v3 (costed step queue + prewarm, exhaustions 385 → 35) 39/80 (−49); v4 (+ safe steps)
  44/80 (+95); v5 (+ hold dash, submitted) 52/80 (+79).
- v5 vs parent f05-02 (all maps) 42/80 (+59) (v4: 42/80, +77); vs f04-05 on 20 maps (odd ones) parent 21/40 (+1),
  v5 24/40 (+67); vs starter 10 maps 20/20; vs f02-01 20 maps 39/40 (+694); vs f03-05 20 maps 18/40 (+51, no paired
  parent run: budget).
- v4 → v5 vs f06-05 per game: deaths 27.8 (parent) → 25.7 → 26.1, kills 22.5 → 25.7 → 26.5, gems collected 824 → 891
  → 896.
- Game budget: 866 games played (counted conservatively, partial profiling games included).

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
  (This generation: read the field in place instead; no export needed.)
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
- Decision-identical fuel refinements, reverted (commits 910cc45, 40ebae0): clear_cost() as 64-bit threat masks
  (a 20-camp group cost ~20k per evaluation, ~5k after), camp tiles' raw words precomputed for observe and the
  camp-block board, initialisation 300k → 225k on 80-camp maps (reserved vectors, single-pass threat masks). They
  removed the last capped turns on 01yellow-brick-road (0 in 2000 rounds), but only shift fuel thresholds and opening
  timing, which reshuffles chaotic games: vs f03-05 45/80 (+135) against 59/80 (+300) before. Games are
  deterministic, so the measured version was kept.
- 00gemfight is lost vs the parent in the mirror by ~1000 both sides with the rewrite: the first contested fight
  (round 35–95) decides it and our champions now start 2–3 rounds earlier; not a fuel bug.
- Host reads are not slower than reads in place for a single tile (~30 fuel each; a bounds-checked raw read costs
  as much): raw reads pay off only where they replace exports, copies, allocations or exceptions.
- Holding stuns and nukes off camps while an enemy champion is within 7 (kHoldRange 7): 44/80 vs f03-05 against
  49/80 without. The spell spent on camps is worth more than the burst kept for a fight that may not come.
- A ranged champion's walk → attack reset → attack while farming: same damage as the reset after a standing hit,
  but Vesper Vale's kills fell 824 → ~680 per 80 games vs f03-05 in every run with it. Reverted; kills stayed ~8/game
  without it (v4), so the drop may be chaos rather than the reset.
- Every version of this rewrite sits at 36–41/70 on the same 35 maps vs f03-05 where the parent scores 50/70; the
  parent's own notes saw decision-identical changes move it 59 → 45/80, so the parent's figure is a high draw.
  Marcella's camp damage is ~10% lower than the parent's in all four versions (cause not found: candidates are
  the kill bonus that roots her at range 4 on low camps, and the parent's "wasted" resets, which left an enhanced
  (stunning) attack buff for her next hit).
- (f07-05) Ending the initialising turn when < 100k is left (v2) without warming the first choice: on five-champion
  maps the first real turn still overran (first choice ~60–80k: every group's clear cost), and a suspended turn
  wastes its remainder on the next round too, so those maps started a round later than the parent. Fixed by the
  costed step queue + prewarm (v3).
- (f07-05) A walking field from our tile for every unfilled group's eta, recomputed on every new tile: while
  travelling to a target without its own field it ran every turn (up to 58k on 01yellow-brick-road) and added
  capped turns there. Fill the target's own field on first use instead; use the from-here field only while ≥ 3
  groups lack one.
- (f07-05) Not fixable here: the first choice on 01boss-battle costs ~33k even with every clear cost cached
  (bot.cpp's per-group loop) and a raid's rally ring 30–45k, so its first real turn is ~95k; on 01yellow-brick-road
  choose() costs 50–60k every 4 turns and still caps ~20 turns per game.

## Known weaknesses
- Snowball maps (00gemfight, 01hooks, 01starfish, 01gold-rush, 00collision B) are still decided by the first fights.
- 01boss-battle: the only altar stands on a monolith's tile; whichever side kills the monolith first banks. Its
  initialisation still takes rounds 1–3 (~280k of steps; the first choice and the rally ring cannot be split).
- vs f03-05 18/40 on 20 maps: not compared against the parent on the same maps (budget); may be a regression there.
- Safe steps and the dash hold were tuned against one opponent (f06-05); kHoldRounds and the 150 per enemy are untested.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals. Spell choice is greedy per turn.
