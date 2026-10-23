## Strategy (≤10 lines)
Spell-first pack. Draft: Vesper Vale (gold), then a stun/spell core: Ryth (area stun), Tavi (stun bolt), Marcella
(2x Focus bolt, stun-immune), Willow (heals). Every turn a spell planner scores each ready slot against every legal
target in its cast range (not only the basic-attack target, as the parent did) and casts the single best pair:
enemy champions by share of health removed x kill reward (+kill bonus, +stun bonus if unstunned and not Marcella,
x1.5 on the team focus target shared in memory); awake camps by damage plus stun value (strength); area spells on the
best centre, never touching sleeping Golems/Monoliths; heals to the most injured ally; shields when hurt under threat.
Casts happen before walking when we fight in place or hit a champion, otherwise after the step (non-exclusive only).
Camp casts wait while enemy champions are within 7, so stuns are saved for them. Stunned enemies lower the engage
bar by 0.2 each; the team camp target counts 2.5x so the pack stays together and stuns chain on one focus target.

## Key parameters
Spell scores: champion reward 200 + 3*gems, kill bonus 400 + 3*gems, stun bonus 120, focus x1.5; camp damage 0.1/HP,
camp stun 0.5*strength; heal 0.3/HP (x2 under 40% HP); min score 5. Save radius for camp casts: 7. Team target
x2.5. Focus target: shared memory bytes 2..5 (x, y, round), valid 2 rounds. Economy and engage bar as in e02-05.

## Changes this generation
New: spell planner (cast_spells) replacing the parent's cast-on-attack-target logic and heal_allies; pre-move and
post-move cast windows; shared focus target; stunned-enemy engage bonus; new draft; team target 1.5 -> 2.5.
Fixes: tiles_around sets tiles with bounds checks (draw_chebyshev near the edge raised engine error 5 on 00lanes);
play_turn wraps the turn plan in a cambc::error guard so a rejected engine call ends the turn's plan, not the game.
Results (local, deterministic, 204 games total): vs e02-01 13/16 (+346), vs e00-01 16/17 valid (+354; the 3 invalid
games were opponent errors), vs e02-05 (parent) 17/20 (+367), vs e01-01 17/20 (+272), vs starter 18/20 (+572).
Ablations vs e02-01 on 8 maps: draft Ryth-first 11/15; team target 1.5: 10/16; 4.0: 7/14; camp casts with enemies
near: 12/16.

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

## Known weaknesses
- 01broken-curve side B loses to e00-01 (-865), e01-01 and e02-05; 00quadrants B loses to e01-01 (-633).
- 00road-of-riches A still loses narrowly to e02-01 / e01-01 (we win fights but farm less monster damage).
- 01gold-rush is a narrow loss to the starter (-22/-11). 00contest ends 0-0 (tiebreak).
- Planner ignores enemy cooldowns and items; no dashes, wards or altar locks used.
