## Strategy (≤10 lines)
Parent A e05-05 (draft-time map plan, map-dependent draft, respawn-aware camp subsystem, Lanchester engage,
lock-aware banking) with a reworked spell planner (focus: spells). Parent B e05-01's spell code turned out to be
identical to A's (both inherit e04-06's planner), so nothing could be ported; B's edge on 01islands/01hooks/
00road-of-riches comes from its camp subsystem (A barely damages camps on 01islands), which the task keeps from A.
Spell changes: stuns go to enemies channeling a tribute (a stun cancels it), any damage spell to recalling enemies;
farming champions open sleeping camps of their own target group from spell range (never Golem/Monolith, not while
banking or with enemies within 7) and may hold position for an exclusive camp cast; the dash slot (Vesper Vale, Ren,
Issac, Vendetta) is used to escape 2 tiles when hurt under threat or retreating from a lost fight.

## Key parameters
Tribute interrupt bonus 300 + 6 x gems (stun must land: not Marcella, not already stunned); recall interrupt
80 + 3 x gems. Dash escape needs >= 2 tiles gain in min distance to threats; landing free, mid tile not wall/occupied
(holes allowed). Retreat dash only with nearest enemy <= 3. Everything else as in e05-05 (plan thresholds, bank
30/15, kRecallHealth 0.35, kShopGold 150, cast threshold score 5).

## Changes this generation
- cast_spells: tribute/recall interrupt bonuses; `opener` lets camp spells target sleeping camps of target_group;
  `hold` lets exclusive camp spells fire before a step while farming.
- dash_away (new) used in guard_life and in engage's losing retreat.
- Results (benchmark.py, release, both sides): vs e05-05 (A) 10/16 +135 on 01islands, 01hooks, 00road-of-riches,
  00gemfight, 01elk-arena (2/2 +864), 01fracture (2/2 +181), 00lanes, 00split. vs e05-01 (B) 2/8 -479 on 01islands,
  01hooks, 00road-of-riches, 00gemfight (A baseline 2/8 -538). vs e03-07 4/8 +4 (A: 3/8 -85) and vs e04-06 4/8 -9
  on 00road-of-riches, 01islands, 00lanes, 01elk-arena. 0 faults, 0 bot errors. 56 games used.

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
- (e06-02) Porting B's spell handling: no-op, the spell planner/fight/engage code of e05-01 and e05-05 is identical.

## Known weaknesses
- 01islands: loses to B and to e03-07/e04-06 by 200-1000; A's camp code hardly farms there (B's farms 10x more
  monster damage). Fixing it needs B's choose_group, outside this generation's focus.
- 00road-of-riches side A and 01hooks side B lose ~1000 to B and to e03-07; 00lanes still roughly even.
- Golem groups: no coordination (00split). Attack resets still only fire right after our own basic attack.
