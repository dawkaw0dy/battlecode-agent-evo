## Strategy (≤10 lines)
Parent A e05-01 (spell pack Vesper Vale/Ryth/Tavi/Marcella/Willow, spell planner, Lanchester engage, altar lock/defense,
e05-01's camp scorer with live health and per-camp kill-order damage) with a more cautious combat retreat.
Crossover note: parent B e05-05's combat code (assess_balance, cast_spells, pick_target, fight, engage, guard_life) is
byte-identical to A's (both inherited it from e04-06); B differs only in draft/plan/camps/banking, so there was nothing
combat-specific to port. Combat was instead changed in A directly, aimed at the snowball losses (01hooks B: 37 deaths
to 6, mirror draft): a lost fight is always left, hurt champions leave even trades, and allies one step further
count in the balance. Everything else is A's.

## Key parameters
kAllyRadius 5 (was 4), kEnemyRadius 5, kHurtShare 0.5 (leave a non-favourable in-range trade below half health or with
>= 25 carried gems), retreat when losing (ratio < 0.6) with an enemy within kEnemyRadius, no bag/health condition
(was: within 4 and gems >= 10 or health < 60%). Engage bar 1.15 - enemy gems/120 + our gems/100 - 0.2 x stunned.
Camps/bank/recall unchanged from A (kRecallHealth 0.35, kShopGold 150, kBankGems 30 / wary 15).

## Changes this generation
- engage(): unconditional retreat from a losing balance within 5; in-range "even" trades left when hurt or carrying 25+ gems;
  ally radius 4 -> 5 in assess_balance.
- Results (bench2/benchmark, release, deterministic): baseline A on 00rushbait-v2/01hooks/01elk-arena: vs e04-06 4/6,
  vs e05-05 3/6. Final: vs e04-06 4/6 +301 (01elk-arena 2/2 at about +900 each), vs e05-05 5/8 +207 (adds 00gemfight),
  vs e05-01 (parent A) 3/8 -76 (01elk-arena lost both sides in the mirror), vs starter 4/4 +946, e03-07 3/4 +347,
  e02-01 4/4 +934 (01hooks, 00gemfight). 0 faults, 0 bot errors. 70 games used in total.

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
- (e05-01) First camp rewrite scanned every group with bitboard ANDs, per-group vision fetches and ally/enemy
  distance loops: turns ran out of 100k fuel (fuel log full of 100000), 9/28 and worse vs the parent. Keep the scan cheap.
- (e05-01) Solo farming of clusters/wolves (ally closer within 2 takes it, x0.25) plus team target only for groups needing
  a pack: champions split up and died to invading packs (00packs B: 22 deaths, 0 kills). Soft packing stays.
- (e05-01) Team target written only when the old one is captured (hysteresis): no gain; contested-group penalty x0.4
  (enemies within 4 of spawns): lost the contested centre groups (01elk-arena, 00ambush). Neither kept.
- (e05-01) Waiting beside respawning groups (timer <= 30 on arrival) was never shown to help; respawning groups are skipped.
- (e06-07) Retreat radius 4 (unconditional) and fragile = health only (no gem trigger): vs e05-01 1/4 -439 on 01hooks/
  01elk-arena (v2 with radius 5 + gem trigger: better). Kept radius 5 and the 25-gem trigger.
- (e06-07) Ally radius 5 + hurt-trade exit alone (no unconditional retreat): no change on 01hooks B (still ~37 deaths).

## Known weaknesses
- 00rushbait-v2 side A: collects ~30 gems all game vs every opponent (not combat; camp choice/pathing on this 30x80 map).
- 01hooks side B: still ~34 deaths vs e04-06 (mirror snowball; whoever wins the first fights wins), retreat did not fix it.
- 01elk-arena vs parent A: the more cautious retreat cedes the contested centre in the mirror (lost both sides).
- 00gemfight side A loses to e05-05 and e05-01.
