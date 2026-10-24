## Strategy (≤10 lines)
Parent f00-00 (map-reading draft, plan modes spread/pack/skirmish, boss plan) with the camp-farming
subsystem rewritten in camps.cpp behind the same interface (update_groups, choose_target, farm).
Knowledge: per-member camp health (camps never regenerate), group state, capture news shared through
shared memory byte 5. Scoring: reward per round = value / (path + respawn wait + clear time at the
team's dps + heal trips + capture), solo attrition allowed only with >= 6 fight rounds per trip and
<= 6 trips (f00-03's rule). Spread teams score their own share (value / n) and keep to their zone;
packed teams score the whole reward minus 45% per surplus helper, follow the leader, and may plan a
team clear (rally byte 6, ready bits byte 7). Groups hitting >= 150/round (3+ elks, golem, monolith)
are woken only with 3 champions ready. Clearing: stand tile out of reach of other monsters, retreat
by incoming camp damage, shared respawn wait spot. Fuel: cached kill order and per-group path fields.

## Key parameters
- Value: gems + gold x 0.25 (purchases < 10, round < 900) / 0.12 / 0.05, gold part capped at
  max(15, 1.5 x gems) unless the map is gemless (gold x 1, no cap). Monoliths only as the boss.
- Trips: margin 120 + 10% max hp; per-trip cost 10 + home distance (capped 60).
- Factors: spread zone x1.8, current target x1.3 (not while its respawn is > 10 rounds after arrival),
  leader target x2 (packed), reward x1.3 (x0.2 if claimed), enemy seen within 7 x0.6 (spread only),
  enemy side x0.5, contested x0.1, team-clear option x0.5 (x0.85 on the live rally).
- Retarget every 5 rounds when not engaged; lone team target dropped after 12 rounds (60-round skip);
  gather patience 15 (packed) / 30 (spread or hard-hitting).
- Retreat when hp <= incoming x (exit steps + 2, ranged + 4) + 40; recall afterwards below 50% hp.
- Stand tile cost: Chebyshev distance + 0.12 x threatening strength; range shrinks by 1 after 2 idle
  rounds on the stand (no line of sight). Respawn wait spot: 2-4 steps out, nearest our spawn.
- Path fields built at the end of a turn only when fuel > 6 x map area + 30000.

## Changes this generation
- Camp subsystem rewritten (camps.cpp); rest of the bot (bot.cpp main loop, fights, banking, shop,
  draft, plan) unchanged apart from removing the old update_groups/choose_target/farm.
- Fuel: the parent ran out of fuel up to 578 times per game on 01big-world. A host BFS costs about 5.5
  fuel per tile (50k on 100x100), and scoring every group with a fresh sort cost > 60k per call (the turn
  was suspended and the next round lost). Now: no per-call BFS, cached kill order, upper-bound pruning,
  fields built only with spare fuel: 669 exhaustions over 80 games (parent 1459).
- Bugs found and fixed on the way: champions waiting beside a respawning group blocked the one standing
  on its spawn tile forever (corridors; map-edge groups); a stand tile in range but out of sight left a
  ranged champion idle under fire from a camp hidden in a bush (hp loss with no visible source is now
  counted as incoming damage); capture walks to rewards an ally was already capturing.
- Results (benchmark.py, release, both sides): vs f00-03 on all 40 maps 29/80 before the last fixes
  (parent 35/80); on a 20-map swing set 21/40 with the final build (parent 21/40 on the same maps);
  vs f00-00 15/40 on that set; vs starter 38/39 valid on the other 20 maps (the invalid game is a starter
  exception). Gains over the parent: 00collision, 00gem-fields, 00expanse, 01big-world, 01elk-arena,
  01concentrated-wealth (one side each); losses: 01boss-battle, 00fragment, 01starvation, 01hooks.
  0 bot errors and 0 faults of ours in about 900 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Static fields with every camp tile as a wall: long detours (01shortcut 2/2 -> 0/2 vs e07-05). Use
  detours only when a camp actually blocks the step.
- Short (4-turn) detours around camps: champion oscillates between detour and static path forever.
- Banking smaller bags (<= 20) for 25-30 rounds after seeing an enemy: no measurable gain (8/16 vs 10/16,
  4/12 vs 3/12 on fight maps); deaths are the real leak.
- Gold weight 0.25 early: farms gold-cluster groups instead of gems; lost 01gold-rush to starter.
- Retreat/step away when outmatched while an enemy is in range: free damage taken; fight back instead
  (small maps vs e07-05 3/16 -> 8/16).
- Plain "0.6 x str x clear time" damage estimate: rated the 26-gem central groups infeasible on small maps.
- Chasing with move_towards and marking the turn as moved on failure: froze all champions for 1500 rounds
  on 01gold-rush (unreachable enemy).
- Charging the altar round trip (2 x altar distance x gems / bank_gems) in group time: 5/12 vs 6/12, reverted.
- (f01-05) Optimistic team clears everywhere (assume helpers will come, gather until "ready"): champions
  waited 30+ rounds or walked into 3-elk groups alone; 3/24 vs parent 8/24. Team clears only in packed modes.
- (f01-05) Requiring every planned helper ready before waking a group: a 5-champion wait never completes.
- (f01-05) Dropping a team target the same round it is chosen when nobody else claims it yet: every big
  group got marked lonely and the team farmed 40-gold clusters across the map (01islands 2/2 -> 0/2).
- (f01-05) Pack/skirmish champions scoring their own share (value / n): they split up and were picked off
  (00fragment 25 deaths vs 3); whole value with no surplus penalty: all 5 on one 240-hp cluster (00contest).
- (f01-05) Avoiding groups where enemies were seen, in packed modes: lost the contested centre (00fragment).
- (f01-05) Heal to 85% after every retreat: too many trips home; recall only below 50% after leaving.
- (f01-05) Retarget every 15 rounds instead of 5: 19/40 vs 20/40. Gold weight 0.35 early: 16/40 vs 20/40.
- (f01-05) Monoliths as ordinary targets for a team of 4+ after round 300: deaths and lost gold (00contest).

## Known weaknesses
- 01boss-battle: champions reach the altar-guarding monoliths one at a time with half the parent's gold;
  the boss is never killed (parent wins one side).
- Overall still slightly below the parent head-to-head (15/40); outcomes on small fight maps swing with
  tiny changes (00fragment, 00packs, 01hooks).
- 00great-wall, 00stampede: heavy elk groups are the main gem source; team clears there are slow and
  carried gems are lost to camp deaths.
- No dash, wards, teleport crystals or altar locks (unchanged from the parent).
