## Strategy (≤10 lines)
Big-game hunter (immigrant; only the starter's main loop and draft skeleton kept). Draft Vesper Vale, Vendetta,
Marcella, Tavi, Varg (the order is also each champion's shared-memory slot). Each champion farms alone: it scores
groups by value / (travel + respawn wait + clear + extra trips + 12), claims its target in shared memory so allies
spread out, and wears strong groups down over several trips (camps never regenerate). Slot 0 calls whole-team
raids on golems (round ≥ 250, 10 shards) and monoliths (round ≥ 600, 15 shards) when the team can do it in 1 / 2
trips: everyone banks first, stages just outside boss range, and engages once 3 are near (or after 15 rounds);
hurt champions step out of range and recall, so the tank rotates. Fights use a Lanchester comparison of the
visible sides (Σhp × Σdamage) within 7 tiles; otherwise flee to spawn (committed for 5 turns, hold spawn and shoot
from it), and mark the contested target for 40 rounds. Pathing avoids awake camps' ranges and clears plugs.

## Key parameters
- Shared memory: [0..4] claim per slot (group+1), [5] flags (bit0 engage, bit1 raid), [6] raid group+1,
  [7] bit per slot "near the raid target". (A pack/squad mode, kSolo=false, is kept but measured worse.)
- Solo feasibility: group damage 0.6·str per turn, budget 0.6·max hp per trip, ≤ 6 trips and ≥ 6 turns per trip;
  extra trip costs 10 + 2·walking distance from spawn. Gold weight 0.4 (0.25 after round 900).
- Score factors: ×1.3 reward waiting, ×0.7 enemy half, ×0.3 within 7 of our last enemy sighting (30 rounds),
  stick with the current target unless another scores 2× (kSwitch); fallbacks ×0.01: respawn wait > 40, ally claim.
- Retreat when hp < max(35%, 4·incoming camp damage + 10%) or hurt below 35% by anything; walk home when an unseen
  attacker keeps cancelling the recall. Heal on spawn to 90%. Shop trip at ≥ 300 gold away from the target.
- Tribute: ≥ 40 gems, ≥ 15 within 10 tiles, any within 4 tiles, ≥ 10 within 30 rounds of an enemy sighting,
  ≥ 5 below half health, before a raid, after round 1850; once started it continues until the bag is empty.
- Fight ratio 1.3 (×1.2 when alone); finish enemies below 2.5× our strength. Stall: 6 turns without moving on an
  approach marks the target bad for 80 rounds; no progress towards it for 25 rounds marks it bad for 150.
- Shards: melee jungle(health) ×5 → monster(str) ×5 → power str/health alternating; ranged monster(str) ×5 →
  power(str) ×5 → alternating.

## Changes this generation
- New bot (immigrant). Final measurements (benchmark.py, release, both sides): vs f00-03 on all 40 maps 17/80
  (mean −457; first full version 16/80, −462); vs starter 12/12 on six maps (rounds to win 959–2000).
  Sanity runs on two maps each vs f04-05, f03-05, f02-01, f01-07: 0/4 each (−584 … −929). 0 faults, 0 caught
  exceptions in every run after the move guards. The last change (idle fallbacks) was checked on one game only.

## Tried and failed (keep entries from the parent's notes; add yours)
- Parent (starter) notes: none (immigrant).
- One pack of 5 following a leader's target (first design): champions arrived piecemeal, leader targets flipped
  every few turns, 0–1/20 vs f04-05. Two squads: no better. Pairs (odd slot follows the even slot): 3/20 vs 6/20 solo.
- Boss raid at round 25 with base stats: everyone died to the golem. Gate by round and shards.
- Whole-team raids on any rich group (≥ 30 gems) too strong for one champion: 15/80 vs 17/80 without; the team
  spent most turns staging (engage needed n−1 near or 30 rounds). Kept for bosses only, engage at 3 near / 15 rounds.
- Fight ratio 1.6: 11/80; 1.1: 4/40 vs 8/40 at 1.3 on the same maps.
- Jungle(str) → power(hp) → monster(str) for everyone (f00-03 style): 13/80 vs 17/80.
- recall() right after moving/attacking throws engine error 44: recall only on an untouched turn.
- Chebyshev distance thresholds flip-flop (bank at ≤ 8 from an altar, walk away, come back): use hysteresis.
- Debug prints inside debug_block are type-checked in release builds: a stale variable name breaks the build.
- move() into a tile the plug path found can throw error 22 (known occupied): check the blocked board first.

## Known weaknesses
- Loses most games to every evolved panel bot: early contested fights snowball (enemy 14 power emblems by the end
  on 01hooks); our fight model is a static comparison, no focus-fire planning or kiting.
- Economy ~½ of f00-03 on open maps (expanse, big-world): long walks back from spawn after retreats on far groups.
- Contested central groups (01elk-arena, 00stampede) are given up instead of fought for as a team.
- Islands/dash maps: no dashes, crystals or wards; unreachable groups are simply ignored.
