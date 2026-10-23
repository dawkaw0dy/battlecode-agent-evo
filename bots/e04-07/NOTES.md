## Strategy (≤10 lines)
Altar control with a precomputed opening. The draft reads the whole map: it picks Marcella (2 gold per tributed gem,
stun-immune) and Vesper Vale (33% gold), then Varg if at least a third of the camp groups have 3+ camps (splash) or
Vendetta otherwise, then Ryth/Tavi/Willow. It writes the opening into draft memory: a greedy clearing route (best
(3*gems+gold)/(path+clear+8) chained from our spawn, no Golem/Monolith) and the altar lock order (nearest first).
In play every champion buys an altar lock (10g) before anything else, locks any free altar within 2 tiles, and the
team follows the draft route (x4 group score) for the first 400 rounds. Champion of rank r (by type) locks altar r of
the plan in the opening; rank 0 is the warden that later re-locks altars seen without our lock (recalls for a lock).
Banking treats only enemy locks as blocked and breaks them when every altar is enemy-locked. The parent's spell
planner and combat are kept. Why: the parent skipped every locked altar and so scored 0 against e03-07's locks,
and the panel bots skip locked altars, so our locks deny their tributes.

## Key parameters
Draft memory (draft.h): byte 0 magic 0xE7, 1 route length, 2..9 route group+1, 10 altar count, 11..26 altar+1.
Shared memory: bytes 0-1 team target, 2-5 focus target, 6 open-altar mask (altars 0..7), 7 route progress.
Route team-dps estimate 250; route bonus x4 until round 400; opening lock duty until round 200. Shop: lock, Treasure,
Jungle, Power. Rest as in e03-05 (bank 30 gems, recall 35% HP, team target x2.5).

## Changes this generation
Radical mutation from e03-05: map-aware draft, draft-memory opening plan (route + lock order), altar locks (buy,
place, warden re-lock), lock-aware banking (own locks usable, enemy locks broken), first-turn rank.
Results (local, release, both sides; 96 games): vs e03-07 7/12 (-63; parent 3/11, about -500 on the same maps),
vs e03-05 10/12 (+312), vs e02-01 9/12 (+408), vs e01-01 10/12 (+619), vs e00-01 11/12 (+630), vs starter 12/12.
Warden fetching locks right after the opening (not only after round 200): vs e03-07 6/12 -116 -> 7/12 -63.
No engine faults or caught exceptions.

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
- (e04-07) Opening lock duty alone does little: champions start with 0 gold, so the first locks come only after
  the first spawn visit. The warden fetching a lock early helped more.

## Known weaknesses
- 01islands loses to every panel bot except the starter (-200 to -700): not investigated (reachability/route?).
- vs e03-07: 00corners both sides (-643/-810), 00lanes B, 01hooks B: its warden clears our locks; no altar guard.
- Locks are only placed by champions that visit spawn; no wards, dashes or crystals used.
