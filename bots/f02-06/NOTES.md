## Strategy (≤10 lines)
Big-game hunter (immigrant, starter infrastructure only). Draft sturdy melee with stuns and resets
(Vendetta, Ryth, Marcella, Varg) plus Vesper Vale for gold. Early on every champion farms cheap groups alone,
scoring value / (travel + respawn wait + clear time + extra trips), with per-champion target claims in shared
memory so they spread out. Shards: jungle → monster → power(hp) → power(main) → durability(hp).
Once the team has shards, any champion may declare a boss raid (golem: three fixed members by handle order;
monolith: everyone, from round 350 with 8+ shards and 4+ champions). Members bank gems first, stage just
outside boss range, stamp "staged" in shared memory, and engage together once enough are staged or the boss
is awake. Hurt champions leave camp range and recall, so the tank rotates; camps never regenerate, so the
raid wears the boss down over several trips. Camps plugging the only route are cleared. After an enemy
sighting (shared memory) the team packs up on the lowest-index champion's target; fleeing melee hit back.

## Key parameters
- Score: (gems + w·gold)/(dist + wait + clear + (trips−1)(2·home+12) + 16); w 0.5 before round 700, 0.3 after;
  ×1.3 current target (not while waiting > 12 rounds for a respawn), ×0.6 for such long waits, ×1.4 reward
  waiting, ×0.15 claimed by an ally (×0.7 if > 3000 hp, off in pack mode), ×0.75 enemy side.
- Trips = ceil(incoming·clear / (0.7·max hp)), give up above 6; bosses never solo.
- Raid feasible: incoming·clear ≤ members·max hp·(1.4 golem / 1.6 monolith), clear ≤ 40 / 70 turns;
  not before round 120, golem needs 3+ shards; abandoned after 160 rounds without damage, 450 rounds max.
- Retreat when hp < max(35%, 3·last hp drop, 4·incoming camp damage), unless the target group dies at least
  1.5 turns before we would; recall once out of camp range. Recall to heal (< 70%) during respawn waits.
- Pack mode for 48 rounds after any enemy sighting; raid staging lowers the head count after 15 rounds.
- Bank at ≥ 22 gems, ≥ 10 within 14 tiles, ≥ 8 during a raid, ≥ 4 within 3 tiles, or after round 1880.
- Enemies: fight only with power ratio ≥ 1.5 + gems/100, hp > 50% and not outnumbered; else flee (≤ 7 tiles).
- Shopping trip at ≥ 300 gold when not under fire; wait on spawn up to 8 turns to heal to 90%.

## Changes this generation
- New bot. Final version: vs f00-03 13/40 on 20 maps (margin −299; the first full version was 4/40, −548);
  vs starter 36/40 on the other 20 maps, 0 faults, 0 bot errors. Previous version vs f01-07: 11/80 on all
  40 maps (margin −507; 6–8/80 before the finishing and respawn-wait fixes).

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter's parent notes: none (immigrant).
- Loading camp groups costs ~3–10k fuel each (vector allocations); doing it in one turn blew 200k on 34 groups.
  Init is now spread over turns with a fuel check; vectors are reserved.
- `spawn_region().positions()` inside the per-group score loop and `grow()` of boss rings every turn each
  burned ~50k fuel; turns then resumed next round and every action was skipped (champions stood still).
- Allied champions out of vision cannot be located: raid gathering by ally positions never completed.
  Shared-memory "staged" stamps fixed it.
- Waiting for a respawn on a group's own spawn tiles blocks the respawn forever.
- The altar field must not treat camp spawn tiles as walls: champions capturing on them never banked.
- Distance-based golem raid membership disagreed between champions (stuck at 2 of 3); fixed members work.
- Bolder enemy fights (ratio 0.9, hp 35%) doubled deaths and lost every game vs f01-07; 1.5/50% is better.
- Angular sectors per champion (f00-03 style, ×0.45 outside own sector): clearly worse here (−756 vs −579).
- Drafting the opponents' line-up (Vesper, Vendetta, Marcella, Tavi with remote shopping, Varg): worse.
- Max 3 trips per group, gold weight 0.8/0.4 or 0.3/0.2, softer claim factor 0.4: within noise or worse.
- Recalling to heal/shop whenever a respawn wait outlasts the round trip: 2/20 vs 6/20 on ten maps.
- Some camp tiles fail every attack (engine error 42/98 although visible); skip such a tile for 20 rounds.

## Known weaknesses
- Still collects about half the gems and gold of f00-03/f01-07: ~20% of turns wait for respawns, ~35%
  travel. Deaths ≈ 6 per game vs f01-07.
- 01boss-battle and 01yellow-brick-road: almost no gems (boss guards the altar / huge groups); 01islands
  needs dashes or crystals, which are not used. No wards, altar locks or teleport crystals.
- Raids still pull champions off farming while waiting at the staging ring.
