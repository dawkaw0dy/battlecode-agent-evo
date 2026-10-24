## Strategy (≤10 lines)
Scaling economy (from f01-07) with a rewritten camp subsystem. Draft Vesper Vale, Vendetta, Marcella, Tavi, Varg;
the draft splits our side's groups into home regions. Each champion scores groups by value per champion-turn:
(gems + w·gold) / (travel + respawn wait + clear + trips home to heal + capture + walk to an altar for the gems).
Clears use a damage model: camps die in the order that hurts least (threat per health, clusters last) and a camp
only hurts while it reaches the side of the target we hit from; approach_goal() puts us on the in-range tiles the
fewest fighters reach. Groups one champion can wear down are farmed alone (heavy ones an ally works on are shared);
bosses and packs too strong for that are raided: the best raid size is planned, raiders gather just outside the
fighters' reach and engage together (or as soon as the group is awake), join a forming raid rather than start a
second one, and the claimers nearest a reward capture it (boss rewards split up to the region's capacity).

## Key parameters
- Value gems + w·gold, w = 0.35 (Vesper 0.5) × max(0.4, 1 − round/1500); factors as parent (home 1.0/0.8/0.6,
  enemy side ×0.7, reward ×1.3, current target ×1.25, sighting ×0.5).
- Capture 6 turns; trip home 8 + walk from spawn (≤60); bank cost = share of gems / 45 × min(walk to altar,
  6 + spawn→altar walk). Trips are expected values: (damage share − spare hp) / (max hp − margin).
- Solo: needs ≥5 turns of tanking per trip, ≤4 trips; small claimed groups ×0.3 per claimer, long ones shared.
- Raid (bosses, or solo infeasible): n = max(2, claimers+1)…alive, ≥3 tank turns per trip across raiders, ≤3 trips,
  +10 turns gather when it needs someone not yet coming, ×(1 + 0.5·claimers) to join; rally ring = 1–2 tiles
  outside reach, wait ≤35 rounds, then engage if ≥2 there else drop the boss for 80 rounds.
- Approach: safest-spot goal on a local box when work ≤2500 cells and fuel allows; plain ring otherwise and for
  12 rounds after the safe spots proved unreachable. Far (>6 beyond range) walk down the cached group field.
- Bank by recall when the walk to an altar is >10 longer than spawn→altar.

## Changes this generation
- Rewrote the camp subsystem behind the same interface (choose, farm, approach_goal, estimate_damage, hit/spells):
  damage/threat model, raid planning with gathering, join bonus, capture by nearest claimers, safe approach spots,
  bank-distance cost, cached-field walking (fewer host BFS), local rally ring (bitboard dilations by a boss's
  reach cost ~50k fuel and exhausted turns).
- Fixed the parent's solo boss raids (it assumed 2+ raiders: Marcella spent ~300 rounds alone on a golem).
- Results (benchmark.py, 40 maps × 2 sides): vs f01-07 53/80 (+148 mean), vs f00-03 49/80 (parent 42/80),
  vs starter 24/24 sampled (earlier version 80/80, 1114 mean rounds vs parent 1161). 0 bot errors in 160 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: debug prints (fuel) alone moved 00lost-world B from 1069 to 524 rounds. Judge changes on
  full 80-game runs, never on single maps; rounds-to-win vs starter is a less noisy farming-speed metric.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.

## Known weaknesses
- Elk-pack-only maps (00stampede, 01starfish, 01elk-arena, 00pilgrimage) and 01hooks still lose to the parent
  often: the retreat rule (hp < incoming·(exit+2)+40) makes a pack's tank flee after a hit or two.
- Lone monolith regions hold one capturer: 200 gems on one champion, a 20-turn tribute.
- No wards, altar locks or teleport crystals; fights are per champion (not changed this generation).
