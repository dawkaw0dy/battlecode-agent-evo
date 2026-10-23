## Strategy (≤10 lines)
Camp economy on the parent's infrastructure (e06-04: movement, pathing, failed-step memory, reachability, combat,
banking, draft plan). Group choice is rebuilt around squad sizing from the patch-100 camp tables:
- need = champions the group requires so its tank survives the clear: 1 + (strength x remaining HP / dps) / (max HP x 0.65).
  Gem and gold clusters (120 HP, 0 strength) and most wolf groups need 1; elk groups 2-3; golems the team.
- The team target only pulls champions in when need >= 2 (or the plan packs: small/starved/3-champion maps);
  a group needing more hands than are around is damped x0.35; a solo-safe group with >= 3 allies already near is damped x0.6.
- Capture hand-off: when a cleared group already has an ally on a spawn tile and no enemy was seen for 25 rounds,
  other champions skip it for 10 rounds and walk to their next group (rewards split among those present, five turns each).
Why it should beat the parent: fewer champion-turns wasted standing on captures and crowding solo-safe clusters, and
fewer solo attempts on elk/golem groups that drain HP into recalls (the parent's worst losses were snowballs after early deaths).

## Key parameters
need: full budget = max HP x (1 - kRecallHealth 0.35); clamp 1..team_size.
Team-target multiplier: max(pack, 1.6) when need >= 2 or pack >= 2.0, else 1.0. Under-squad damping x0.35 (x0.6 tested worse).
Crowding damping x0.6 when need == 1 and helpers >= 3. Capture hand-off: skip 10 rounds, only if 25 rounds without enemies
and carrying < plan().bank_gems. Everything else unchanged from e06-04 (bank 30/15, gold weight 1.0, Treasure > Jungle > Power).

## Changes this generation
Mutation "radical new strategy", focus camps. The camp chooser and capture behaviour changed; combat, banking, draft kept.
Results (pool.py, both sides, release; 0 bot errors, 0 faults):
- 12 maps (01elk-arena, 00rushbait-v2, 01broken-curve, 00corners, 00gemfight, 00lanes, 01islands, 00gem-fields, 01the-farm,
  00split, 01starvation, 00road-of-riches) vs e05-05 + e06-05: final 28/48, margin +180; parent e06-04 26/48, +91.
  vs e05-05 alone 12/24 (+133), vs e06-05 16/24 (+226).
- Panel sanity (00ambush, 01big-world, 00pilgrimage): 6/6 each vs starter, e01-01, e02-01, e03-07, e04-06; 2/6 (-117) vs e06-04.
340 games used in total.

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
- (e06-04) None of my own failed this generation; untested ideas: skip a group whose respawn timer stays 0
  (blocked by an occupant) for many rounds; vary banking under threat.
- (e07-06) Jungle emblem before Treasure (less camp damage): 5/24 (-278) vs 10/24 (-142) on the first 6 maps. Gold first matters.
- (e07-06) A Power emblem between Treasure and Jungle: 5/24 (-325). Keep Treasure > Jungle > Power.
- (e07-06) Gold weight 1.8 before round 1200 in group scores: 22/48 (+44) vs 27/48 (+87) with weight 1.0 over 12 maps.
- (e07-06) Team-target pull 1.0 + (pack-1) x 0.4 on solo-safe groups on packing maps, together with a capture hand-off
  without the 25-round enemy guard: vs e05-05 01broken-curve 0/2 (-965/-787), 00corners 0/2; keep the full pack on
  small/starved maps and hand off only when no enemy was seen recently (single 8-game test, effects not separated).

## Known weaknesses
- Mirror-ish matches vs e05-05/e06-05 are decided by side (team B snowballs on 01elk-arena, 00rushbait-v2, 01broken-curve);
  combat, not camps, decides them, and this generation did not change combat.
- 00lanes 0/4 and 01broken-curve 1/4 vs e05-05/e06-05; 00split weak.
- Loses 4/6 to the parent e06-04 on 00ambush/01big-world/00pilgrimage although it beats it on aggregate vs the panel.
- need uses this champion's dps only (not the squad's spells); golem groups are probably over-estimated.
