## Strategy (≤10 lines)
Map reader. The draft measures map side, team size, spawn-to-spawn distance, cluster vs fighter camp counts,
golems/monolith within half the map and nearest altar distance, then picks one plan: aggro (spawns <= 32 apart
on maps <= 45), hunt (golem near and >= 4 champions), else farm. The plan (kind, bank threshold, danger %, chase
range, assumed helpers, allies-per-group cap, golem start round, recall %, shop gold) is a 10-byte struct in draft memory.
Each plan has its own draft order (Varg first on cluster-rich maps). Champions drop camp groups they cannot walk
to, score groups by value / (claimers+1) / (dist + clear time + 6) and claim their target in shared memory
(one byte per ally slot) so allies spread out, or join big groups up to the cap. Groups that fail the danger
estimate are only a fallback. Sticky banking, recall below the plan's %, shop trips, power shards of the main stat.

## Key parameters
bank = clamp(altar_dist/2 + 16 (22 cluster-rich), 16, 45) (aggro <= 20); danger 45/55/50 (farm/hunt/aggro);
cap 2 (1 cluster-rich farm, 3 hunt); golems from round 600 (farm), 500 (aggro), 0 (hunt); recall 33%; shop at 150 gold.

## Changes this generation
New immigrant bot from the starter infrastructure (main loop, pathing helper): map-reading draft, plan struct in
draft memory, shared-memory group claims, static reachability filter, guarded unit queries.

## Tried and failed (keep entries from the parent's notes; add yours)
- Strict danger filter with no fallback: on 01big-world every group was rejected and champions idled at spawn (0 gold).
- Re-skipping unreachable groups for a fixed 40-60 rounds: on 100x100 maps they were re-picked at once
  (skip check uses round + distance); now unreachable groups are dropped at init and skips add the distance.
- Several live queries (camp health/state, group state, ability use) can throw engine errors 97/98 even after
  is_visible(); they are wrapped in a local guard that skips the unit for the turn.

## Known weaknesses
- Farming is much weaker than r00-01 (loses ~-1000 on 01big-world / 01islands vs r00-01): no Treasure items, slow clears.
- About 1 in 8 games still prints one engine error 98 (source not located).
- 00corners lost on both sides vs starter in the last run.
