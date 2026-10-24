## Strategy (≤10 lines)
Parent A e06-05 (spell planner rewritten from the patch 100-102 kits, travel dashes, Lanchester engage, lock-aware
banking, draft-time map plan in draft memory, value/time camp scoring with per-group HP/respawn memory) crossed with
parent B e06-04's camp-access fixes:
- Failed-step memory: a step that returns false (hidden ally/enemy in a bush) blocks that tile for 8 rounds in
  move_towards_closest/move_away_from, so a champion no longer sits on a respawning group's spawn tile retrying it
  (the group never respawned and the team waited beside it all game holding gems: 00road-of-riches).
- Group reachability: once per champion, when a whole-map field fits in fuel, a terrain field from our position marks
  which camp groups are walkable; choose_group skips the others (01islands: the Chebyshev fallback chose groups
  across water and the team idled on the shore).
Everything else (spells, dash, engage, banking, draft, plan) is A's.

## Key parameters
Bad step memory 8 rounds; reachability computed when fuel >= field_fuel() + 20000 (field_fuel = w*h*11 + 12000).
Unchanged from A: champion worth 250 + 3 x gems, kill bonus 400 + 3 x gems, cast threshold 8, camp casts wait
while enemies within 7, travel dash off on large maps / within 15 rounds of an enemy sighting / below 60% HP.
Camps: score = value / (approach or respawn wait + clear + capture), danger factor (budget/damage)^2 (min 0.02),
groups needing > 25 rounds of respawn wait skipped. Shared memory: 0..1 team target, 2..5 focus, 6..7 lock mask.

## Changes this generation
- Ported bad_step_tiles/forget_step and learn_reachability (+ the reachable filter in choose_group) from B into A.
- Results (benchmark.py, both sides, release, 116 games, 0 faults, 0 bot errors):
  vs e06-05 (A) 11/16 +328 on 8 maps: 01islands +1010, 00road-of-riches +805, 01elk-arena +816, 00lanes +178;
  00split -127, 00gemfight/01hooks/00contest split by side.
  vs e06-04 (B) 9/16 +57: 00split +498, 00gemfight +592, 00lanes +298, elk-arena +72; 01hooks -902, islands -132.
  vs e03-07 14/16 +514 (road-of-riches -318 both sides). On road-of-riches/islands/fracture: starter 6/6 +784,
  e02-01 6/6 +648, e00-01 5/6, e01-01 5/6, e05-05 4/6, e04-06 4/6 (all losses on 00road-of-riches).

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
- (e07-04) Travel dash off (B has none) inside the child: vs B on islands/hooks/road-of-riches mean -21 (dash: -335,
  mostly 01hooks), but vs A 8/16 +202 (dash: 11/16 +328) with 01elk-arena -810 and 00lanes -125. Dash kept.

## Known weaknesses
- 00road-of-riches: now farms (beats A by 800, starter, e02-01) but still loses ~300 to e03-07/e04-06 and side A to
  e05-05/e01-01/e00-01; camp order on this cluster-heavy map not tuned.
- 01hooks loses ~900 to e06-04 with travel dash (noisy: without dash it splits by side).
- 00split still slightly behind A (-127); 01islands slightly behind B (-132).
- A group blocked by an enemy standing on its spawn tile is still waited on (respawn timer stays 0).
