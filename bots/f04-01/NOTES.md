## Strategy (≤10 lines)
Defense-first economy from the parent f03-05, unchanged in its decisions: farm camps by value per champion-turn,
bank at the nearest altar, carry an altar lock and lock every altar in reach, hunt channelling enemies, fight by the
square law. This generation rewrote the fuel subsystem behind the same interface (live(), fuel(), view(),
invalidate(), free_tile(), foes(), fill_fields(); now in fuel.cpp, shared.h and common.h):
- A suspended turn (out of fuel) used to be wasted: it resumed next round where live() refuses every action.
  play_turn() now replays that round's turn from the start.
- The vision snapshot, champion reads, camp-group states, altar locks, grow() and local field copies were made
  5-50x cheaper, so on big maps the bot stays far below the budget and the fixed fuel gates no longer skip moves.
- fill_fields() only starts a walking field when the dearest one measured so far fits with room to spare.
- Fixed a latent crash (heap-layout dependent): see "Changes".

## Key parameters
- Field fill rule: start a field when fuel_remaining > max(min(reserve, 20k), 1.25·field_cost + 5k);
  field_cost = largest measured compute + export (first one timed in initialise()).
- Snapshot: visible tiles re-read only when our position changed (no wards); allies once per round (nobody else
  moves during our turn); camps, champions and teleports on every refresh (our hits kill, dashes move us).
- foes() and altar-lock boards cached until invalidate().
- kFuelStats (bot.cpp) prints fuel per turn and suspended turns every 500 rounds.

## Changes this generation
Measured costs (100x100 map, 30x30 in brackets): host call ~40; new bitboard ~750 [260]; in-place board op ~200
[40]; champion handle ~800 (it reads both ability slots); camp_group::state() ~4k (re-reads the vision board);
full field 30k [1.1k]; first Dist::load 23k (vector zero-fill) vs 2.6k after; old view() refresh ~9k.
- view(): boards filled in place, LazyBits exports a local copy on first at(), raw sdk_champion reads for allies.
- foes(): positions_into + sdk_champion_at/sdk_champion/sdk_champion_gems instead of champion handles.
- observe(): group state via sdk_camp_group_state/cooldown; enemies(): foe memory only read when kFoeMemory.
- grow() on a scratch board with shift_*_into (5.6k → ~1.6k per step on 100x100); Bits/Dist never zero-fill;
  per-altar tribute zones and per-group respawn-wait zones built once; go()'s route copy reuses one buffer.
- Profile (6 maps vs f03-04): parent avg 15–58k fuel/turn, up to 79 suspended turns per champion on 01big-world
  (2172 turns > 60k); now avg 8–30k, 1–2 suspended turns (the first turn: initialise() needs 240–330k on maps
  with many camps), 213 turns > 60k.
- Crash found on the way: with the heap layout changed, 01boss-battle trapped (out-of-bounds read, engine
  "bot faults") ~50 times a game. Cause, inherited from the parent: turn()'s plug block clears plug_ for a boss
  guarding the only altar and then reads k_[plug_] and remaining(plug_) with plug_ = -1. Guarded now.
- Also: the global View (bitboards) is built on first use (a bitboard sizes its storage from the SDK's layout,
  itself set during dynamic initialisation), the camp layout is read with a buffer that fits it
  (camp_groups_safe(); the SDK starts with a fixed 256-word buffer), the dash tries a raw cast instead of
  throwing on refused landings, camp_groups() is read once at start, and reward groups are checked every turn.
- benchmark.py, 40 maps × 2 sides, 0 faults / 0 caught exceptions in every run listed:
  v1 (rewrite only): vs f03-05 (parent) 48/80 (+83); vs f03-04 53/80 (+246; parent's record 43/80, +130);
  vs f02-01 67/80 (+507; parent 69/80); vs f00-03 70/80 (+612; parent 69/80); vs f01-07 70/80 (+553; parent
  67/80); vs starter 40/40 (20 maps).
  Final (v3 = v1 + the fixes above): vs f03-05 44/80 (+56); vs f03-04 49/80 (+200).
  v4 (v3 with rewards checked every 4th turn again, as in the parent): vs f03-05 43/80 (−20); not kept.

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
- A global (namespace-scope) cambc::bitboard in a .cpp file can be constructed before the SDK captures the map
  layout: build boards on first use. Raw sdk_* reads (sdk_champion, sdk_camp_group_state, sdk_cast) are safe and
  far cheaper than SDK handles, but return error codes instead of throwing: check them.
- Changes that only move heap allocations can turn a latent out-of-range read into a trap: run every map
  after allocation changes and look at "Engine faults", not only caught exceptions.

## Known weaknesses
- Decisions are the parent's: still loses 00eight (both sides vs every opponent), 00stampede and 00gemfight
  vs f03-04; fights are per champion (square law over visible units), no grouping.
- initialise() overruns the 2x first turn on maps with many camps (camp_groups() is read twice, ~30k each, and
  analyse_map()'s per-camp loop costs ~1.6k a camp): the replay keeps the loss to the first turn.
- Only the runner visits the enemy's altars, and nobody guards our locks. No wards or teleport crystals.
