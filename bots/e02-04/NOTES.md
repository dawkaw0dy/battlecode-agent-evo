## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing). Parent: e01-01.
Draft: Vesper Vale (33% gold), Marcella (gold per tributed gem), then Vendetta, Varg, Ryth, Tavi, Willow, Ren, ...
Every camp group is scored by (3*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 75% of current HP are
scaled down x0.05 (last resort only). Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
Team target in shared memory (u16 at offset 0) gives x1.5, own target x1.3 (hysteresis). Respawn timers remembered.
Shop on spawn or dead: Treasure emblem first, then Jungle (health) emblem, then Power shards (Focus for casters).
Gems banked at >= 20, or >= 5 within 4 tiles of an altar, late game, or at 1000. Recall < 35% HP; shop recall at 150 gold.
Combat: enemies in range first (finishable-this-hit > most carried gems > lowest HP); above 50% HP chase enemy
champions within 4 tiles whose HP <= 2 basic attacks (kill = 100 gold + their gems).

## Key parameters
kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP, stuck skip 12 turns -> 50 rounds,
group score weights gems x3, team target x1.5, own target x1.3. Chase: kChaseRadius 4, kChaseHits 2.0, own HP > 50%.
Enemy target key: health - 100000 if health <= own Strength - 20 * carried gems (lowest first).

## Changes this generation
Parent e01-01, mutation: parameter tweak, focus combat. Test set vs e00-01: 01hooks 00gem-fields 00gemfight (parent's
worst losses) + 00split 01starfish 00lanes, both sides (12 games). Parent baseline: 6/12, total margin -1189.
- Kept: enemy target choice finishable > carried gems > lowest HP (was lowest HP): 5/12, -1053 vs e00-01 (neutral);
  head-to-head vs parent 6/12, +224.
- Kept: chase nearly dead enemy champions (HP <= 2 basic attacks, within 4 tiles, own HP > 50%): 7/12, +1707 vs e00-01;
  vs parent 7/12, +1270; vs starter 7/8, +4210 (01hooks 00gem-fields 00quadrants 01big-world). No bot errors/faults.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- e01-01: Tribute before fighting when next to an unlocked altar with >= 5 gems: +761 vs +1331 (lost 00lanes B). Reverted.
- e01-01: Recall at 45% HP: 7/12 but -347 total margin (less farming time). Reverted.
- e01-01: Recall at 28% HP: 6/12, +961, below the 35% baseline. Reverted.
- e02-04: Retreat (step away) when visible enemy power (sum HP*(Str+0.5*Focus) within 6) > ours x1.0: 4/12, -809 vs e00-01
  but 1/12, -3380 head-to-head vs parent (00lanes collapsed). Ratio x1.5: 5/12, -2348. Retreat only when HP<60% or
  gems>=10: 3/12, -3114. Reverted: backing off from fights loses tempo and lets enemies farm.
- e02-04: Recall at 50% HP when an enemy champion is within 6 tiles (35% otherwise): 3/12, -1718 vs e00-01. Reverted.

## Known weaknesses
- 01hooks B and 00gemfight A still lose heavily (-750..-980) vs e00-01 and vs the parent: probably map-side economy,
  not combat. 00gem-fields B loses to the enemy Vesper picking off our champions (parent died 8 times there).
- No dash travel, no attack resets, no wards, no altar locks; chase is a straight distance-field walk (no stun use).
- Engine-rejected actions (units known to the engine but missing from vision masks) are treated as failed actions.
