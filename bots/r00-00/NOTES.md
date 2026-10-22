## Strategy (≤10 lines)
Map reader. Draft measures map side, cluster vs fighter camp counts, golems near spawn and nearest altar distance,
then picks a plan: big_camp (golem within half the map), aggro (side <= 35), else farm. Each plan has its own
draft order (strength/tank champions: Vendetta, Vesper Vale, Ryth, Varg, Marcella, Willow). Plan, bank threshold,
danger % and chase range go into draft memory bytes 0..4 ('R', kind, bank, danger, chase).
Champions score every camp group by (gems + 0.08*gold) / (distance + clear time + 6), skip groups whose estimated
camp damage exceeds danger% of current health (team dps counts nearby allies), remember respawn timers, cast damage/
stun abilities and attack resets on camps and enemies, shield/heal when hurt, carry gems until the bank threshold,
recall below 33% health, buy a Jungle emblem then power shards. Stuck/unreachable targets are skipped for 40-60 rounds.

## Key parameters
bank_threshold = clamp(altar_dist/2 + (clusters>fighters ? 14 : 8), 7, 40); danger_pct 45 (55 aggro);
chase_range 2 (5 aggro); recall < 33% hp; stay on spawn until 95% hp; target hysteresis x1.3.

## Changes this generation
New bot from the starter infrastructure (main loop, pathing helper) with map-reading draft and plan memory.

## Tried and failed (keep entries from the parent's notes; add yours)
- Casting "excludes walking" spells after moving throws (engine error 126): casts are now skipped after a move.
- Touching a camp/champion after our own cast killed it throws "not in vision": check vision tiles first.

## Known weaknesses
- 01big-world: side A scores 0 gems (champions idle near spawn/targets); root cause not found, stuck detection did not fix it.
- 00elkring side B and 01islands lose to the starter; danger filter may be too passive vs elk-only maps.
- No coordination between champions beyond the shared plan; possible rare "attack cannot be reset" error (seen once).
