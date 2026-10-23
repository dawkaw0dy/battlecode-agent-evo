## Strategy (≤10 lines)
Scaling economy, built from the starter infrastructure (main loop, distance-field pathing). Parent e01-01.
Draft (rewritten): on the draft turn, analyse every camp group (monsters per group, elk/golem health share, path
distance from our spawn) and score all 12 champions; pick the best one still free. Base order follows the gold scalers
(Vesper Vale, Marcella, Vendetta, Varg, Ryth, Tavi, ...); big groups lift Varg/Ryth, heavy camps lift Ryth/Marcella,
long walks lift Vendetta. Static fallback order if analysis throws.
Opening (new): draft memory stores per group (our path distance, enemy path distance). For the first 150 rounds
choose_group multiplies each group's score by opening_bias(): x0.6 when the enemy reaches it >2 tiles sooner
(contested), x1.25 when we reach it >4 tiles sooner (our side). Rest of the bot unchanged from the parent:
group score (3*gems+gold)/(dist+clear+8), Treasure->Jungle->Power shards, bank at 20 gems, recall < 35% HP.

## Key parameters
Opening: kOpeningRounds 150, kContestedBias 0.6 (enemy 3+ tiles closer), kOwnSideBias 1.25 (we are 5+ tiles closer).
Draft memory layout: byte 0 = group count (<=255), then (ours, theirs) u8 path distance per group, 255 = unreachable.
Draft scores: vesper 10, marcella 9.5, vendetta 9, varg 8.5, ryth 8, tavi 7, willow 6, ren 5.5, C&I 5, stellar 4,
prism 3.5, heart 3; varg +1.0*big, ryth +0.8*big +0.5*heavy, marcella +0.3*heavy, vendetta +0.6*far.
Inherited: kBankGems 20, kShopGold 150, recall 35% HP, spawn heal to 90%, danger 0.75*HP.

## Changes this generation
Mutation: subsystem rewrite, focus opening (e02-03). Test: 15 maps (01hooks 00gem-fields 00gemfight 01gold-rush
00corners 00lanes 01big-world 00split 01starfish 00lost-world 00quadrants 00rows 01elk-arena 00packs 01islands), both sides.
- Map-aware draft + draft-memory opening plan + opening_bias hook in choose_group.
  Parent-equivalent vs e00-01: sum margin -773 (v4); with opening bias: +3184 (16/28 valid).
- Robustness: tiles_around no longer uses bitboard::draw_chebyshev (engine error 5 near map edges); bank/farm block
  guarded against a rare engine error 5 seen on 01big-world (fuel-dependent, not reproducible with instrumentation).
- Final (30 games each): vs e00-01 17 wins, +3167; vs e01-01 (parent) 14 wins, +819; vs starter 30/30, +18938.
  0 bot errors, 0 faults from this bot. Parent's worst maps vs e00-01: 01hooks -27.5 avg (was -71.5),
  00gem-fields +264.5 (was -605), 00gemfight +164.5 (was -208.5).

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- e01-01: Tribute before fighting when next to an unlocked altar with >= 5 gems: +761 vs +1331 (lost 00lanes B). Reverted.
- e01-01: Recall at 45% HP: 7/12 but -347 total margin (less farming time). Reverted.
- e01-01: Recall at 28% HP: 6/12, +961, below the 35% baseline. Reverted.
- e02-03: Draft scores led by Vendetta (10) with strong map bonuses (Varg +3*big, Ryth +1.5*big): fine on the 5 worst
  maps (+49.5 mean) but -2402 total on 10 other maps (00packs -920, 01islands -756) vs parent's +862. Keep gold scalers first.
- e02-03: Opening bias 0.4/1.5 (stronger): -85 mean on 5 maps vs -10 with 0.6/1.25.

## Known weaknesses
- Results swing a lot by side on the same map (e.g. 01hooks +660 A / -715 B): contested first camps still decided by
  turn order; opening bias only reshuffles targets, no explicit early contest/avoid logic.
- Draft ignores enemy picks (revealed as we go); no counter-picking. Champion score weights are hand-set, lightly tested.
- No dash travel, no attack resets, no wards, no altar locks, no deliberate fights beyond enemies in range.
