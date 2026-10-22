## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing).
Draft: Vesper Vale (33% gold), Marcella (gold per tributed gem), then Vendetta, Varg, Ryth, Tavi, Willow, Ren, ...
Every camp group is scored by (3*gems + gold) / (path distance + estimated clear turns + 8); clear time uses
own Strength + 0.2*Focus times nearby allies. Groups whose estimated camp damage exceeds 75% of current HP are
scaled down x0.05 (last resort only). Monolith groups are never targeted; sleeping golems are not woken below Strength 90.
Team target in shared memory (u16 at offset 0) gives x1.5, own target x1.3 (hysteresis). Respawn timers remembered.
Shop on spawn or dead: Treasure emblem first, then Jungle (health) emblem, then Power shards (Focus for casters).
Gems banked at >= 30, or >= 5 within 4 tiles of an altar, late game, or when they reach 1000; decision is sticky.
Recall < 35% HP (step away from threats first), recall to shop at >= 150 gold when safe; wait on spawn to 90% HP.

## Key parameters
kBankGems 30, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP, stuck skip 12 turns -> 50 rounds,
group score weights gems x3, team target x1.5, own target x1.3.

## Changes this generation
New bot (immigrant, scaling-economy archetype). Value/cost camp-group targeting with shared team target,
Treasure -> Jungle -> Power shop order, tile-based vision queries for all target scans.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).

## Known weaknesses
- 01big-world and 00lanes lose heavily (-450..-840) vs r00-01; root cause not investigated (maybe pathing to far groups
  or contested lanes). 00contest and 01the-farm end 0-0 (decided by tiebreak).
- No dash travel, no attack resets, no wards, no altar locks, no deliberate fights beyond enemies in range.
- Engine-rejected actions (units known to the engine but missing from vision masks) are treated as failed actions.
