## Strategy (≤10 lines)
Defense-first economy from f04-03/f03-05: altar locks, deny, tribute under fire, square-law fights, draft
Vesper Vale, Vendetta, Marcella, Tavi, Varg with one home region per champion, kill-by-kill camp clears.
This generation rewrote the fuel subsystem (nav.cpp, common.h; same calls: view / invalidate / free_tile / foes /
go / step_on / walk_static / fill_fields / eta / Bits / Dist / grow / live / fuel):
- Boards and distance fields are read straight from the engine's chunked storage in wasm memory (8x8 tiles per
  chunk; layout checked against host reads at start, host reads otherwise). No exports at all.
- Boards are built in place into storage that lives as long as the bot (no bitboard temporaries).
- Allies are read once a round, enemies once per vision refresh, each with one non-throwing host call.
- Field searches and route searches start only when the fuel left covers this map's cost; choose() is
  pruned by exact bounds and deferred (≤ 2 turns) when the turn cannot afford it.

## Key parameters
- Field search cost estimate: 3500 + 4·tiles; go() needs 12000 + half of that; fill_fields needs reserve + all.
- approach_goal (safest tiles): needs 25000 + 35·work (measured 15–30 fuel per unit of work; was 60000 + 40·work).
- choose(): due every 4 rounds; deferred while fuel < moving-average choose cost + 35000, at most 2 turns.
- simulate() memo: 12 entries per group, exact inputs (team, pool, dps, max hp, damage taken, clear-cost version).
- Measurement switches (all off): kFuelProfile (main.cpp), kPhase / kHeavy (bot.cpp).

## Changes this generation
Measured fuel (fuel_remaining deltas; release build, flags on only for measurement):
- Host call ~40 + a per-tile charge on whole-board work; 100x100: field search 30k, field export 23k, bitboard
  export 1.7k, temporary 0.9k; vision queries 190 each. A C++ exception costs thousands: champion::position() on
  an ally out of sight throws, and the parent did that on every vision refresh (view() 8.5–12k, now 1.3–1.8k).
- Small helper calls cost ~100 fuel each (Dist::at, eta, remaining); a float loop ~8 per iteration.
- Parent on 100x100 maps: ~54k a turn, 2.6% of turns cut short (6–9% for some champions); now ~32k, 0.1%.
  On camp-dense 01boss-battle choose() cost 50–85k a call (30 groups, 50–80 simulations) and cut 2–4% of turns:
  now ~1/3 of the simulations, cuts ~15 a game (was ~100).
- camp_group::state() allocates a board, re-reads vision and makes a host call per camp: replaced by two raw calls.
- camp_groups() costs ~38k on boss-battle and was built twice at start: analyse_map hands its list over.
- camp_block_ is updated per changed group (a full rebuild is ~15k on camp-dense maps).
Results (bench2.py, 40 maps × both sides; parent figures from the evaluation cache, which reproduces locally):
- v1 (zero-copy reads, in-place boards, no-throw reads): vs f03-05 46/80 (parent 40), vs f04-05 42/80 (33).
- v2 (+ choose bounds/order/memo/deferral, approach_goal gate, incremental block): vs f03-05 45/80, vs f04-05
  43/80, vs f02-01 64/80 (parent 65). 0 faults and 0 `bot error:` in all games.
- 01boss-battle B vs f03-05 (worst loss last generation, 0–2110): won 1049–50 in v1.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
  (This generation: no copy at all, read the field's own storage.)
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: debug prints (fuel) alone moved 00lost-world B from 1069 to 524 rounds. Judge changes on
  full 80-game runs, never on single maps; rounds-to-win vs starter is a less noisy farming-speed metric.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.
- Lock detours of 14 tiles + idle champions running locks to far altars (≤40): 57/80 vs 65/80 at 8 tiles.
- me.attack(altar_lock_at(a)) throws "Target must be a visible enemy or neutral camp" (the parent's lock code
  never worked); a tribute at the locked altar is what hits the lock.
- Hunting any enemy carrying ≥40 gems (prey even when far): 65/80 (+430), same as without; not kept.
- Pair farming (melee + ranged share the leader's target, plan as a unit of two): vs f02-01 66/80 with fewer gems
  (221 vs 246 by round 600) and no extra kills; vs f03-04 8/20 vs 9/20 alone, deaths 19 vs 14 per game. The pair
  rarely stands together (banking, recalls), so it only splits income.
- Charging every raider the full walk-out reserve: no team could ever raid a double monolith (00rows lost late to
  raiders on both sides). Only the last champion in reach needs it.
- Win counts swing ±5/80 between versions that change nothing systematic (17 map flips each way); compare
  aggregates (gems by round 600, deaths by cause, gold) from the stats files instead.
- A phase profiler that scans a tag table on every call adds ~10–20k a turn and doubles the cut turns it
  measures; log per-turn deltas into a fixed array and print only heavy turns (kHeavy).
- Host bitboard set() / is_set() cost about as much as a whole-board operation; raw writes need the layout check.
- The second round of fuel cuts (v2: choose ~3x cheaper, cuts on boss-battle 100 → 15 a game) did not add wins
  over v1 (88/160 both): once turns stop being cut, fuel is no longer what decides games.

## Known weaknesses
- Outcomes are still decided by PvP: 00gemfight and 01gold-rush are lost the same way as the parent lost them.
- Turn 1 on camp-dense maps still runs out (initialise ~190k + the first choose); a champion that dies is
  re-initialised by the engine on a dead turn (its camp memory starts over).
- choose() still costs 30–60k when many clear costs change at once (first plan, after a respawn).
- 00eight, 01elk-arena and 01concentrated-wealth remain coin flips against the lineage bots.
