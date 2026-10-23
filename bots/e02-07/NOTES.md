## Strategy (≤10 lines)
Big-game hunter (immigrant, built from the starter infrastructure: main loop, distance-field pathing).
Draft: Ryth, Willow (heals), Varg (shield), Marcella, Vendetta, Heart of Ruin, Vesper Vale, Ren, Tavi, ...
Groups scored by (3*gems + gold) / (path + clear turns + overhead). Golem/monolith groups use the whole team's
damage (team_size * own dps), need total camp damage < 60% of the team HP pool, get x1.4, overhead 20; the
monolith is never started after round 1700. Cheap groups use nearby helpers and a x0.05 danger factor.
At a big group the champion stops 7 tiles away, tributes gems >= 5 (camp kills destroy gems), and waits until
3 allies (or the team) are within 5 tiles, the camp is already awake, or 30 turns pass; sleeping big camps
are never hit before that. Team target in shared memory (u16 offset 0, x1.5), own target x1.3.
Shop: Jungle emblem (health / focus for casters), then Monster emblem, then Power shards alternating with health.
Recall < 35% HP after stepping away from threats; recall to shop at >= 150 gold; bank at >= 20 gems.

## Key parameters
kStageRange 7, kMaxWait 30, kBigBonus 1.4, big overhead 20, team HP pool share 0.6, kBankGems 20,
kShopGold 150, recall 35% HP, spawn heal to 90%, shield below 85% HP.

## Changes this generation
New bot (archetype big-game-hunter). Test set vs e01-01: 00great-wall 00lost-world 00quadrants 00split
01boss-battle 00monolith 00packs 01elk-arena, both sides (16 games).
- Baseline as above: 4/16, mean -220 (wins 01boss-battle 2/2, 00split, 00packs).
- Validation: vs starter 12/12 (+843 mean); vs e00-01 1/12 (-380 mean). No faults, no `bot error:`.

## Tried and failed (keep entries from the parent's notes; add yours)
- (inherited, TOOLS.md) Starter step selection picks blocked/unreached tiles; only step to free tiles in `visited`.
- (inherited) Stale champion/camp handles throw 97/98: scan champion_tiles()/camp_tiles() + champion_at/camp_at, guard each block.
- (inherited) Casting "excludes walking" spells after a move throws; attack resets throw: resets unused.
- e02-07: e01-01's draft (Vesper/Marcella first) with the hunter logic: 4/16, -269 (worse margin).
- e02-07: Treasure emblem before Jungle/Monster: 3/16, -264.
- e02-07: Power shards of damage only (no health shards): 2/16, -364.

## Known weaknesses
- Loses clearly to e00-01/e01-01 on maps without big camps (01elk-arena -800): hunter shop (Jungle/Monster)
  earns less gold than Treasure-first economy there; no fallback build for maps without golems/monolith.
- Waiting at staging costs farming time; no wards, dashes, altar locks or deliberate PvP.
