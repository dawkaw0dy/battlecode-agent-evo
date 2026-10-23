## Strategy (≤10 lines)
Camp farming and altar control unchanged from the parent (e04-04): every champion buys an altar lock and locks
altars it passes; one warden re-locks cleared altars; the others farm camps with the e04-04 group scorer.
This generation rewrote the endgame subsystem (observe_altars / place_locks / clear_enemy_lock / warden_duty /
bank_gems, same calls from play_turn):
- Altar memory: an enemy lock seen within 150 rounds still counts when the altar is out of sight (no bouncing).
- Bank trigger: winning bag (score + gems >= 1000), time left < 1.5*distance + gems/10 + 30, else bag >= 20,
  >= 10 with an enemy within 7, >= 8 below 60% HP, >= 5 within 4 of an altar.
- Altar choice: cost = 1.25*distance + 6 if enemy-locked + 12 per enemy within 3, hysteresis 4, skip 60 rounds
  after 15 turns without progress. Locks are broken with basic attack + attack reset (5 hits).
- Denial: lock placement prefers altars an enemy stands next to (cancels its tribute); with a lock, < 5 gems,
  > 70% HP and not outnumbered, walk into lock range of such an altar.

## Key parameters
kBankGems 20, kLockMemory 150, kLockClearCost 6, kEnemyNearCost 12, kAltarHysteresis 4, kBankStuck 15,
kFallthroughFuel 60000 (end the turn instead of falling through to farming when a second path field is unaffordable).
Unchanged: kShopGold 150, recall 35% HP, kLockRange 2, kRelockAfter 120, camp parameters of e04-04.

## Changes this generation
Subsystem rewrite (endgame), all via tools/bench2.py (both sides, release), 64 games total:
- Parent vs e04-06 baseline on 01elk-arena 01hooks 00lanes 00corners: 5/8, mean -120.
- Final vs e04-04 (parent) on 01elk-arena 01hooks 00lanes 00corners 01the-farm 00pilgrimage: 6/12, mean +69.
- Final vs starter 4/4 (+836), vs e03-07 3/4 (+268) on 00lanes/01elk-arena or 01hooks.
- Earlier build vs e04-06: 01elk-arena B 756-900 / 876-780 (baseline 336-1014); 01hooks A 1001-11 once the
  capture deferral was removed.
- 0 faults, 0 bot errors in the final runs (one engine error 5 from fuel suspension fixed by kFallthroughFuel).

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
- Deferring a tribute trip while a damaged target group is close (finish the clear first): 01hooks A vs e04-06
  went from a win to 561-1001. Removed.
- Walking toward enemy-tributed altars to lock them at >= 50% HP with gems carried: more deaths; restricted to
  < 5 gems, > 70% HP, not outnumbered.
- Falling through from a failed bank move to farming with low fuel: a second distance field suspended the bot
  and pick_target threw engine error 5.

## Known weaknesses
- Losses on 01elk-arena and 01hooks side B are combat losses (30-50 deaths per game), not banking problems;
  the endgame subsystem cannot fix them.
- Vs the parent the result is mixed (6/12): the endgame change trades 00lanes B / 00pilgrimage B for
  01the-farm and 00corners margins.
- Enemy tributes are only detected by an enemy standing next to an altar.
- g_stage diagnostic string is appended to `bot error:` lines (stage and round) for debugging.
