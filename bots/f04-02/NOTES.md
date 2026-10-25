## Strategy (≤10 lines)
Parent A (f03-05) unchanged in play: defense-first farming (altar locks on every altar, deny enemy tributes, tribute
under fire when the channel outlives the damage), square-law fights, f02-01 spells, home regions per champion.
Draft: Vesper Vale, Vendetta, Marcella, Tavi, Varg (first 3-5 by team size), unchanged.
New this generation, the opening: the draft turn (10x fuel, whole map) now writes the whole group table into
draft memory (camps, kinds, rewards, walks from both spawns and to the nearest altar), so a champion's first turn
no longer scans the map. Turn 1 used to spill into rounds 2-5 on most maps (all five champions standing still while
the enemy walked); now initialisation fits round 1 on all but the densest map, and the two remaining spawn/altar
fields are computed at the start of turns 2 and 3. Arriving first at contested camps is the likely gain vs f03-04.

## Key parameters
- Draft memory: 0 'E', 1 team size, 2 groups, 3 layout ('T' table after the home slots, 'P' pair homes then the
  table, 'Q' pair homes only), 4.. home slot per group, then (pairs) the table: home_altar (2 bytes), per group
  n (+128 when gems/gold/respawn follow explicitly; otherwise derived from the camp kinds, which matched the engine
  on every map), d_ally, d_enemy, bank_d (2 bytes each), per camp x, y, kind. Largest map (00road-of-riches,
  51 groups / 132 camps) 810 bytes. Falls back to the old map scan when it does not fit.
- Turn 1 (table present): camp handles fetched on first use (camp_groups() ~30k), threat tables per group on first
  use, camp-tile lookup in a 64+-slot hash (CampIndex) instead of two map-sized int16 arrays; spawn field on turn 2,
  altar field on turn 3, or at once when we carry gems.
- kOpenPairs (plan.h, off), kOpenRounds = 200: opening home regions shared by slot pairs.
- Debug switches at the top of bot.cpp: kDeathTrace (what each champion was doing when it died, first-turn end
  round), kProfileInit (fuel per initialise step), kVerifyTable (table vs a fresh map scan). All off.

## Changes this generation
Crossover note: the focus area was identical in both parents (draft.cpp, plan.cpp, choose(), farm() and the draft
memory layout are byte-for-byte the same in f03-04 and f03-05), so there was nothing of B's opening to port. B's
wins on the evidence maps come from its combat rewrite (enemies()), which is outside the focus and was not ported.
All runs benchmark.py, both sides; 0 faults and 0 bot errors throughout (825 games).
- Baseline (parent copy) vs f03-04: 43/80 (+130). Early kill difference (before round 150) predicts the result:
  ahead 82% wins, behind 32% (we were behind in 31 games, ahead in 17).
- Draft group table + cheap first turn (first version): vs f03-05 22/80. Cause: the deferred altar field was only
  computed "when a turn had fuel to spare", which busy champions never had; first tribute at round 582 vs 285.
- Fixed (fields at the start of turns 2-3, altar field at once when carrying gems): vs f03-05 43/80 (+72),
  vs f03-04 51/80 (+209; early kill difference ahead 22 / behind 19 games).
- Final build on a hard 10-map subset (00gemfight, 01hooks, 00eight, 01starfish, 00quadrants, 01big-world,
  00road-of-riches, 01the-farm, 00collision, 01elk-arena): vs f02-01 15/20, f01-07 15/20, f00-03 15/20;
  vs starter 10/10 (5 maps). The fallback path without a table (forced): 2/2 vs f02-01, no warnings.

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
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: debug prints (fuel) alone moved 00lost-world B from 1069 to 524 rounds. Judge changes on
  full 80-game runs, never on single maps; rounds-to-win vs starter is a less noisy farming-speed metric.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.
- Lock detours of 14 tiles + idle champions running locks to far altars (≤40): 57/80 vs 65/80 at 8 tiles.
- me.attack(altar_lock_at(a)) throws "Target must be a visible enemy or neutral camp" (the parent's lock code
  never worked); a tribute at the locked altar is what hits the lock.
- Hunting any enemy carrying ≥40 gems (prey even when far): 65/80 (+430), same as without; not kept.
- (f04-02) Pairs sharing home regions all game (3 regions for 5): vs f03-04 43/80 -> 43/80, margin +130 -> +59
  (fight-heavy maps 9 -> 14/40, A's strong maps 34 -> 29/40).
- (f04-02) Pairs only for the first 200 rounds (on top of the fast first turn): vs f03-04 50/80 (+238) against 51/80
  without, vs f03-05 32/80 against 43/80. Kept as kOpenPairs = false.
- (f04-02) Ryth instead of Varg, picked before Tavi (Vesper, Vendetta, Marcella, Ryth, Tavi): vs f03-04 42/80 (+82)
  vs 43/80 (+130), our PvP deaths 1666 -> 1792.
- (f04-02) Deferring first-turn work to "a turn with fuel to spare" (fuel(reserve + 60k) at the end of a turn): it
  never comes for a busy champion; give deferred work a fixed turn instead.
- (f04-02) Fuel: libc memset is a byte loop (~2.5 fuel/byte); a 64-bit fill still ~8 fuel/word; vector::resize
  zero-fills at ~2 fuel/element. Two map-sized int16 tables cost ~80k on a 100x100 map: use a small hash instead.
- (f04-02) Fuel: the first champion turn starts with ~174k (2x minus ~26k of start-up), not 200k. Printing in the
  middle of a turn (std::endl flushes) distorts profiles: collect strings, print once at the end.
- (f04-02) mapgen.mjs info lists only one half of a symmetric map's camps (00road-of-riches: 66 of 132).

## Known weaknesses
- Combat is still the parent's square law over visible units: 1100-1600 PvP deaths per 80 games against 1100-1200
  for f03-04. Death traces: most deaths are while running (caught), the rest are hurt champions that stay because
  the team ratio (allies within 7 at full health) looks good while the enemy focuses the weakest.
- Varg is our weakest pick (vs f03-04 baseline: 78 kills, 230 deaths, fewest gems); the replacement tried (Ryth)
  was no better.
- Still loses 00eight, 00gemfight, 01hooks, 01gold-rush and 01yellow-brick-road on both sides against f03-04 most
  of the time (the first fights snowball). No wards or teleport crystals; heals are never cast (no Willow logic).
- On 00road-of-riches (132 camps) initialisation still ends in round 2.
