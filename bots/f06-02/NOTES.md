## Strategy (≤10 lines)
Parent f05-02 unchanged except one camp-farming rule (parameter tweak, focus: camps): draft Vesper Vale, Vendetta,
Marcella, Tavi, Varg; home regions per champion; groups scored by value per champion-turn with a camp damage model;
raids for bosses; gold into shards; turn-by-turn focus-fire simulation decides fights; one-cast-per-turn spell scoring.
New: a boss (monolith, golem) is raced for. A raid's per-champion rate splits the reward by n, so the model kept
boss raids at 2–3 champions, slow clears that the enemy often finished first; each raider beyond the first now adds
kBossRace = 15% to a boss raid's score, so boss raids are planned bigger and finish sooner.

## Key parameters
- kBossRace = 0.15 (new): boss raid score × (1 + 0.15·(n − 1)). Only boss groups; solo/non-boss plans unchanged.
- Gold weight 0.35 (Vesper Vale 0.5) × max(0.4, 1 − round/1500); kRallyRounds 35; kGatherTurns 10; kShareTurns 15;
  kCaptureTurns 6; kTripTurns 8; solo needs cap ≥ 5·peak and ≤ 4 trips; raids need cap·n ≥ 3·peak and ≤ 3 trips.
- Camps: value = min(hp, dmg); ×0.3 when our basic attack kills it this turn anyway; +40 + 3·str for a kill;
  stun (cc) + str·taken when that camp hits one of us; area spells add every camp in the radius; cast only at ≥ 30.
- Enemy sighting within 7 of a group (≤ 24 rounds old): ×0.5; other champion's home ×0.6, shared ×0.8.

## Changes this generation
Method: the parent's exact per-game results (same sources, same engine) are in evo/run2/cache.jsonl, so every run
is compared game by game against them without replaying the parent. Changes that touch only boss groups change
only the 18 maps with a monolith or golem (checked: every non-boss game was identical), so boss changes were tested
on those 18 maps, both sides. Screen = vs f04-05 on the odd-numbered maps + vs f03-05 on the even ones (80 games);
complement = the other halves (80). 839 games in total, 0 engine faults, 0 `bot error:` / `warn:` lines on our side.
- KEPT kBossRace 0.15. Boss maps vs f04-05 + f03-05 (72): 41W vs parent 36W, mean margin +99 vs +34. Vs the parent
  (mirror, boss maps, 36): 17W (−1 from 18). Vs f02-01 boss maps (36): 34W vs 33W. Vs starter boss maps: 36/36.
  Screen 45/80 (parent 45); with the complement 94/160 vs 89/160, the gain all on boss maps.
- Reverted kBossRace 0.3: boss maps 39/72 (+3), margin +59 (0.15 better on both).
- Reverted kRallyRounds 35 → 20 (with 0.15): boss maps 37/72, margin +56.
- Reverted no enemy-sighting penalty on boss groups (with 0.15): boss maps 40/72, +81 (no gain over 0.15 alone).
- Reverted gold weight 0.35/0.5 → 0.5/0.65 (with 0.15): screen 50/80 (+5 over 0.15 alone), complement 42/80 (−7
  against 0.15 alone), so −2 over 160 games; margin +117/+101. With it: mirror boss maps 21/36, f02-01 31/36 (−2).
- Reverted gold weight 0.65/0.8: screen 48/80, margin +45.
- Reverted weighing raids for non-boss groups whenever a solo clear needs ≥ 1 trip home: screen 48/80, +53
  (vs 50/80 with the gold change it was stacked on).
- Worst losses from the task: 01boss-battle A vs f05-05 and 00collision A vs f05-03/f04-05 are chaotic: the same
  build with debug prints only turned 00collision A vs f04-05 from 233–1014 into 396–1046 and 1017–613. 00contest
  (one monolith per side, 0 gems in 2000 rounds vs f03-05/f04-05 in the parent) went 0–600 → 800–0 (A) vs f04-05.

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
- Boss race 0.3 per raider, rally wait 20 rounds, no sighting penalty on bosses: none beat race 0.15 on boss maps.
- Gold weight up (0.5/0.65 or 0.65/0.8): +5 on one 80-game half, −7 on the other; neutral at best.
- Raids for non-boss groups whenever solo needs a trip home: 48/80 vs 50/80; raiders gather and split small rewards.

## Known weaknesses
- Snowball maps (00swirl, 01hooks, 01starfish, 01yellow-brick-road, 00gemfight) are still decided by the first
  fights; 40–60 deaths in lost games. Fights are per champion, no shared engage call.
- Maps with many camps (01yellow-brick-road, 01boss-battle: 80 camps) still cap a few turns: clear_cost() is
  O(n²) per group (~20k for a 20-camp group) and initialisation ~300k (rounds 0–1 lost).
- 01boss-battle: the only altar stands on a monolith's tile; whichever side kills the monolith first banks.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
- Spell choice is greedy per turn: no look-ahead to time a stun for when the allies' damage arrives, and no
  team-wide spell coordination (two stuns can land on the same target in one round).
- Boss evidence is +5 wins over 144 boss games (+5 vs f04-05/f03-05, −1 mirror, +1 f02-01): small next to the noise.
- 00contest and 01boss-battle are still decided by who kills the monolith first; raids still wait up to 35 rounds.
