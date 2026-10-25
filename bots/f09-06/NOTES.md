## Strategy (≤10 lines)
Altar control on top of a from-scratch economy (immigrant from the starter; only main.cpp's loop is kept).
Draft Varg, Vendetta, Marcella, Vesper Vale, Tavi. Each champion picks its own camp group and posts it in shared memory
(bytes 0-4); a group is scored by its share of value per turn at its best team size (up to 2 extra helpers), with
camps' damage turned into recall trips, so safe groups get split and strong ones gather helpers (rally before waking).
Groups that block every altar are team raids. Banking picks one altar by walking distance + a lock-breaking cost and
breaks enemy locks by tributing into them. Altar control: every champion carries a lock, locks any altar the enemy can
reach as easily as we can when passing it, cancels enemy tribute channels and shuts altars in front of gem carriers;
our own lock guards our tribute. Fights: an 8-turn focus-fire simulation with approach delays; short chases only;
flee home (dead ends penalised, fight back when cornered); go heal below half health when enemies are about.

## Key parameters
- Locks: kUseLocks, kPreLock (on), kPreLockSlack 10, kLockGems 15, kLockRange 2, kLockDetour 0 (off).
- Banking: want at 45 gems, or 15 within 8 steps, or round 1800+, or our score + gems ≥ 1000; kBreakCost 12, kBankStick 6.
- Targets: kExtraHelpers 2, kCallCost 10, kRallyTurns 12, kOtherSectorValue 0.85, kForeignValue 0.5,
  kTankShare 0.8 (max hp ≥ 0.8 × group strength × (range + 2)), kGateTankShare 0.4, kGateValue 400, kCaptureGiveUp 12.
- Fights: kSimTurns 8, kEngageValue 150, kFleeValue 100, kChaseReach 2, kChaseTurns 4, kDeepChase 7.
- Health: recall below 30% (40% near enemies, 50% if an enemy was seen within 12 tiles in 10 rounds); leave camp range
  when incoming hits × (escape turns + 2) ≥ hp; no recall with an enemy seen within 10 in the last 3 rounds.
- Shared memory: 0-4 group targets per draft slot, 5-7 latest enemy sighting (x, y, count<<6 | round/4).

## Changes this generation
- New bot. Nav: per-group / per-altar terrain fields filled with spare fuel, host search with units near the goal,
  stick with the unit-aware route 8 turns, avoid a tile where a hidden unit blocked us. Fuel: one target choice per
  turn, shared memory cached per turn; late fuel overruns 53 → 12 per 20 games.
- Measured (20 games each, release, both sides): lock-aware banking vs f03-05 2/20 → 9/20 (we collected but could not
  tribute through their locks). Locks vs f05-02 (10 maps): none 8/20 (they tribute 14.9k), reactive 11/20 (13.9k),
  + pre-lock 11/20 with mean +195 (12.4k, ours 16.3k). So locks did cost them gems.
- Heal-at-half-health rule vs f05-02: 6/20 → 11/20, deaths 326 → 240. Fight simulation: deaths 406 → 326.
- Dynamic helpers vs single pack vs fixed sectors (vs f00-03, 10 maps): 15/20 vs 4-9/20 vs 5/20.
- Late builds (10-map sets, 20 games each): starter 18/20 then 10/10 (the 2 losses were 0-gem games on
  01yellow-brick-road, fixed by the capture give-up), f00-03 8/20, f02-01 14/20, f05-02 11/20, f01-07 9/20;
  final build: f04-05 4/20 (−304), f08-04 2/20 (−353).
- 0 faults and 0 `bot error:` lines from our side in every benchmark. Game budget: 881 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- One pack of all champions: walks together between 2-turn kills (lost-world clusters); 4-9/20 vs f00-03.
- Fixed angular sectors per champion (no joining): long walks, dangerous groups never taken; 5/20 vs f00-03.
- Valuing every boss as a full-team raid: mean rounds-to-win vs starter 1341 → 1824. Only gates are raids now.
- Camp tiles as walls in the cached fields: altars behind a camp corridor became "unreachable", banking stopped.
- Keeping farming/banking steps out of enemy reach all the time (kUseDanger): 0/20 vs 2/20 vs f08-04.
- Retreat at 60% instead of 50% near enemies: 8/20 vs 11/20. Lineage draft order (Vendetta, Vesper, Marcella, Tavi,
  Varg) on 3-4 champion maps: 9/20 vs 12/20. Walking up to 6 tiles to lock an altar: 8/20 vs 11/20.
- Long chases: the enemy retreats to its spawn (heals 100% per round) or kites; now only targets within range + 2.
- Recalling with an enemy 5 tiles away: killed mid-channel. Fleeing away from enemies into a dead end: died in place.
- Debug builds diverge from release (fuel thresholds): judge only on release multi-map runs.

## Known weaknesses
- Far behind f04-05..f08-04: they farm about twice our camp damage and kill us 2-3 times per death of theirs.
- Melee champions (Marcella, Vendetta, Varg) die most; snowball maps (01gold-rush, 00swirl, 01shortcut) are lost early.
- Narrow camp corridors (01corridors) still cost many camp deaths: teammates block the tank's retreat.
- Groups on hole islands are skipped (no dash/teleport-crystal capture). No wards.
- Lock memory is per champion; only one lock carried at a time.
