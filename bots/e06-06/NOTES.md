## Strategy (≤10 lines)
Opening-first rework on the parent's (e05-01) infrastructure: movement, combat, banking and camp scoring are kept;
the draft and the first ~150 rounds are new. The draft turn (10x fuel, full map) computes distance fields from both
spawn regions and stores a plan in draft memory (magic 0xE6): per camp group the side margin (enemy path distance
minus ours) and one opening group = best value / (our distance + 8 + 0.1 x camp strength) among non-monolith groups
at least 3 tiles closer to us than to the enemy. Champions read the plan on their first turn and multiply that group's
score by 3 until it is taken (respawn seen) or round 150, so the pack opens on a safe home-side camp instead of
racing into the contested centre where every panel bot's identical stun pack waits.
Draft: Vesper Vale, Ryth, Marcella, Willow, Tavi (Marcella and Willow ahead of Tavi): stun immunity and heals make
the first fights against the Vesper/Ryth/Tavi stun packs of the panel go our way instead of trading stuns.

## Key parameters
kOpeningMargin 3 (draft.h), kOpeningBonus 3.0, kOpeningRounds 150 (bot.cpp). Draft memory: [0] 0xE6, [1] group
count, [2] opening group + 1, [3..] int8 side margin per group (read only if the group count matches the map).
Everything else as the parent (kTeamBonus 2.5, kStickiness 1.3, bank 30/15, shared memory 0..7 unchanged).

## Changes this generation
New draft order and draft-time opening plan (draft.cpp), plan reader + opening bonus in choose_group (bot.cpp).
Results, deterministic local, 8 maps (00rushbait-v2 01elk-arena 01hooks 00ambush 00packs 01big-world 00gemfight
01broken-curve): vs e05-05 8/16 (+122) (parent 4/16, -116); vs e04-06 11/16 (+279); vs parent e05-01 8/16 (-25).
On 00split 01starvation 00road-of-riches 01gold-rush: vs e03-07, e02-01, e01-01, e00-01, starter 40/40.
Parent's worst losses: 01elk-arena A vs e05-05 now +612, 01hooks B vs e04-06 now -440 (still lost), 00rushbait-v2 A
vs e04-06 -99 (was -1009). 0 faults, 0 bot errors. 293 games used in total.

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
- (e06-06) All vs e05-05 on the 8 maps above (16 games each): opening group allowed at margin 0 (the centre: rush it)
  with the parent draft 3/16 (-226); opening margin 6 5/16 (-112); opening bonus 5.0 4/16 (-247); keep margin 3, x3.
- (e06-06) Counter-draft (Marcella first once the enemy shows 2 stunners) with the parent draft: 3/16 (-138), no gain.
- (e06-06) Drafts without the opening plan: e03-07's sturdy order (Marcella, Vesper, Vendetta, Ryth, Varg) 2/16 (-451);
  Vesper, Willow, Ryth, Tavi 4/16 (-55); Vesper, Willow, Ryth, Marcella 4/16 (-226); Vesper, Ryth, Marcella, Willow 5/16 (-42).
- (e06-06) Discounting enemy-side groups (margin < -4, x0.4 before round 300): games identical, never decisive; removed.

## Known weaknesses
- Results are chaotic: one parameter change flips whole maps (first fight decides the game). 00ambush (0/2) and
  00packs B still lose to e05-05; 00packs 0/2 and 01elk-arena B lose to the parent.
- The opening group is chosen from static distances; it ignores which camps the enemy will actually take first.
- Symmetric maps with all rich groups in the centre get no opening group (margin < 3 everywhere): parent behaviour.
