## Strategy (≤10 lines)
Scaling economy of f02-01 (draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions from draft memory,
value-per-turn camp choice, shards at every spawn visit, boss raids, spells.cpp kit reader) with the defense
subsystem rewritten from scratch in defense.cpp behind the same calls (enemies, tribute, recall_or_walk, go_home).
Threat model: every visible enemy gets burst (one turn, resets and spell), sustained power, reach (range, spell,
walk, dash) and walking distance (one bounded distance field from us), so enemies across holes/walls are ignored.
Fight or flee by the square law (Σhp·Σpower, parent's power estimate) with gem-scaled odds; flee home, recall
when nobody is within 6. Gems: tribute beside an altar whenever enemy damage over min(channel, 3) turns + camps
stays below our health, also in the middle of a fight; long channels are kept unless a stun or a kill is coming.
A tribute of ≥40 gems writes claim byte 255 so allies within 18 come to guard it when an invader was reported.
Channels: hold_channel() runs each channelling turn; a recall is cancelled only when a hit lands next turn.

## Key parameters
- Threat reach: melee 2 (+2 dash), ranged attack range (+2 dash), spell range 4 (Cassi, Ryth, Prism, Stellar,
  Marcella, Ren, Tavi; HoR 3). Burst = str·(1 + reset) + spell mult·focus. Relevant: ≤6 steps from hitting us.
- danger_at(tile, turns): Σ burst + power·(turns − steps − 1) over threats that can hit within `turns`.
- Tribute: hp > camp·(T+1) + danger(min(T+1, 3)) + 40, no unstunned stunner within reach when T ≥ 2 (not pressed).
- Hold tribute unless danger(min(left+1, 3)) ≥ 0.9·hp or a stunner arrives with ≥2 ticks left; never when the
  first hit is next turn or the tribute completes next turn start.
- Fight odds need 1 + min(0.6, gems/80); run alone when next-turn burst ≥ 0.95·hp. Guard bag ≥ 40, radius 18.
- Everything else (choose, farm, shop, spells) unchanged from f02-01.

## Changes this generation
- defense.cpp: new threat model (assess, danger_at, steps_to_hit, first_hit), hold_channel, tribute_now,
  guard; enemies/tribute/recall_or_walk rewritten. bot.cpp: channel early-return calls hold_channel();
  write_claim accepts 254 (guard signal); sighting helpers moved to defense.cpp.
- Long-bag fix: a 1061-gem bag needs a 107-turn tribute; summing enemy damage over the whole channel blocked
  every tribute (boss-battle loss). Danger now looks 3 turns ahead.
- Local results (final settings): vs f00-03 51/80 (parent 56/80 on the same games, mean +191 vs +204 gems,
  deaths 721 vs 747), vs f02-01 38/80 but +51 mean gems (they lose 8440 carried gems, we 6351),
  vs starter 48/48. vs f01-07 40/80 (run with flee home weight 25 in the low-health branch; final uses 12).
  Now wins 01elk-arena both sides and 00contest B (parent 0-gem losses); 0 faults, 0 caught exceptions.

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
- Cancelling a recall whenever an enemy could arrive before it completes: the flee rule (recall when nobody within 6)
  restarted it next turn, so recalls looped and never finished (5110 cancels / 9568 recalls). 48 → 51/80 once the
  cancel waits for a hit next turn.
- Danger-scored flee steps instead of the shortest route home (kFleeMode 1, home weight 25): 45 vs 51/80.
- Recall when fleeing only if the first possible hit is ≥4 turns away (walking reach) plus "cornered: fight back
  when chasers kill us before home": 42 vs 51/80.
- Kit-based power (str·(1+reset/8) + spell·focus/8) for the fight odds: 45 vs 51/80. The parent's 1.1 str +
  0.25 focus stays.
- Recall only where nobody can break it (recall_or_walk): champions walked home across maps instead of recalling
  (islands economy). Off.
- gem_count() works on any *visible* champion (allies and enemies, e.g. an enemy carrying 207); hidden allies
  throw engine error 97, so a big tribute is signalled through the claim byte instead.
- assess() with its bounded distance field (max 14) costs ~3.5k fuel (field ~2.3k); not a fuel problem.

## Known weaknesses
- Not clearly better than the parent: within noise vs f00-03 and f02-01, and 40/80 vs f01-07 (parent notes 58/80).
- Map outcomes flip with any rule change (hooks, elk-arena, contest are snowball maps decided by early fights).
- Still loses 00road-of-riches, 00interlock A, 01fracture A to f00-03; slow economy on some B sides.
- No altar locks of our own, no wards, no teleport crystals; the guard signal shares the claim bytes.
