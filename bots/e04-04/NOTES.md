## Strategy (≤10 lines)
Altar control from the parent (e03-07): every champion buys an altar lock first and locks altars it passes; one
warden re-locks altars the enemy cleared; others farm camps and bank gems at altars free of enemy locks.
This generation rewrote the camp subsystem (observe_groups / choose_group / farm_camps / camp part of pick_target):
- Per-group memory: remaining health from visible camps, reward waiting (cleared, uncaptured), respawn round.
- Score = (gems * (3 + round/700) + gold * (1, 0.4 after round 1500)) / (approach + clear + capture + 2),
  approach = max(path - attack range, wait for respawn); groups needing > 25 turns of waiting are skipped.
- Danger: expected damage vs health above the recall threshold, score x (budget/damage)^2 (min 0.02).
- Wait next to (never on) a group that is about to respawn: an occupied spawn tile delays the whole group.
- In-range camp order: awake first, then target group, then highest strength per health; clusters last.

## Key parameters
kBankGems 20, kShopGold 150, recall 35% HP, kLockRange 2, kRelockAfter 120 (unchanged from the parent).
Camps: kChooseEvery 1, kFieldFuel 80000 (re-choose only when the path field is affordable), kScanFuel 15000
(stop scanning groups before fuel runs out), hysteresis x1.3 on the current target, rivals > helpers x0.5,
helpers = allies within 8 of us or within 2 of the group.

## Changes this generation
Subsystem rewrite (camps), all via tools/benchmark.py (both sides, release), final version:
- vs e03-07 (parent) on 01elk-arena 00lanes 00pilgrimage 00corners 01hooks 01the-farm 00contest 01big-world:
  13/16, mean +308 (parent's worst map 01elk-arena now +420 avg; 00lanes +283).
- vs e02-01 on 00lanes 00contest 01the-farm 00corners 01hooks 01islands: 7/12, mean +274.
- Earlier build (threshold danger) vs e01-01 7/8, vs e00-01 6/6 valid, vs starter 6/6.
- 0 engine faults, 0 caught exceptions from this bot in the final runs (e00-01 throws its own on 00lanes).

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; only
  cast them right after our own basic attack this turn.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- `vision::has_altar_lock` is true for our own locks too: use `altar_lock_at(...).is_ally()` before skipping an altar.
- Running out of fuel mid-turn (100k) suspends the bot; on resume, engine calls throw error 5 "Invalid metadata,
  alignment, overlap or argument" at random places. A full-map distance_field costs ~50-60k on 70x70 maps. Fix:
  check `cambc::fuel_remaining()` before the field and inside the group scan. bitboard::positions() is costly:
  precompute group tile lists once.
- `vision::has_camp(tile)` threw error 5 on in-vision spawn tiles; use `vision::camp_tiles().is_set(tile)`.
- Waiting ON a respawning group's spawn tile blocks its respawn forever (the champion idles): -700 gems/game on
  00corners until fixed.
- Re-choosing only every 6 turns, a Chebyshev fallback when fuel is short (targets flip between metrics), and an
  "ally already there" x0.6 factor: champions flip targets and walk more than they clear. Reverted.
- Skipping sleeping camps of non-target groups (to avoid waking them): -400 gems on 00lanes/00corners. Reverted.
- Parent-style danger (damage > 0.75 HP -> x0.05) vs the smooth budget: 8/12 +162 vs 10/12 +410 vs the parent.
- Lowest-HP-first camp order (parent) vs strength/health order: no gain.

## Known weaknesses
- 0-0 games (00contest, 01the-farm vs some bots) are decided by tiebreaks; 01the-farm still loses to e02-01.
- No multi-champion coordination for golems/monolith (monolith skipped); allies only count as helpers when near.
- 00corners side A and 01elk-arena side A still lose narrowly to the parent.
- g_stage diagnostic string is appended to `bot error:` lines (stage and round) for debugging.
