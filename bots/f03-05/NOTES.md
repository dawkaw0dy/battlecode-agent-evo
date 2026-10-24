## Strategy (≤10 lines)
Defense-first: protect our score and carried gems, deny theirs. Kept from the parent (f02-02): movement, pathing,
camp farming model and draft. New strategy layer on top (deny / tribute_under_fire / foe_damage in bot.cpp):
- Every champion carries an altar lock (10 gold, bought on each spawn visit). An enemy seen channelling a tribute
  within 9 tiles gets its altar locked (placing a lock cancels the channel), walking into lock range (2) when we
  arrive before its channel ends.
- Channelling enemies cannot act: they count ~0 in the fight odds and become prey (hunted even when far, hit first).
- Tribute timing: a tribute starts only when we outlive ceil(gems/10)+1 turns of camp and enemy damage
  (enemies walk one tile a turn into reach 4); when outnumbered next to an altar we bank instead of running.
- PvP spells from f02-01's spell module (kill-shots, stuns on chasers and channels, walk+reset+hit for ranged).
Why it beats the parent: 98% of the parent's deaths were PvP kills (1053 of 1079 in 80 games vs f02-01).

## Key parameters
- kLocks / kDeny / kPressureTribute switches at the top of bot.cpp; kLockRange = 2.
- Lock target: enemy channelling with ≥5 gems within 9; walk to lock only if (distance − 2) ≤ channel left + 1
  and hp ≥ 35%.
- Fight odds: channelling enemy power ×0.2, recalling ×0.6; prey within 7 overrides "keep farming".
- foe_damage(T): Σ power·(T − max(0, d − max(range, 4))) over visible non-channelling enemies.

## Changes this generation
- v1 (defense layer only, parent camps and spells): vs f02-01 52/80 (+147 mean margin); parent 36/80 (−48).
  Opponent gems lost on death 7240 → 10112 over the 80 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: debug prints (fuel) alone moved 00lost-world B from 1069 to 524 rounds. Judge changes on
  full 80-game runs, never on single maps; rounds-to-win vs starter is a less noisy farming-speed metric.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.

## Known weaknesses
- Fights are still per champion (square law over visible units); no grouping before contests.
- No wards or teleport crystals.
