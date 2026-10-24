## Strategy (≤10 lines)
Map reader. The draft (10x fuel, full map) measures the map and writes a `Plan` into draft memory
(plan.h/plan.cpp): walking distances from both spawns, per-group reachability, an "enemy side" flag,
an angular zone rank around our spawn, spawn-to-spawn gap, gem supply and altar distance.
From those numbers it picks one of three modes and the matching draft order:
- skirmish (spawn gap <= 32 and area <= 1600): Vesper, Ryth, Tavi, ...; strength shards; chase on sight;
- spread (>= 3 reachable groups per champion, area >= 1800): solo farmers, each champion gets a zone bonus;
- pack (otherwise): Vesper, Ryth, Vendetta, Willow, ...; followers favour slot 0's target (x4).
- boss plan (any mode): if every walkable altar approach is a camp spawn tile (01boss-battle: monoliths), no
  banking while that group lives; from round 400 the whole team chips it down, captures it, then banks.
Champions score camp groups by value / (travel + respawn wait + clear time + capture + health-trip cost),
with a kill-order damage model to decide how many helpers a group needs; shared memory holds each
champion's target. Bank at altars, recall when hurt, shop eagerly, fight by local HP x strength ratio.

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
- Boss plan: boss_start = 400; boss score 100 / (distance + 10) while alive, skipped while respawning.

## Changes this generation
- New bot (immigrant from starter infrastructure): plan.cpp/h (draft map reading), draft.cpp (plan-based
  draft), bot.cpp/h (targeting, pathing with detours, banking, shopping, spells, fights).
- Pathing: static distance fields per group/altar/home; when a camp blocks the static path a camp-aware
  detour is kept until arrival (fixes champions stuck next to altars with 200+ gems); detours drop goal
  tiles occupied by camps; champions never stand on a group's tiles unless capturing (blocks respawn).
- Fuel: fields and bitboards are mirrored into wasm memory (export_linear_buffer) instead of per-tile host
  calls (each ~1k fuel); worst sampled turn on 01big-world went from 12k to 56k fuel left. The draft's
  map analysis uses the same mirrors.
- Boss-guarded altars (draft detection + boss phase): 01boss-battle vs e02-07 -827/+427 -> +940/+955,
  vs starter 0-0 ties -> +993/+1171. Other maps unchanged (deterministic replays identical).
- Contested captures: a champion held a capture for 300+ rounds on a long region an enemy also stood on
  (01yellow-brick-road lost 68-120 to starter); now times out and avoids the group for 60 rounds.
- Results (benchmark.py, release, both sides): vs starter 79/79 valid on all 40 maps (+760 mean; the one
  invalid game is a starter exception). vs e07-05 (run-1 champion) 27/40 on 20 maps (twice, +191 mean);
  vs e03-07 12/12; vs e00-01 16/16; vs e06-04 3/8 on four fight-heavy maps. 0 bot errors of ours in ~870 games.

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

## Known weaknesses
- Small fight maps vs strong bots are side-dependent: 00collision, 01starfish, 00gemfight, 00packs mostly lost
  to e07-05 / e06-04 (we die carrying gems; enemy champions out-earn us in gold).
- Spread mode walks to far contested groups (01starfish side B) and loses carried gems on the way back.
- No dash, no wards, no teleport crystals, no altar locks of our own; groups unreachable by walking are ignored.
- Boss plan only covers camps on altar approaches; altars reachable only by dash/teleport are not handled.
- Willow as support follows slot 0 and dies often on 01elk-arena.
