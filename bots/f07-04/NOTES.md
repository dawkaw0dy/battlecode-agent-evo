## Strategy (≤10 lines)
Scaling economy from f01-07 … f05-02 (unchanged in play): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home
regions per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses; gold into
shards; turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem and altar-lock layer; f05-02's
one-cast-per-turn spell scoring.
This generation (parameter tweaks, fuel): no turn is lost to a fuel overrun any more. The first turn's map analysis is
~60–90k cheaper and the first turn yields once it has run (instead of starting a turn that spills into round 2); the
route/goal search and a new raid rally goal are not started on a turn the target choice has drained. Capped turns
per 80 games: parent ~150–260 (5–10 per game, up to 87 on 01yellow-brick-road), now 0–3.

## Key parameters
- kAfterInit 100k: fuel the first turn must have left after initialise(), else it yields (its budget is 2× = 200k).
- kWalkFuel 30k: in farm(), after the first hit attempt (cheap, kept), no approach goal / route search below this
  (go() itself refuses below 25k; the goal costs up to ~40k).
- kRallyFuel 60k: a new rally goal (local box around a boss, ~30–50k, cached 10 rounds) is built only above this,
  and is put off one turn at most (rally_defer_), since mid-game raid turns reach farm() with ~50–57k.
- Unchanged gates: approach_goal fuel(60000 + 40·work) (relaxing it was tried, below), go() 25k, fill_fields
  reserves 30k / 40k, choose() every 4 rounds.
- Measured costs (release, fuel_remaining()): camp_groups() 15–38k; per camp in analyse_map ~950 (vector growth);
  a BFS ~2.9 fuel per tile (28.6k on 100×100); a 10,000-entry int16 table assign ~17k; first choose() on
  01boss-battle ~65k (43k of it the first clear_cost() pass), later ~25k; a rally goal 30–50k.

## Changes this generation
- analyse_map() takes the walking field from our spawn the champion computes anyway (home_): one BFS instead of two;
  tiles/kinds vectors reserved; d_ally/d_enemy in one pass over tiles with hoisted buffer pointers (−44k on 00ambush,
  −60k on 01boss-battle). Decision-identical.
- One tile → camp table (camp_tile_ → camp_group_/camp_index_) instead of two map-sized tables (−17k on 100×100);
  k_ health vectors reserved. Decision-identical.
- clear_cost(): threat matrix as 64-bit masks per camp (same first-64-camp truncation, same summation order);
  checked against the old n² loop in-game: 1,100 checks per champion on 01yellow-brick-road, 0 mismatches.
- First turn: `if (!live() || !fuel(kAfterInit)) return;` right after initialise(). Effect (replay .fuel files):
  00ambush/00monolith act from round 2 (was 3), 01boss-battle from round 3 with no overrun (was round 4, capped,
  round 5 lost), 01yellow-brick-road/01big-world/00road-of-riches one round earlier.
- farm(): kWalkFuel guard; rally branch: kRallyFuel guard with one-turn deferral.
- retreat(): the melee swing skips an enemy this turn's spell already killed (has_champion() first, as strike()
  does); it threw "engine error 98" once in 80 games vs f02-01 (a caught `warn:`, ~3k fuel). Checked on
  00great-wall both sides (the map where it fired): 2/2, no warnings.
- Results (tools/benchmark.py, release, all 40 maps × both sides; parent figures are its stored evo-results games,
  reproduced exactly here on 4/4 checked games):
  vs f06-05 32/80 (−97) [parent 35/80, −111]; vs f04-05 40/80 (−39) [parent 41/80, +9] (package before the
  one-turn rally deferral). Final build (before the swing fix below): vs f03-05 54/80 (+193) [parent 48/80, +130];
  vs f02-01 66/80 (+527) [parent 69/80, +597]; vs starter 79/79 valid (+999; the 80th game is the starter's own
  caught exception), rounds-to-win −2.9 ± 13.3 vs parent.
  Over 160 games 72 vs 76 wins: inside the noise (a fuel-only logging change alone moved f06-05 from 35 to 38/80;
  01yellow-brick-road flips ±900 between runs of decision-identical builds). Kept for the fuel metric: no lost turns.
- 0 engine faults, 0 `bot error:` and 0 `warn:` lines from our side in every run (one caught exception in a starter
  game, 01yellow-brick-road B, was the starter's).
- Game budget: 854 games played (counted conservatively, short profiling runs included).

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
  (f07-04: the masks are back, together with the other opening savings; the W/L difference stays inside the noise.)
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
- f07-04: relaxing approach_goal's fuel gate from 60000 + 40·work to 30000 + 30·work (the full "safest tiles" goal
  was refused ~3–8× as often as built; it really costs ~20–30 per work unit): vs f06-05 32/80 (−97) vs parent 35/80
  (−111); vs starter 79/79, rounds-to-win −1.5 ± 9.1. No measurable gain; reverted.
- f07-04: reading the health of every fighting camp in sight (not only the target's), so groups worn down by others
  look cheaper to choose(): vs f06-05 34/80 (−88), vs f04-05 39/80 (−29) against 32/80 and 40/80 without; more capped
  turns (10–13 vs 0–3 per 80 games). Reverted.
- f07-04: the raid rally guard without the one-turn limit could hold a raider still: mid-game raid turns reach farm()
  with 49–57k (15 deferrals in one 01boss-battle game).

## Known weaknesses
- Snowball maps (00swirl, 01hooks, 01starfish, 01yellow-brick-road, 00gemfight) are still decided by the first
  fights; 40–60 deaths in lost games. Fights are per champion, no shared engage call.
- The first turn still overruns its 200k on the most crowded or largest maps (01boss-battle, 01yellow-brick-road,
  01big-world, 00lanes, 00road-of-riches: initialise() 230–300k), so round 2 is lost there; camp_groups() and the
  per-camp copies dominate. The draft (1M fuel) could precompute more into draft memory.
- 01boss-battle: the only altar stands on a monolith's tile; whichever side kills the monolith first banks.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
- Spell choice is greedy per turn: no look-ahead to time a stun for when the allies' damage arrives, and no
  team-wide spell coordination (two stuns can land on the same target in one round).
