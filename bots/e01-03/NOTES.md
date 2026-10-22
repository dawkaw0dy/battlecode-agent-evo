## Strategy (≤10 lines)
Split farm, built from the starter infrastructure (main loop, distance-field pathing).
Draft: Vendetta, Vesper Vale, Ryth, Marcella, Varg, Tavi, Heart of Ruin, ... (self-sufficient farmers).
At draft, camp groups are split into one region per pick: seeds by farthest-point sampling over non-monolith
groups on our half (Chebyshev), every group joins its nearest seed; each region gets its nearest altar.
Plan is written to draft memory (pick slot per champion_draft, region per group, altar per region).
Each champion scores groups by (3*gems + gold)/(path + clear turns + 8); foreign-region groups x0.3,
enemy-half groups x0.5, groups whose camp damage exceeds 60% HP x0.01. Tributes at its region's altar.
Shared memory (6 bytes): invasion report (round, x, y, region, enemy count); healthy champions within
30 tiles regroup at the report for 12 rounds. Outnumbered or hurt near enemies: walk home, then recall.

## Key parameters
kBankGems 25, kShopGold 150, recall 40% HP, spawn heal to 90%, foreign region x0.3, enemy half x0.5,
danger 0.6*HP -> x0.01, report age 12 rounds, regroup range 30. Shop: Jungle emblem, then Power shards.

## Changes this generation
New bot (immigrant, split-farm archetype): draft-time region split in draft memory, per-region altar,
invasion reports in shared memory, retreat/recall when outnumbered, attack reset used right after own attack.

## Tried and failed (keep entries from the parent's notes; add yours)
- (from TOOLS.md) Starter step selection picks blocked/unreached tiles; only step to free tiles in `visited`.
- (from TOOLS.md) draw_chebyshev near the map edge throws; tiles_around sets tiles with bounds checks.
- Danger x0.05 with 75% HP threshold: lone champions still died to camps on 00quadrants (5-6 deaths each);
  stricter 60%/x0.01 and 40% recall did not clearly fix it (one 4-game sample).

## Known weaknesses
- 01starvation loses heavily (-500..-700) vs e00-01: few camps, lone champions are out-farmed/killed by a
  grouped team; split farming is a poor fit for scarce maps.
- 00quadrants / 00split are side-dependent losses vs e00-01; many deaths to camps when farming alone.
- No dash travel, no wards, no altar locks. Region seeds use Chebyshev distance, not path distance.
