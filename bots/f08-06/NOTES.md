## Strategy (≤10 lines)
Map reader built from the starter's infrastructure. The draft (10x fuel) walks the map once: walking distance from
both spawns to every camp group, boss/monster/contested flags, map size and where the gem income lies. It picks a
plan (farm / boss / aggro), a bank threshold and one angular sector of our-side groups per champion (equal value),
and writes all of it to draft memory (bytes 0-31 header and slot table, 4 bytes per group from byte 32).
Champions score groups by value / (eta + clear + respawn wait + extra trips), where camps never heal and a group too
strong for one trip costs walks home; own sector ×1, other sectors ×0.45, shared ×0.7, enemy-side groups ×0.3.
Shared memory: bytes 0-4 hold each slot's target (crowded groups ×0.6), bytes 5-7 the latest enemy sighting
(groups within 10 of it avoided for 40 rounds). Fights: engage when hp×dps beats theirs by the plan's bias (1.5,
aggro 1.35), else flee early (enemy range + 3), melee hits on the way out, ranged shoots melee-only threats at 3+.

## Key parameters
- Draft plan: boss if near bosses hold ≥ 35% of gem income; aggro if area ≤ 1600 or spawn gap ≤ 24 and clusters
  < 50% of income; else farm. Bank threshold 40 (aggro 30), or ≥ 15 gems within 10 tiles, ≥ 5 within 4, endgame.
- Picks (all strength champions): farm Vesper Vale, Varg, Vendetta, Marcella, Tavi; boss Marcella first;
  aggro Vendetta first. Shop: 5 monster str, 5 jungle hp, 5 power str, 5 durability hp, then power str/hp.
- Target: gold weight 0.5 before round 600 then 0.2; kMaxTrips 6; trip cost 12 + 2·distance from spawn; kStick 1.5;
  retarget every 3 rounds (slot-staggered) or when the target is respawning.
- Hurt: recall below 30% hp (step out of camp reach first); shop recall at 210 gold (banks first); tribute under
  fire when hp > incoming × (channel + 1) × 1.3; altars near enemies avoided for 60 rounds when there are others.
- Movement: cached host fields (target, altar, spawn); when units block, a detour field is used for 12 rounds.

## Changes this generation
- New bot (immigrant, map-reader archetype). Start: 0/16 vs f07-02 and 2/8 vs f00-03 after the first working
  version. Final: vs f07-02 11/40 on 20 maps, vs starter 8/8, vs f00-03/f01-07/f06-05 4/8 each, vs f05-02/f04-05
  3/8, vs f02-01 2/8 (4-map panel check); vs f03-05 18/80 on all 40 maps mid-way (before the last fight changes).
- Measured gains (vs f03-05, sum of per-map average margins): flee with melee hits / ranged kiting −4148 → −1942
  (9 maps); fight bias 1.15 → 1.35: 2/8 → 6/8 vs f00-03 on fight maps; crowding 0.6: −1942 → −1638; shared enemy
  sighting + flee pad 3: −5790 → −4604 on 8 fight maps; bias 1.5: −4604 → −3755.
- Bugs fixed on the way: other groups' camps block paths (fields route around them), a field to an empty spawn tile
  of a half-dead group left champions idle 300 rounds, eta mixing two measures flip-flopped targets, static and
  detour fields alternating oscillated at altars, recall/tribute while stunned threw (engine 44/45).
- 0 engine faults and 0 `bot error:` lines in all scanned runs (final 40 games included). ~690 games used.

## Tried and failed (keep entries from the parent's notes; add yours)
- (f08-06) Fight bias 1.45/1.6 before the flee fixes: 3/8 vs 6/8 at 1.2/1.35. Bias 1.05/1.2 after: −2834 vs −1942.
- (f08-06) Melee dps ×0.6 in the fight estimate when the enemy is not adjacent: −4448 vs −3755 on fight maps.
- (f08-06) Flee pad 4 instead of 3: clearly worse on fight maps (−5587).
- (f08-06) Jungle emblem first (strength, then monster strength): farm maps −2484 vs −2596, fight maps −5196 vs
  −3755; jungle health first: fight maps −5659. Kept monster strength → jungle health.
- (f08-06) Assume a raid party of 3 for groups ≥ 2500 hp (so someone starts big groups): −4545 vs −3584 on 6 boss
  and big-group maps; only 01concentrated-wealth improved.
- (f08-06) Sector weights 0.7/0.85 (−3305) and 0.3/0.6 (−2882) vs 0.45/0.7 (−2484) on 8 farm maps.
- (f08-06) Shop recall at 300 gold or 120 gold when below 60% hp: −2629 vs −2484.
- (f08-06) Fact: map stdout of the C++ bots is padded with NUL bytes; strip them before grepping logs.

## Known weaknesses
- Islands-style maps (01islands): most groups are not reachable on foot; no dash or teleport-crystal routing, so
  the team farms two groups and scores ~0.
- Altars enclosed by camps (01boss-battle centre) are unusable; no monolith or golem raids worth the name.
- Still dies 2–5× more than strong opponents on small contested maps (hooks, swirl, elk-arena, contest); fights
  are a single hp×dps comparison of visible units with no memory or regrouping.
- Farming is ~20% slower than f03-05 on peaceful maps (more walking; eta is a Chebyshev/spawn-distance bound).
- No wards, altar locks, crystals or dashes; Vendetta dies most (melee chasing ranged enemies).
