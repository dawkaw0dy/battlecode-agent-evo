## Strategy (≤10 lines)
Scaling economy from f01-07 … f04-05 (unchanged outside spells): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg;
home regions per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses;
gold into shards; turn-by-turn focus-fire simulation decides fights; f04-05's fuel subsystem and altar-lock layer.
This generation rewrote the spells subsystem (spells.cpp, same calls: spells_on_group / reset_hit / fight_spells,
plus reset_attack() used by strike, the ranged chase and lock breaking). One successful cast per turn, so every
ready slot's option is scored on one scale (damage that lands after shields, kills, a stunned attacker's prevented
hit, a cancelled tribute or recall, a shield's absorbed damage) and only the best one is cast. Resets fire only when
a target is left in reach. "Excludes walking" spells are skipped after a walk and forbid walking after the cast.

## Key parameters
- Camps: value = min(hp, dmg); ×0.3 when our basic attack kills it this turn anyway; +40 + 3·str for a kill;
  stun (cc) + str·taken when that camp hits one of us; area spells add every camp in the radius; cast only at ≥ 30.
- Shield on camps: min(shield, threat·taken·4), ×1.5 below half health; threat = incoming_, or (before the group
  wakes) the strength of its camps that reach us when we hit this turn. In fights: min(shield, 2·threat_at+camps).
- Enemies: predicted damage via ability.damage_amount(target); kill +1000 + value; kill with the following basic
  attack +600 + value/2; stun + (sustain + burst/2), ×0.3 when the target reaches none of us next turn;
  tribute cancelled by a stun +300 + 5·gems; recall cancelled +150. Retreating, rooting spells only for a kill,
  a stun on a chaser (within its reach + 1) or a cancelled channel.
- kHoldRange = 0 (off): holding stuns/nukes while an enemy is within 7 measured worse.

## Changes this generation
- spells.cpp replaces ability_range / spells_on_group / reset_hit / fight_spells / the inline resets in bot.cpp.
- All runs tools/benchmark.py, release, both sides; 900 games, 0 engine faults, 0 `bot error:` and 0 `warn:` lines
  from our side (one caught exception in a starter game was the starter's).
- v1 (hold at 7, ranged walk→reset→attack while farming): vs f04-05 39/80 (−57), vs f03-05 44/80 (+102).
- v2 hold off: vs f03-05 49/80 (+159), vs f02-01 73/80 (+606; parent 67/80, +546).
- v3 + stun value only when the target threatens us: starter 79/79 valid (+994), f00-03 70/80 (+627),
  f01-07 73/80 (+660), f03-05 48/80 (+120), f04-05 38/80 (−93).
- v4 (submitted) = v3 with the farming walk→reset reverted (parent behaviour): vs f03-05 41/70 on the first 35 maps
  (+95). Same 35 maps: parent 50/70 (+264), v1 36, v2 41, v3 41. Parent vs f03-05 full: 59/80 (+300, reproduced).
- Worst maps (01boss-battle, 00gemfight, 00packs, both sides): vs f04-03 new 3/6, parent 4/6; vs f04-04 new 2/6,
  parent 1/6 (01boss-battle still lost on both sides vs f04-04).

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
