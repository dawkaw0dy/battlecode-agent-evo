## Strategy (≤10 lines)
Scaling economy (parent f02-01, grandparent f01-07). Draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; the draft
splits our side's camp groups into one home region per champion and now also writes a full group table (camps,
value, respawn, spawn distances, dash-route distance) into draft memory, so the champions' first turn skips the map
scan. Champions score groups by value per turn, claim them in shared memory, turn gold into shards at every spawn
visit, raid bosses together and fight by a square law (Σhp·Σdps), fleeing home when outnumbered.
Dashers (Vesper, Vendetta) route over one-tile holes and dash across them. Work that does not fit in the fuel left
(a group field, a move, a vision refresh before a hit) is skipped instead of running the turn out of fuel.
Spells as in f02-01 (spells.cpp): each slot read from the engine, highest-value ready slot fires each turn.

## Key parameters
- Picks unchanged: Vesper Vale, Vendetta, Marcella, Tavi, Varg (first 3–5 by team size).
- Draft memory: 0 'E', 1 team size, 2 groups, 3 'G' if the group table is present, 4.. home slot per group,
  256..1023 group table (12 bytes per group + 3 per camp; omitted when it does not fit -> champions scan the map).
- Fuel guards: field compute needs field_cost_ + 5k; go() needs max(25k, go_cost_ + 3k); a hit or reset that must
  refresh vision needs g_view_cost + 4k. Each estimate is a decaying peak: max(new, old − old/16).
- Dash routing: jumpable hole = hole with standable floor on both sides, straight across (dash_walls()); dashers use
  it for group fields, home/altar fields and go(); next to a hole they may dash for a 2-step gain (else 3).
- Everything else as in f02-01 / f01-07 (score, gold weight, trips, retreat, tribute, fight ratios, spells).

## Changes this generation
- Fuel in the opening (measured with probes): a distance field costs 28–57k on 100×100 but fill_fields kept only
  40k, the second hit_group after go() refreshed vision (~18k) with ~13k left, and go() computed a route with 25k
  left. Each overrun lost the next turn (big-world: 36 lost turns in 300 rounds -> 7, all of them the first turn).
  After round 3: 34 overruns in 70 games, opponents 276.
- Group table in draft memory: the champion's map scan (camp_groups + per-camp host calls, ~4k fuel a group, plus
  2 spawn fields) is replaced by unpacking the table (reserve the vectors: allocations dominate local code).
  The first turn now fits on most 50×50 maps; many-camp maps still spill into rounds 2–3.
- Dash routing over one-tile holes, then dash-route distances from the draft (before a group's own field exists,
  eta() fell back to the walking distance, which marks hole-locked groups unreachable).
- Fuel estimates as decaying peaks instead of the maximum (one-off costs made go() refuse moves all game).
- Results (local): full 40 maps, previous commit (before the decaying peaks): vs f00-03 60/80 (parent 56/80,
  margin +25818 vs +16356), vs f01-07 64/80 (parent 58/80, +25940 vs +17060). Final version: 26/30 vs f00-03 and
  19/30 vs f01-07 on 15 mixed maps (previous commit 21/30, 22/30), 19/19 vs starter, 19/40 vs f02-01 (mirror, ~50%).
  0 faults, 0 caught exceptions of our own in all games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Pre-emptive Varg shield (before the first camp hit): shielding +60%, results 30 vs 31/52, margin down. Kept the
  "under fire" rule; the shield takes the cast a splash reset would use.
- Chase dash when only two tiles short (dash + hit on a runner): 54 vs 56/80 vs f00-03. Reverted.
- Melee nuke whenever the target is ≥3 away: 43 vs 42/60, inside noise; not adopted (only after a missed chase step).
- can_attack() stays false after Vendetta's kill although the engine has the attack available: a reset then throws
  error 26 ("An available attack cannot be reset"). Hitting directly works.
- Cassi & Issac instead of Varg (two bodies for one pick): 5 vs 6/22 on f00-03 loss maps, margin −5910 vs −4926.
- Ryth instead of Varg: 24 vs 23/46, margin +791 vs +498: noise, not adopted.
- Keeping the dash when the route crosses a hole within 8 steps (Vesper waited 6 turns at a hole after a travel
  dash): 31 vs 33/50, reverted.
- Ending the first turn after setup unless 70k/90k fuel is left: 27 vs 33/50 (on small maps it gives up a round-1
  turn that fits).
- Contested groups (d_enemy ≤ d_ally + 2) ×0.75 in the first 200 rounds: 29 vs 33/50, reverted.
- Walking-only routes for a fleeing dasher whose dash is on cooldown: 30 vs 33/50, reverted.
- Plain maximum as fuel estimate: one fallback route (two extra fields) or the first vision refresh (static ally
  list) fixed go() at 30–44k and hits at 18k on a 40×40 map for the rest of the game.
- Allocations are expensive (~500–1000 fuel each): unpacking 24 groups cost 60k before reserving vectors (25k after).

## Known weaknesses
- Games are chaotic: a one-rule change flips ~15 of 80 games both ways; the f01-07 loss-map set read 23/28 once
  and 18–19/28 for every later variant. Judge on ≥80 games and the gem margin.
- Many-camp maps (00road-of-riches, 01boss-battle) still lose rounds 1–3 to setup: camp_groups() (~1k a group),
  the per-tile lookup tables and spawn_tiles() per group remain.
- A travel dash often leaves the dash on cooldown at the next hole (waits up to 8 turns).
- Still loses 00interlock, 00corners and 00rushbait-v2 on side A and 00fragment, 00road-of-riches on both sides:
  fights and mid-game economy, outside this generation's opening focus.
- No wards, no altar locks, no teleport crystals.
