## Strategy (≤10 lines)
Parent e06-05 (spell planner, camp subsystem, draft-time map plan) with the defense subsystem (guard_life,
bank_gems, place_lock, hit_lock, buy_lock) rewritten from the patch 100-102 rules behind the same interface.
A per-turn threat model (`threat(enemies, tile, turns)`) estimates damage on a tile over N of our turns: enemy
champions approach one tile per turn then basic-attack, one 2 x focus burst if within 4 + N, channeling (tributing)
enemies count zero, awake camps only within their own attack_range. Tributes start when that threat over
ceil(gems/10) turns stays below 1.2 x HP (damage does not cancel a tribute, only death/stun/lock do).
Bank threshold falls with value at risk (wary 15, HP < 50% -> 10), and banking is forced when the end of the game
is within travel + tribute + 30 rounds or the bag wins outright. Altar choice: path + lock hits (x0.6 ranged) +
contest penalty. Our lock goes first on an altar an enemy tributes at (cancels it), then the one we tribute at.

## Key parameters
Recall: HP < 35%, no enemy champion or awake camp within 6 (parent rule; stricter variants lost). Hurt next to an
altar: tribute if threat over its duration < HP. Tribute at altar: gems <= 15 or threat < 1.2 x HP.
Bank: plan().bank_gems, wary (enemy within 8 in last 25 rounds) plan().wary_bank, HP < 50% -> 10, altar within 5 and
>= 5 gems, late = round + 1.25 x altar distance + tribute turns + 30 >= 2000. Altar cost: enemy within 4 of it
+6 + strength/10 (+3 if it is tributing). Lock placement: +1000 tributing enemy, +200 other enemy, +150 our altar.
Shared memory unchanged: 0..1 team target, 2..5 focus target, 6..7 enemy-lock mask.

## Changes this generation
- Defense subsystem rewritten (see above); rest of the bot unchanged. Flags kDenyWalk/kWalkHome/kBagFloor/kCloseBank
  keep the tested-and-rejected variants switchable (all false).
- Results (benchmark.py/bench2.py, both sides, release, ~104 games total, 0 faults / 0 bot errors):
  final build vs e06-05 (parent) 3/8 +15 on 01elk-arena/00gemfight/00split/00lanes; previous step 8/16 -49 on
  8 maps (islands +15, road-of-riches -104, split -140, elk-arena -144, gemfight +13, contest 0, lanes +13, hooks -44).
  Almost every map is side-decided in the parent mirror too (00gemfight, 01elk-arena, 00split: side B wins).
  vs e03-07 4/8 -137 (elk-arena +762, contest 2/2, islands -308, road-of-riches -1004); vs starter 3/4 +484.

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
- (e07-02) Walking an altar-lock carrier up to 7 tiles to an enemy tributing (deny walk) and walking a hurt champion
  home when a recall is unsafe: 00gemfight side B -277 vs parent (+442 without). Both off (kDenyWalk, kWalkHome).
- (e07-02) Raising the recall health floor with a big bag near enemies plus a strict recall-safety rule (no enemy within
  spell reach + recall time): 00lanes/00split/01hooks -212 vs parent; the parent's (35%, nothing within 6) gives -92.
- (e07-02) Bank threshold 10 with an enemy champion within 6 (kCloseBank): neutral (-47 vs -49 on 8 maps); off.
- (e07-02) Tribute only when the threat over ceil(gems/10) turns < 0.85 x HP: 01elk-arena side A died with 144 gems,
  0 tributed. Threat model is pessimistic (all enemies focus us), so tribute at < 1.2 x HP or <= 15 gems: -1008 -> -432.


## Known weaknesses
- 00road-of-riches: collects 44-130 gems a game (camp choice/plan, not defense); loses to everyone.
- 00split side A: collects ~4 gems in 2000 rounds (parent side A ~99); not investigated.
- 01islands: loses to e03-07 by ~300 on both sides.
- Results vs the parent are dominated by side: one decision flips a game by +-900; the threat weights are lightly tuned.
