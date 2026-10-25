## Strategy (≤10 lines)
Camp sweep (immigrant on the starter's main loop). Draft Vendetta, Varg, Vesper Vale, Marcella, Tavi (resets, splash).
Each champion runs its own farming loop: every 3 rounds it scores all camp groups by (gems + w·gold) × factors /
(walk distance + respawn wait + clear time + extra trips + 8) and claims its target in shared memory (bytes 0–4).
Group knowledge per champion: alive / reward waiting / respawning (with respawn round), monster health left.
Alive groups count as walls for routing and distances (camp plugs in front of altars). Kill monsters first (lowest
hp, splash-aware), spells before the basic attack, attack reset after it; capture by standing on a group tile.
Bank in small batches whenever an altar is close. Enemies within 8: engage when our nearby power (hp·(str+focus/4),
allies within 6) is ≥ 0.7× theirs or a kill is in reach, otherwise flee home and recall once clear.
Shop on spawn or when dead: jungle(str) → monster(str) → durability(hp) → alternating power shards.

## Key parameters
- Score factors: ×1.3 current target, ×0.3 light group (clear < 12 turns) claimed by an ally, ×0.65 group nearer the
  enemy spawn, ×0.4 within 10 of an enemy sighting < 20 rounds old; gold weight 0.35 (0.12 after round 1200).
- Multi-trip: damage taken ≈ 0.75·monster str·clear time; beyond hp − 30% → trips of 65% max hp, ≤ 4 trips,
  each costing distance from spawn + 14.
- Bank: ≥ 80 gems anywhere, ≥ 8 within 3, ≥ 20 within 8, ≥ 45 within 16, ≥ 40 within 15 after an enemy sighting,
  the winning amount, the last rounds, or ≥ 15 when no group is ready.
- Retreat below 30% hp or hp < 2× incoming; shopping trip at ≥ 300 gold below 70% hp; ≤ 2 heal turns on spawn.
- Fight ratio kFightRatio 0.7; chase ≤ 8 turns without a hit, then 15 rounds off.
- Fuel: target choice only with ≥ 45k fuel left; cheap straight-line bound prunes groups before distance reads;
  group tiles and ring tiles read through precomputed raw offsets; turns resumed in a later round stop at once.

## Changes this generation
- New bot (immigrant, camp-sweep archetype). Local results of the final version: vs f00-03 7/16 (8 maps, mean
  −45); vs starter 15/16 on 8 other maps (the loss is 01yellow-brick-road side B, 0 gems banked); vs f06-05 0/16
  on 8 maps (mean −777) and 0/16 on 8 more with an earlier version. No faults; no `bot error:` lines from our side
  in the final runs.
- Fight ratio sweep vs f00-03 (same 16 games): 1.15 → 2/16 (−367), 0.9 → 4/16 (−188), 0.7 → 7/16 (−45),
  0.5 → 7/16 (−71). Kept 0.7.
- Game budget: about 165 games played.

## Tried and failed (keep entries from the parent's notes; add yours)
- (f07-06) vision::camp_groups() costs ~40k fuel per call; the first-turn init with it plus a full turn overflowed
  200k and the resumed turn threw "Known occupied destination". Keep the map::camp_groups() objects from init and
  call state() only for groups with a visible tile; init on the first turn and act from the second.
- (f07-06) Distances through camp tiles: on 01the-farm the altar sits behind a wall plugged by a camp group, and
  champions oscillated beside it for hundreds of rounds (0 gems banked). Alive groups must be walls.
- (f07-06) Per-tile rdist reads over every group's ring cost ~56k fuel on 00lanes; the over-budget turn resumed the
  next round and the stale-round guard aborted the move every third round.
- (f07-06) Stronger avoidance of enemy-side groups (×0.4) and of sightings (×0.25 within 12, 40 rounds):
  2/16 (−422) vs 2/16 (−367).
- (f07-06) Bank triggers "enemy seen and ≥ 15 gems" / "idle with any gems": walks of 20–30 steps to bank
  30–40 gems; ~30% of turns were bank walks on 01the-farm.

## Known weaknesses
- Far behind the lineage (f06-05): walking is ~35% of turns, attacking ~20%; no home regions per champion.
- 01hooks and 00quadrants lose by ~1000 / ~300: lone farmers are caught by grouped enemies (deaths are kills).
- No dash pathing: maps that need dashes over holes (01yellow-brick-road) bank nothing.
- No wards, altar locks or teleport crystals; no healing logic beyond spawn visits.
