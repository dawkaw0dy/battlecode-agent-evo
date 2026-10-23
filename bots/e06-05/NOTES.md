## Strategy (≤10 lines)
Parent e05-05 (e04-06 spell pack + e04-04 camp subsystem + draft-time map plan in draft memory) with the spell
subsystem (`Bot::cast_spells(bool stay)`) rewritten from the patch 100-102 kits behind the same interface.
The planner scores every ready (slot, target) pair in one health-equivalent unit and casts the best:
enemy champions by mitigated damage (`damage_amount(target)`) x kill worth, a kill bonus when spell plus this
turn's basic attack finish them, stun bonus (more on an enemy channeling tribute/recall, which a stun cancels);
awake camps by damage, killing blow and the hit a stun prevents; area spells try every centre within radius of a
target; heals pick the most effective heal; shields go up whenever damage is incoming.
New: travel dashes (Vesper Vale, Vendetta, Ren, Issac never dashed before): `dash_along` spends a ready dash
slot on two tiles of the path field inside `move_towards_closest`, then the normal step follows (3 tiles/turn).
The rest of the bot (camps, banking, engage, draft, plan) is unchanged.

## Key parameters
Champion worth 250 + 3 x gems; kill bonus 400 + 3 x gems; stun +120, +100 + 4 x gems on a tributing enemy,
+60 recalling (+40 for damage on a recalling enemy); team focus target x1.5.
Camp: damage x 0.15, +15 for the killing blow, stun = strength x 0.8 (in camp range of us) or x 0.4.
Heal: effective heal >= min(120, 0.4 x heal) or ally < 45% HP; score 0.3 x heal, x2 when low.
Shield: incoming = awake camps within their range + 1, enemy champions within 4; score min(shield, 4 x incoming) x 0.2.
Cast threshold 8. Camp casts wait while enemy champions are within 7 (kept from the parent, tested by e03-05).
Dash: only full two-tile gains, landing free and reached by the field, middle tile not a wall; off on large maps
(plan size_class 2), within 15 rounds of an enemy sighting, below 60% HP, or with an enemy champion within 8.
Shared memory unchanged: 0..1 team target, 2..5 focus target, 6..7 enemy-lock mask.

## Changes this generation
- cast_spells rewritten (see above); new `dash_along` member called from move_towards_closest (cast_spells
  call sites unchanged).
- Results (benchmark.py, both sides, release, 256 games total, 0 faults / 0 bot errors on our side):
  vs e05-05 (parent) 12/20 +235 (10 maps incl. its worst: road-of-riches +146, hooks +198, elk-arena +684,
  lanes +284, gemfight +857) and 9/16 +90 on 8 fresh maps; final gated build 6/12 +77 on 6 maps.
  Final build: vs e03-07 13/16 +275 (8 maps); vs e02-01 11/12 +612 (6 maps). Before the dash sighting gate:
  vs e03-07 8/8 +370 on large maps; vs e04-06 7/12 +36, e01-01 7/12, e00-01 5/10 valid, starter 7/12 +360
  (6 maps incl. road-of-riches/islands, which all lose).

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
- (e05-05) B's camp code gated on `fuel_remaining() > 80000` inside A: A spends most fuel before farming, so the choice
  never re-ran, state went stale and champions idled (-1000 gems on most maps). Gate by map area instead.
- (e05-05) B's sturdy draft everywhere: lost A's strong maps (01elk-arena -775 vs A). Spell draft everywhere: 01fracture
  -47 vs B (sturdy +216..+360). Hence the map-dependent draft.
- (e05-05) Pack 2.5 everywhere: 00lanes -278 vs A (pack 1.0: -35..-250). Pack 1.0 on 00gemfight: lost side A -750.
- (e05-05) Danger damage / min(helpers, 2) instead of / helpers did not fix 00split (golem group drains HP); kept as safer.
- (e06-05) Travel dash on large maps (sturdy draft, pure farming races): vs parent 2/8 -56 with dash vs 6/8 +93 without
  on 01fracture/01big-world/00expanse/01the-farm; vs e03-07 neutral (+370 vs +390). Dash is off on large maps.
- (e06-05) Ungated dash on PvP maps (00contest): champions leap into unseen enemies; 83-102 deaths vs parent. Gated by
  recent enemy sighting (15 rounds) and HP >= 60%; vs e03-07 +249 -> +275.
- (e06-05) Single reversions to the parent's planner pieces (heal rule, area centres on unit tiles only, raw damage
  and worth 200, threshold 5 + camp stun 0.5) did not change 00contest/00ambush vs parent: those maps are
  side-chaotic snowballs in near-mirror matches (parent vs parent: side A wins 100 kills to 6).
- (e06-05) Stronger shield rule (shield as soon as damage is incoming, no HP gate) vs the parent's (< 75% HP): parent's
  gate measured worse on large maps (-78 vs -56 vs parent), so shields go up early.

## Known weaknesses
- 00road-of-riches: we collect only 44-130 gems a game (0 deaths), even the starter wins; not spell related
  (camp choice / plan on a cluster-heavy 40x40 map), not investigated.
- 01islands: loses to e02-01/e04-06/e01-01 by 300-900; e02-01 farms 1000 there by walking (no crystals).
- 01the-farm: nobody tributes in 2000 rounds; decided by gold.
- 00contest / 00ambush vs the parent: side-dependent snowballs; we lose some.
- Results vs the parent are noisy (one decision flips a game by +-900), so the planner weights are lightly tuned.
