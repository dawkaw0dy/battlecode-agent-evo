## Strategy (≤10 lines)
Parent e03-05 (spell-first pack: Vesper Vale, Ryth, Tavi, Marcella, Willow; spell planner; Lanchester engage) with
the defense subsystem rewritten (bank_gems + new guard_life). Altars are chosen by path length + turns to break an
enemy lock (hits_remaining + 2) + 8 per enemy champion within 3; enemy locks seen by any ally are shared in memory
(bytes 6..7, u16 mask). Enemy locks are basic-attacked (ranged from 3 tiles) instead of skipping the altar forever.
We buy an altar lock (10g) before shards and drop it on altars within 2 that have no lock, first on one an enemy is
tributing at (placing a lock cancels that channel). A tribute starts only if the visible enemies cannot kill us during
its ceil(gems/10) turns (bags <= 10 always go in); otherwise step back. Banking starts at 30 gems, 15 when an enemy
was within 8 in the last 25 rounds, 10 when under 50% HP. Under 35% HP next to a usable altar we tribute (damage does
not cancel it) instead of recalling; otherwise recall when nothing threatens us, else back off.

## Key parameters
kBankGems 30, kWaryBankGems 15 (enemy seen within 8 in last kEnemyMemory=25 rounds), hurt-bank 10 gems at <50% HP,
kRecallHealth 0.35, lock reach 2, enemy-lock cost hits+2, enemy near altar +8, tribute safe if predicted damage
< 0.9 x HP (predicted: strength x turns in reach + 2 x focus per visible enemy; awake camps strength x turns).
Shared memory: 0..1 team target, 2..5 focus target, 6..7 enemy-lock mask.

## Changes this generation
Subsystem rewrite (defense): bank_gems rewritten; recall block moved to guard_life(); buy_lock/place_lock/hit_lock new.
Cause of the parent's 0-gem losses vs e03-07: it treated every visible lock as permanent and never tributed.
Results (local, deterministic, 78 games): vs e03-07 parent 1/8 (-710) -> 6/8 (+380) on 00fragment, 01broken-curve,
00quadrants, 00road-of-riches. On 01broken-curve, 00quadrants, 00road-of-riches, 01gold-rush, 00lanes, 01the-farm:
vs e03-05 (parent) 7/12 (+152), vs e02-01 7/12 (+297), vs e01-01 9/12 (+395), vs e00-01 11/12 (+357),
vs starter 12/12 (+483). 0 faults, 0 bot errors.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- (e02-05) Reading champion_at(tile).health() right after our attack killed the unit throws engine error 97 even when
  has_champion(tile) is true: check the handle's is_visible() first and guard each combat block locally.
- (e03-05) Team-target bonus 4.0 (strict deathball): 7/14 vs e02-01 (too little parallel farming); 1.5: 10/16. Keep 2.5.
- (e03-05) Spending spells on camps while enemy champions are within 7: 12/16 (+252) vs 13/16 (+346) when saving them.
- (e03-05) Ryth-first spell draft without Vesper Vale: 11/15 vs e02-01; Vesper first is better (13/16).
- (e03-05) `bitboard::draw_chebyshev` near the map edge raises engine error 5 (00lanes, 30 tall): set tiles manually.
- (e04-06) Skipping enemy-locked altars (parent behaviour): 0 tributed gems vs altar-lock bots (e03-07). Break them.

## Known weaknesses
- 01the-farm loses to the parent both sides (309-596, 715-1017; 13 deaths on B, 267 gems lost) and to e02-01:
  the lower wary bank threshold / lock gold may cost farming there; not tuned (no time).
- 00road-of-riches A still loses to e03-07, e03-05, e02-01, e00-01; 00lanes B and 01gold-rush B narrow losses.
- No ally-tribute escort; invaders near our altars are only answered by lock placement and the normal engage logic.
