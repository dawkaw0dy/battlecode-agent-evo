## Strategy (≤10 lines)
Scaling economy from e00-01 (camp-group value/cost targeting, Treasure -> Jungle -> Power shop, batched banking),
with a rewritten combat subsystem. Fights are judged by a Lanchester-style balance: (sum HP x sum DPS) of us plus
allies within 4 against enemies within 5 (+ half the strength of awake camps within 4); DPS = Strength + 0.3*Focus.
Engage bar 1.15, lowered by up to 0.35 for gems the enemies carry (kill = 100 gold + gems), raised by up to 0.4 for
our own carried gems. In range: fight unless clearly losing (<0.6); when losing, melee hits then steps away, ranged
kites only from enemies within 2. Out of range: back off from a lost fight when carrying >=10 gems or <60% HP; chase a
weak/rich enemy (<50% HP, >=20 gems or within 3 hits) at ratio >=1.5 for at most 6 turns, never near their spawn.
Targets in range: killable this turn first (Strength, x2 with a ready reset), then (100 + 3*gems)/HP, stunned x1.2.
One cast per turn: stun an unstunned champion, else a damage spell worth >= Strength, else attack reset + second hit.

## Key parameters
Engage bar 1.15 (-gems/120 up to 0.35, +own gems/100 up to 0.4), losing < 0.6, chase ratio 1.5, chase 6 turns then
20 rounds rest, ally radius 4, enemy radius 5, threat radius 6. Economy parameters unchanged from e00-01
(kBankGems 30, kShopGold 150, recall 35% HP, spawn heal 90%).

## Changes this generation
Combat subsystem rewritten (pick_target, fight, fight_best_target, threats, new engage() replacing the
"enemy in attack range" branch). Attack resets re-enabled, used only right after our own basic attack this turn.
Combat blocks and heal_allies guarded locally against stale-unit reads (engine error 97 after a kill).
Results (local, deterministic): vs e01-01 8/16, mean gems +120 (parent on the 6 shared maps: 6/12, -11; new 5/12, +97);
vs e00-01 8/16 incl. one parent-side fault; vs starter 10/10.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- (e02-05) Reading champion_at(tile).health() right after our attack killed the unit throws engine error 97 even when
  has_champion(tile) is true: check the handle's is_visible() first and guard each combat block locally.

## Known weaknesses
- 00lost-world (0/2 vs e01-01 and e00-01) and 00quadrants side B (-945 vs e01-01) got worse than the parent; not investigated.
- Balance ignores items/mitigation and enemy cooldown state; no dashes, wards or altar locks in combat.
- 00contest ends 0-0 (tiebreak).
