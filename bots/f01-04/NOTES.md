## Strategy (≤10 lines)
Map reader. The draft (10x fuel) measures the map and writes a `Plan` into draft memory (plan.cpp): walking
distances from both spawns, group reachability, enemy-side flags, angular zones, spawn gap, gem supply.
Modes: skirmish (gap <= 32, area <= 1600; strength shards, chase on sight), spread (>= 3 groups per champion,
area >= 1800; solo farmers with zone bonus), pack (followers favour slot 0's target x4). Boss plan: if every
altar approach is a camp tile, no banking while it lives; from round 400 the team kills it, captures, banks.
Champions score groups by value / (travel + respawn wait + clear + capture + health-trip cost) with a
kill-order damage model for helpers; shared memory holds targets. Bank at altars, recall when hurt, shop
eagerly, fight by local HP x strength ratio. Pathing: static fields with camp/unit detours; dash champions
(Vesper Vale, Vendetta, Ren...) dash 2 tiles down the travel field whenever it saves two steps and no enemy
is within 6 tiles; all allies (also hidden in bushes) count as occupied when stepping.

## Key parameters
- Group value: gems + gold x (0.15 early / 0.08 / 0.03 late or well equipped); gemless maps value gold x1.
- Feasible group: clear damage (kill order by strength/health) x0.8 <= hp_ref x0.9 + 100,
  hp_ref = max(hp, 1100) when helpers come; infeasible x0.03; surplus claims x0.55 each.
- Enemy-side groups x0.5 (gold-only x0.25); zone x1.6 (spread); current target x1.5; retarget every 40 rounds
  or when the target respawns more than 6 rounds out.
- Fight ratio (sum hp x str within 6-7 tiles): engage >= 1.3 (chase only in skirmish/chase plans or >= 1.6),
  retreat < 0.85, hold between (ranged keep out of enemy reach, melee press within 4).
- Retreat/recall at 30% hp (45% with enemies near, 75% when outmatched) but never turn away from an
  enemy in attack range unless below 20%.
- Bank threshold plan.bank_gems = clamp(20 + avg_dist/2, 25, 60); also when the altar is <= 2 (or <= 6 with 15+).
- Shopping: walk home with >= 150 gold within 10 tiles, recall with >= 210 gold; Tavi shops anywhere.
- Chases capped at 12 steps without a hit, then 30 rounds off.
- Capture hold timeout: capture_time + 6 rounds on a region without payout marks it contested for 60 rounds (x0.1).
- Boss plan: boss_start = 400 for a monolith/golem guard (0 otherwise); boss score 100 / (distance + 10)
  while alive, skipped while respawning.
- Travel dash: dash_min 2 (field value at our tile), lands 2 tiles away gaining >= 2 steps, needs a free first
  step (holes may be jumped); best 3 landings tried; dash_enemy_clear 6 (no dash with an enemy within 6).
- Stepping blocks every live ally position (champions(team::ally)), not only visible champions.

## Changes this generation
All measured with benchmark.py, both sides of all 40 maps, against the parent f00-00 (deterministic).
- Kept: travel dash down step_along's field when no enemy is seen (dash_min 4): vs f00-00 49W/31L +110.8;
  vs f00-03 37W/43L -21.9 (parent vs f00-03: 35W/45L -31.9).
- Kept: allies (also those hidden in bushes) are occupied tiles in step/dash masks: vs f00-00 51W/29L +109.4
  (same margin, +2 wins; a correctness fix).
- Kept: dash_min 4 -> 2: vs f00-00 58W/22L +144.2; vs f00-03 37W/43L +14.4.
- Kept: travel dash allowed while enemies are visible but more than 6 tiles away (was: none visible):
  vs f00-00 61W/19L +255.1 (final version).
- Final checks: vs starter 77W/2L +766 (the one invalid game is a starter exception); 0 faults and
  0 bot errors of ours in 880 games.

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
- f01-04: escape dash in step_away (dash out of threat tiles toward home when retreating): vs f00-00 +98.3
  vs +110.8 without it (dash_min 4 version); flag dash_escape=false.
- f01-04: melee chase dash (dash next to the chased enemy when 2-3 tiles away, then strike): vs f00-00
  47W/33L +12.3 vs 58W/22L +144.2 without it; flag dash_chase=false. Dashes near fights hurt; travel dashes help.

## Known weaknesses
- Small fight maps vs strong bots are side-dependent: 00collision, 01starfish, 00gemfight, 00packs mostly lost
  to e07-05 / e06-04 (we die carrying gems; enemy champions out-earn us in gold).
- Spread mode walks to far contested groups (01starfish side B) and loses carried gems on the way back.
- No dash, no wards, no teleport crystals, no altar locks of our own; groups unreachable by walking are ignored.
- Boss plan only covers camps on altar approaches; altars reachable only by dash/teleport are not handled.
- Willow as support follows slot 0 and dies often on 01elk-arena.
- Still loses to f00-03 on 00stampede, 01concentrated-wealth, 01starfish, 01yellow-brick-road, 01the-farm
  (target choice / fights, not pathing). Fields still treat holes as walls: groups reachable only by dash
  or teleport stay ignored.
