## Strategy (≤10 lines)
Scaling economy (parent f01-07) with a rewritten spell subsystem (spells.cpp). Draft Vesper Vale, Vendetta, Marcella,
Tavi, Varg; the draft splits our side's camp groups into one home region per champion and writes it to draft memory.
Champions score groups by value per turn, claim them in shared memory, turn gold into shards at every spawn visit,
raid bosses together and fight by a square law (Σhp·Σdps), fleeing home when outnumbered.
Spells: each slot's kind, range, targets and "excludes walking" are read once from the engine (read_kit). Every turn
the ready slot with the highest value fires: damage capped at target health, +1000 for a kill, stun value = the
target's damage if it can hit us (tribute 600, recall 250). Walk-excluding casts only from where we already hit,
or when decisive, or (melee) after a chase step that did not reach. Resets follow a hit, or a ranged walk (the reset
gives the spent attack back). Fleeing: kill-shots, a stun (and a shot) on a lone chaser, melee swing + reset swing.

## Key parameters
- Camp casts: value = Σ min(dmg, hp) (clusters ×0.5) + kill 100 + 3·str + stun str·taken (×0.3 if it hits an ally).
- Fight casts: dmg + kill 1000 + recall 200; stun: tribute 600, recall 250, else power ×1.5 (×0.5 out of its reach).
  Skip a non-kill spell when a reset would finish someone (hp ≤ 2·0.9·str).
- Hit order (champions): kill (hp ≤ 0.9·str) > recalling > tributing > lowest hp.
- Shield: under camp fire (min(shield, incoming·taken·5)) or an enemy within 3; also while fleeing.
- Error 26 on a reset (Vendetta's kill gives the attack back unseen): take the free hit, retry the reset (≤3).
- Everything else as in f01-07 (score, gold weight, trips, retreat, tribute, fight ratios).

## Changes this generation
- Spell subsystem rewritten behind the same calls (spells_on_group, reset_hit, try_dash) plus fight_spells,
  strike_after_move, flee_spells, spell_dps; enemies() calls them instead of its inline spell/strike code.
- Ranged champions walk + reset + hit (chasing and farming); dash-then-hit when the dash leaves the attack.
- Walk-excluding casts block the walk afterwards (move_failed_), so no throwing moves after a cast.
- Local results, final version (v6): vs f00-03 56/80 (parent 42/80 on the same maps), vs f01-07 58/80,
  vs starter 40/40 (20 maps). 0 faults, 0 caught exceptions in the last 200 games. 01hooks B and 00contest A
  (parent's 0-gem losses) now win vs f00-03.

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
- Pre-emptive Varg shield (before the first camp hit): shielding +60%, results 30 vs 31/52, margin down. Kept the
  "under fire" rule; the shield takes the cast a splash reset would use.
- Chase dash when only two tiles short (dash + hit on a runner): 54 vs 56/80 vs f00-03. Reverted.
- Melee nuke whenever the target is ≥3 away: 43 vs 42/60, inside noise; not adopted (only after a missed chase step).
- can_attack() stays false after Vendetta's kill although the engine has the attack available: a reset then throws
  error 26 ("An available attack cannot be reset"). Hitting directly works.

## Known weaknesses
- Games are chaotic: a one-rule change flips ~15 of 80 games both ways; judge changes on ≥80 games and gem margin.
- Fleeing ranged champions without a stun (Vesper) still get run down by a melee chaser; the fight/flee ratio
  flips with what is visible each turn (fight model untouched this generation).
- Still loses 00fragment, 00road-of-riches, 01broken-curve, 01fracture to f00-03 (strategy, not spells).
- No wards, no altar locks, no teleport crystals; heal (Willow) and AoE kits are handled generically but untested.
