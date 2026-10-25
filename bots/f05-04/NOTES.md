## Strategy (≤10 lines)
Parent f04-03 (structure and strategy unchanged): defense-first economy with altar locks, deny, tribute under fire,
square-law fights, f04-03's kill-by-kill camp simulation and raids; draft Vesper Vale, Vendetta, Marcella, Tavi, Varg.
This generation (parameter tweaks, focus defense) adds small rules on top:
- Doomed retreat: run when the enemies that can hit us next turn would kill us within 2.5 turns, whatever the team odds.
- Recall at 35% health (not 20%) when an enemy was seen in the last 15 rounds or an ally reported one within 12 tiles.
- Unseen attacker: hit with no awake camp in reach and no living camp within 5 tiles → walk home when hurt/carrying.
- Answer an ally's enemy sighting from 15 tiles (was 10); bank at 30 carried gems (was 45), 5 with enemies about (10).
- A boss beside the only altar (01boss-battle: every altar spot is under a double monolith) is raided by the whole
  team via the rally ring; the parent planned it solo, the sim failed and nobody ever tributed (0 gems all game).

## Key parameters
- kDoomTurns 2.5: reach power = Σ enemy power (1.1·str + 0.25·focus, ½ when recalling, 0 channelling) within range+2.
- kHurtNearEnemy 0.35 (else 0.2); "enemies about" = seen ≤ 15 rounds ago, or shared sighting age ≤ 12 within 12.
- kGhostFlee: unseen hit → last_enemy_seen_ = now; walk home if hp < 60% or gems ≥ 10.
- kHelpRange 15, kHelpAge 4 (sighting ages come in steps of 4), kHelpHp 0.6, gems < 15 (unchanged).
- kBankGems 30, kBankDanger 5 (danger = sighting ≤ 30 rounds within 16, or enemy seen ≤ 20 rounds ago).
- kAltarRaid: planned team = all living champions for an altar plug; blocked plugs set altar_wait_until_.

## Changes this generation
Dev set: 10 maps where the parent lost (00gemfight 01gold-rush 00collision 00stampede 01yellow-brick-road 00swirl
01starfish 00packs 00lanes 01hooks) × 2 sides vs f03-05 + f04-05 = 40 games; parent there 15/40 (−260 mean margin).
Parent results for all maps came from evo-results r04 (reproduced exactly locally). All runs 0 faults, 0 bot errors.
- Altar raid: 01boss-battle vs f03-05/f04-05/f04-04 4/6 (parent 1/6). First version trapped (engine fault, OOB):
  turn() read k_[plug_] after plug_ = -1 (parent UB, see below); fixed.
- kDoomTurns 2.5: 17/40 (−145), PvP deaths −18%/−10%. Kept. 3.5: 12/40; 1.8: 16/40 (−192). Reverted.
- kHurtNearEnemy 0.35: 20/40 (−24), deaths 861 vs 988. Kept. 0.5: 18/40 (−30), same deaths. Reverted.
- Bank 30 / danger 5: 21/40 (+21), gems lost on death −13%. Kept. Bank 45 + danger 5 on dev + 6 farming maps:
  34/64 vs 35/64, lower margins. Reverted.
- Ghost flee: 24/40 (+135), our PvP deaths −22%, enemy deaths +7%. Kept.
- Help range 15: 26/40 (+256), enemy deaths 1336 vs 1036. Kept. Range 20: 26/40 (+222), fewer tributes. Reverted.
- Ghost flee only with no living camp within 5 (camps in bushes hit farmers unseen; 00lanes vs f00-03 0/2 → 1/2,
  three champions had farmed < 40 gems): dev + 3 farming maps 28/52 vs 29/52 (−4 vs f03-05, +3 vs f04-05). Kept.
- Help hp 0.5 (was 0.6): 23/52 (−2) vs 28/52 (+170). Reverted.
Final version: dev + 01big-world 00expanse 01the-farm: vs f03-05 14/26 (+196), vs f04-05 14/26 (+143).
Other 29 maps (one step earlier): vs f03-05 29/58 (parent 31), vs f04-05 32/58 (parent 27). vs f04-03 (parent):
dev 13/20 (+149, one step earlier), 15 fresh maps 15/30 (+105). vs f02-01 16/20 on 10 maps the parent lost 11/20;
vs f01-07 7/10 (parent 6/10); f00-03 8/10; starter 10/10. 813 games in total.

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
- Pair farming (melee + ranged share the leader's target, plan as a unit of two): vs f02-01 66/80 with fewer gems
  (221 vs 246 by round 600) and no extra kills; vs f03-04 8/20 vs 9/20 alone, deaths 19 vs 14 per game. The pair
  rarely stands together (banking, recalls), so it only splits income.
- Charging every raider the full walk-out reserve: no team could ever raid a double monolith (00rows lost late to
  raiders on both sides). Only the last champion in reach needs it.
- Win counts swing ±5/80 between versions that change nothing systematic (17 map flips each way); compare
  aggregates (gems by round 600, deaths by cause, gold) from the stats files instead.
- f05-04: turn() read k_[plug_] right after plug_ = -1 in the boss-beside-the-altar branch (undefined behaviour the
  parent survived by luck); a new read there trapped (wasm OOB, 100-300 engine faults per game on 01boss-battle).
  Engine faults show up as `botFaults` in the stats and "Guest trapped" in `cambc run` output, not in stdout.
- f05-04: more caution is not safer: doomed retreat at 3.5 turns (12/40 vs 17/40) and recall at 50% (same deaths,
  fewer wins) — a champion fleeing 15–35 tiles home under chase dies anyway and leaves its team outnumbered.
- f05-04: help range 20 (≈ 15, fewer tributes), help at 50% health (23/52 vs 28/52), bank at 45 with the early
  danger bank kept (34/64 vs 35/64).
- f05-04: dev-set gains shrink on fresh maps (dev 15 → 26/40, other 29 maps 58 → 61/116): judge on fresh maps too.
- f05-04: stats JSON events (kind death/gemLoss/gemSubmission, entries[].context/target/timestamp) give deaths by
  killer; replay stdout lines start with NUL padding (strip '\x00' before matching trace lines).

## Known weaknesses
- PvP is still decided by f03-04/f04-05-style fight simulation: 00gemfight, 01gold-rush, 00collision remain losses vs
  f04-05 (25–40 deaths per game); fleeing on foot from ranged chasers over long distances to spawn rarely works.
- Close farming races (00quadrants, 00rows, 01islands, 01big-world) are lost by ~50–150 gems more often than the
  parent: the extra caution (recalls, early banking, answering sightings) costs farming time.
- The square-law odds still count allies within 7 tiles who may not be fighting.
