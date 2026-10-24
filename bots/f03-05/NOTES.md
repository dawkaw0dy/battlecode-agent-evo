## Strategy (≤10 lines)
Defense-first: protect our score and carried gems, deny the enemy's. Kept from the parent (f02-02): movement,
pathing, camp farming model and draft. New layer on top (lock_altars / deny / tribute_under_fire / foe_damage):
- Our altar locks block only ENEMY tributes, so every champion carries one (10 gold, rebought at each spawn
  visit) and locks every altar it comes within 8 tiles of; lineage bots skip locked altars, f00-03 must break them.
- An enemy seen channelling a tribute gets its altar locked (placing a lock cancels the channel).
- Channelling enemies cannot act: ~0 in the fight odds, and they are prey (hunted, hit and stunned first).
- Tribute only when we outlive ceil(gems/10)+1 turns of camp + enemy damage; outnumbered beside an altar we bank
  instead of running. Carriers avoid fights (need 1 + gems/50) and do not answer distant alarms.
- PvP spells from f02-01's spell module (kill-shots, stuns on chasers and channels, walk+reset+hit for ranged).
Why: 98% of the parent's deaths were PvP kills, and nothing stopped enemies from banking.

## Key parameters
- kLocks / kDeny / kPressureTribute / kLockAll / kLockRunner switches at the top of bot.cpp; kLockRange = 2,
  kLockDetour = 8 (runner 60).
- An altar counts as ours for 150 rounds after we last saw our lock on it; a detour gives up after 2·8+4 rounds.
- Lock target: enemy channelling with ≥5 gems within 9; walk to lock only if (distance − 2) ≤ channel left + 1
  and hp ≥ 35%.
- Fight odds: channelling enemy power ×0.2, recalling ×0.6; prey within 7 overrides "keep farming".
- foe_damage(T): Σ power·(T − max(0, d − max(range, 4))) over visible non-channelling enemies.

## Changes this generation
All runs bench2.py, 40 maps × both sides, 0 faults / 0 bot errors throughout.
- Parent copy vs f02-01: 36/80 (mean margin −48).
- v1 deny + pressure tribute + channel-aware odds: 52/80 (+147).  v2 + f02-01 spell module: 53/80 (+210).
- v3 + cautious carriers (need 1 + gems/50, alarms only below 15 gems): 54/80 (+230), our gems lost 9459 → 6863.
- v4 + lock every altar within 8: vs f02-01 65/80 (+429), opponent banked 49k → 37k gems over the 80 games;
  vs f00-03 70/80 (+567; parent 49/80); vs f01-07 (v6 = v4 + lock breaking) 69/80 (+556).
- v6: break an enemy lock within 4 instead of rerouting (only matters against lock users).
- v8 + lock runner (last pick, no gems, hp > 60%, before round 1600: detours up to 60 to unlocked altars and
  recalls for a new lock): vs f02-01 66/80 (+450), opponent banked 37k → 33k.
- Final = v8 + memory of enemy-locked altars (skip them; break the nearest when every altar is locked), and
  locks are broken by submit_gems at the locked altar. Final vs starter 20/20 (10 maps), vs v8 (a lock user)
  14/30 on mirror-like games, 0 warnings.

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
- Lock detours of 14 tiles + idle champions running locks to far altars (≤40): 57/80 vs 65/80 at 8 tiles.
- me.attack(altar_lock_at(a)) throws "Target must be a visible enemy or neutral camp" (the parent's lock code
  never worked); a tribute at the locked altar is what hits the lock.
- Hunting any enemy carrying ≥40 gems (prey even when far): 65/80 (+430), same as without; not kept.

## Known weaknesses
- Still loses 00eight (both sides, all three opponents: 17–20 deaths, heavy camp damage), 01elk-arena and
  01concentrated-wealth A often; fights are still per champion (square law over visible units), no grouping.
- Only the runner visits the enemy's altars, and nobody guards our locks: f00-03 breaks them with five hits. A lock-using opponent can turn the trick on us; we
  break enemy locks within 4 but otherwise route to the next altar as the parent did.
- No wards or teleport crystals.
