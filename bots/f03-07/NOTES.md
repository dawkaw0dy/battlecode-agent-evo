## Strategy (≤10 lines)
Safe banker (immigrant, built on the starter's main loop only). Draft Marcella, Vendetta, Vesper Vale, Varg, Ryth.
Every ally computes the same split of our side's camp groups into angular, equal-value sectors (one per champion).
Each champion scores groups by (gems + w·gold)·factor / (walk + clear time + extra trips + respawn wait), clears
them by multi-trip attrition (camps never regenerate) and stands on a spawn tile to capture, hitting weak
neighbouring camps meanwhile. Gems are banked at the nearest unlocked altar as soon as one is close; trips that stop
making progress are abandoned for 60 rounds. Champions heal (walk home or recall) below 65% health between
targets, retreat out of camp range before the next hits could kill them, avoid awake bosses, and flee home from
enemies unless the square-law ratio (Σhp·Σdps, visible allies within 6) clearly favours us.

## Key parameters
- Score factor: own sector 1.0, another champion's sector 0.6, enemy side 0.65, current target 2.0 if engaged
  (damaged camps, reward waiting, ≤3 tiles) else 1.3 (none while respawning), reward ×1.3, ally claim (lower index,
  clear < 15) 0.3, recent enemy sighting within 8 tiles 0.5. Gold weight max(0.12, 0.35·(1 − round/1500)).
- Skip a group when its camp damage would down us in < 6 turns (monoliths, golems, elk pairs); wait ≤ 40 rounds
  for a respawn, wait cost ×2.5; wait off the spawn tiles (an occupied spawn tile delays the respawn forever).
- Tribute: ≥40 gems, or altar ≤ 4 + gems/4 steps, or endgame, or ≥8 gems after a recent enemy sighting.
- Retreat from camps when hp < incoming·(exit steps + 2) + 80 + 3·gems; recall below 25%, at ≥150 gold for a shop
  trip, or below 65% when not engaged; walk instead when spawn ≤ 5 steps away.
- Enemies within 7: fight if ratio ≥ 1.6 + gems/60 and hp > 40%; keep farming (answering hits) if ratio ≥ 1.1,
  hp > 50% and they are not about to reach us with ≥20 gems; else flee (kill shot first, tribute if adjacent).
- Build: jungle(str) → monster(str) → durability(hp) → power(hp) → power(str), then alternating power shards.
- Shared memory: bytes 0–4 target claims, 5–7 latest enemy sighting (x, y, round).

## Changes this generation
- New bot (immigrant, safe-banker archetype) on the starter's main loop. Final local results: 8/20 vs f02-01
  (benchmark.py, 10 maps), 9/20 vs f01-07 (10 maps), 10/20 vs f00-03 (10 maps, one build earlier), 20/20 vs
  starter (10 maps); 0 faults and 0 caught exceptions in every run.
- Biggest wins found while building: capture-ready groups scored negative (trips = 0) so champions walked away from
  every capture (vs starter 1/6 → 6/6); per-champion sectors (16-game margin vs f00-03 −6379 → −3883); heal
  below 65% between targets; abandon blocked tribute trips; side attacks while capturing.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- (f03-07) Turns overran 100k fuel (per-tile host reads in loops) and the stale-round guard then skipped every
  move: champions looked frozen. Export fields/visibility to local buffers once per turn.
- (f03-07) Standing on a camp's spawn tile while waiting for its respawn delays the respawn indefinitely.
- (f03-07) Shared-memory claims where every ally defers to every other made champions abandon half-killed camps;
  only lower indices win a claim, and never for an engaged target.
- (f03-07) Team boss raids (gather on a ring, shared-memory ready flags, retreat on measured damage): worse on all
  boss maps (our gems 2517 → 1396–1997); champions rarely converge and the enemy contests the boss.
- (f03-07) Fight/flee changes were all worse vs f02-01: fight need 2.2 or 1.3, keep farming while enemies are out of
  reach, stricter even-fight rule, enemy radius 8. Fleeing is what keeps the bank safe.
- (f03-07) Neutral or worse: tribute at ≥70 gems, durability before monster, gold weight 0.5, enemy-side factor
  0.85 or 0.45, sector factor 0.4, sectors including centre groups, shop at 240 gold, Tavi instead of Ryth
  (better vs f00-03, worse vs f02-01), heal threshold 0.35, camp-retreat margin 120 (16-game margins vs
  f02-01 −1422 → −2567, vs f00-03 −2464 → −4551).
- (f03-07) Dash (Vendetta, Vesper) to escape when fleeing from a chaser within 4: margins vs f02-01 −1422 → −4422,
  vs f00-03 −2464 → −3232. Reverted.

## Known weaknesses
- Still ~1.5–2x slower farming than f02-01/f01-07 on open maps; no boss raids, so monolith-only gem maps
  (00contest, 01gold-rush) and boss-guarded altars (01boss-battle: the only altar has one free tile next to two
  monoliths; we collect ~600 gems and bank none) are lost.
- Lone champions still die to enemy groups on small contested maps (01hooks, 01yellow-brick-road).
- No wards, altar locks, teleport crystals or dashes; Tavi's field shopping is unused.
