## Strategy (≤10 lines)
Scaling economy (grandparent f01-07, parent f02-01) with the parent's spell subsystem. Draft Vesper Vale, Vendetta,
Marcella, Tavi, Varg; the draft splits our side's camp groups into one home region per champion and writes it to
draft memory. Champions score groups by value per turn, claim them in shared memory, turn gold into shards at every
spawn visit, raid bosses together and fight by a square law (Σhp·Σdps), fleeing home when outnumbered.
This generation (combat parameter tweaks): chase less. No chasing enemies near their spawn, no chasing below half
health, hunting enemies more than 5 tiles away only at 2.5:1 odds; a healthy champion in a near-even one-on-one
stands and fights instead of running.

## Key parameters
- kFoeZone = 4: enemies within 4 tiles of their spawn are fought if in reach but never chased (keep farming).
- kChaseHp = 0.5: with the odds in our favour but no hit this turn, chase only above 50% health, else walk home.
- kStandRatio = 0.8: one visible enemy, our health ≥ 90%, ratio ≥ 0.8·need → fight instead of fleeing.
- kHuntRatio = 2.5: odds needed to engage when the nearest enemy is more than 5 tiles away (was 1.6).
- Unchanged: need = 1 + min(0.5, gems/100); ×1.5 within 3 of home; kChaseTurns = 5 (no-chase 15 rounds after);
  help an ally's sighting within 10 tiles (age ≤ 4, hp > 60%); power = 1.1·str + 0.25·focus.
- Spells as in f02-01 (see its notes): kill +1000, stun by threat, resets after hits.

## Changes this generation
Measured on a 20-map combat-heavy subset (00contest 00fragment 00swirl 00stampede 00split 01gold-rush 01elk-arena
01concentrated-wealth 01shortcut 00reverse-spiral 01hooks 00gemfight 00eight 01starfish 00packs 00rushbait-v2
00lost-world 01yellow-brick-road 00gem-fields 01the-farm), both sides, vs f00-03 + f01-07 (80 games per step):
- Parent: 26 + 25 = 51/80.
- KEPT T1 no chase into the enemy spawn zone (radius 4): 31 + 26 = 57/80, margin +364/+117 (parent +184/+123).
- KEPT T3 chase only above 50% health (was 30%): 27 + 31 = 58/80, margin +250/+336.
- KEPT T4 stand in a healthy near-even one-on-one: 28 + 31 = 59/80, margin +271/+374.
- KEPT T6 hunt ratio 1.6 → 2.5: 28 + 32 = 60/80, margin +375/+416.
- Full 40 maps, final version: vs f00-03 55/80 (+289), vs f01-07 57/80 (+263); parent 56/80 (+204), 58/80 (+213).
  On the 20 held-out maps alone the final version is worse (27/40 and 25/40 vs parent 30/40 and 33/40), and
  vs f02-01 on 10 held-out maps 8/20. Net: equal wins with the parent, higher mean margin; the subset gains look
  partly like noise and selection. 0 faults, 0 caught exceptions in all 740 games of these versions.
- 900 games used in total (budget exhausted).

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
- f03-01 T2: no-chase zone radius 6 instead of 4: 25 + 25 = 50/80 vs 57/80. Reverted to 4.
- f03-01 T5: answer allies' sightings within 15 tiles instead of 10: 23 + 24 = 47/80 vs 59/80, margins down vs
  both opponents. Pulling champions off their camps to fights costs more than the extra numbers win.
- f03-01 T7: give up a chase after 3 hitless approach turns instead of 5: 23 + 27 = 50/80 vs 60/80. Reverted.
- Noise: each one-rule change flipped 15–25 of 80 games both ways; a 20-map subset that was tuned on did not
  predict the other 20 maps. Judge on all maps (≥160 games) or not at all.

## Known weaknesses
- Combat losses: 00contest and 00fragment vs f00-03 (30+ of our champion deaths vs ~10 of theirs); f00-03's
  Vesper Vale wins the Vesper mirror and snowballs on kill gold. About a quarter of our champion deaths happen
  within 8 tiles of the enemy spawn (stats-file death events).
- The fight/flee ratio still counts only visible champions within 9 Chebyshev tiles, so enemies across walls or
  holes count, and enemies standing on their own spawn are ignored even when they are about to step off.
- Still loses 00quadrants, 00road-of-riches, 01broken-curve, 01fracture to f00-03 (strategy, not combat).
- No wards, no altar locks, no teleport crystals.
