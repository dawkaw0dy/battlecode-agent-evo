## Strategy (≤10 lines)
f04-05 (parent A: scaling economy, fuel subsystem, fight simulation, altar locks) with its opening and camp choice
crossed in from f04-03 (parent B). Draft is unchanged (Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions).
New in the draft turn: a joint opening plan. Every slot gets its first group and the team it clears it with
(greedy over groups × team size 1–3, the strongest free slots first, a clear simulated from full health, value per
champion-turn with B's territory discount), written to draft memory; champions follow it until that group is cleared,
blocked or round 150. Camp choice after that is B's: kill-by-kill clear simulation from the health we have now,
the smallest raid team that succeeds, territory discount, an engage gate before waking a group, the last camp
finished from a spawn tile, Varg's splash in the hit order. A group standing on every altar spot (01boss-battle's
double monolith) is known to all champions from the start and raided with up to 20 extra trips.

## Key parameters
- Draft memory: 0 'E', 1 team size, 2 groups, 4.. home slot per group; 256 'O', 257–261 first group per slot (255 none),
  262–266 team size per slot.
- Opening plan stats per pick (hp, camp dps, range): Vesper 1000/62/3, Vendetta 1000/98/1, Marcella 1200/66/1,
  Tavi 900/70/3, Varg 1250/50/1; value = gems + 0.35·gold; rate = value / n · territory / (walk + clear + 6 + bank / n).
- kOpeningRounds 150. B's camp constants: kRallyRounds 30, kGatherTurns 8, kMaxTrips 3, reserve =
  max(0.2·max hp, hit·(exit + 2) + 40), hand-off max(0.2·max hp, 2·hit + 40). kGuardTrips 20.
- Everything else as in f04-05 (fuel, locks, fights).

## Changes this generation
benchmark.py, 40 maps × both sides (80 games per line), 0 faults and 0 caught exceptions of ours in every run.
| version | vs f04-03 (B) | vs f04-05 (A) |
|---|---|---|
| start (= A) | 47/80, +137 | — |
| v1: B's camp choice ported | 55/80, +188 | 47/80, +24 |
| v2: + draft opening plan | 59/80, +264 | 46/80, +104 |
| v3: + altar-guard raid (submitted) | 59/80, +263 | 46/80, +126 |
- Opening income (mean per game, us/them): gems collected by round 200 vs B 51/53 (v1) → 62/47 (v3); vs A 53/51 → 58/47.
- The trace that motivated the plan (00swirl, round 3): two slots each planned half a raid on different elk pairs and
  waited 30 rounds on the rally ring; two others shared a 0-gem gold cluster.
- 01boss-battle (the only altar under a double monolith; nobody banks until someone kills it, ~round 1050): the ported
  engage gate blocked the altar plug as "too strong"; the guard is now exempt and raidable (0/2 → 1/2 vs A).
- Panel, 10-map dev set × 2 sides (00ambush 00collision 00contest 00gemfight 00lanes 00swirl 01boss-battle 01hooks
  01starfish 01yellow-brick-road): starter 19/19 (one game void: the starter's own `bot error`), f00-03 17/20,
  f01-07 17/20, f02-01 14/20, f03-05 13/20.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
  (f04-05: read the field in place instead; no export needed.)
- Enemy memory (3 rounds) + fight/flee hysteresis: 23–24/80 vs 34/80 without. Vision flicker is not the problem.
- Unseen ("unknown") camp groups blocking the route reset the plug logic every turn: champions stood still.
- Ranged champions stalled on range-3 tiles without line of sight: shrink the approach radius when nothing is hit.
- Boss raids gated at 2.5 tank-turns: everyone walked into monoliths and never hit them; 8 turns works.
- Raid and current-target bonuses on a respawning boss kept all five champions waiting 150 rounds.
- Rotation-aware retreat (exit after 1 step when an ally is as close to the camp): 43/80 vs 50/80 vs parent.
- Games are chaotic: debug prints (fuel) alone moved 00lost-world B from 1069 to 524 rounds. Judge changes on
  full 80-game runs, never on single maps; rounds-to-win vs starter is a less noisy farming-speed metric.
- iostream float printing costs 10–30k fuel per line: traces distort fuel profiles.
- Hysteresis again, on the simulated fight value (±60 in favour of the last decision): 14/32 vs 17/32 without.
- Engage bias −300 (fight almost anything): 16/32 vs 23/32 at −150. Bias 0 or +60 (cautious): 16–17/32.
- Trading blows when the estimated damage of fleeing exceeds our health: most deaths in fights came from it.
- Retreat scoring without camp damage: champions stood under elk fire choosing between two equally "safe" tiles.
- Allies counted only within 7 of us: one step changed the count, so a champion flip-flopped engage/retreat
  beside a camp until it died. Count allies near the enemies too.
- A local C++ BFS instead of the host's: >500k fuel on 100×100 (the host's costs ~30k). Never BFS locally.
- memcpy of engine buffers (~1 fuel per byte) and distance_field export (~2.3 per tile): read in place instead.
- Re-choosing the target every 2 turns instead of 4 and checking reward groups every turn (affordable now):
  rounds-to-win vs starter 1072 vs 1066–1084, no measurable gain; not kept.
- Decision-identical fuel refinements, reverted (commits 910cc45, 40ebae0): clear_cost() as 64-bit threat masks,
  camp tiles' raw words precomputed, initialisation 300k → 225k on 80-camp maps. They only shift fuel thresholds
  and opening timing, which reshuffles chaotic games: vs f03-05 45/80 (+135) against 59/80 (+300) before.
- 00gemfight is lost vs the parent in the mirror by ~1000 both sides with the rewrite: the first contested fight
  (round 35–95) decides it and our champions now start 2–3 rounds earlier; not a fuel bug.
- Host reads are not slower than reads in place for a single tile (~30 fuel each; a bounds-checked raw read costs
  as much): raw reads pay off only where they replace exports, copies, allocations or exceptions.
- (from f04-03) Lock detours of 14 tiles + idle champions running locks to far altars (≤40): 57/80 vs 65/80 at 8.
- (from f04-03) Pair farming (melee + ranged share the leader's target): fewer gems, no extra kills; not kept.
- (from f04-03) Charging every raider the full walk-out reserve: no team could ever raid a double monolith.
- f05-01: group analysis cached in draft memory (camps as a nibble offset from the group's first tile + 4-bit kind;
  bit-identical to analyse_map on every map tried) saves ~75k fuel per champion, first full turn round 4 → 3.
  Decision-identical otherwise, but vs A 42/80 (+75) and vs B 57/80 (+250) against 46/80 (+126) and 59/80 (+263):
  a timing shift again, not kept. Note: the plain 3-byte-per-camp layout does not fit 01boss-battle (~160 camps;
  mapgen.mjs info lists only one symmetric half of the camps). Most of that ~120k is vector growth, not host calls.
- f05-01: the altar guard learned per champion (when tribute() finds every spot under a camp) reached only the
  champion that tried to bank; champions do not share memory, so it is now computed statically at initialise.
- f05-01: the guard with 8 extra trips was still never raided (double monolith, 40,000 hp, 400 hits per turn).

## Known weaknesses
- PvP still decides most losses (00swirl, 01hooks, 01starfish, 00gemfight, 01elk-arena: 40–70 deaths a side).
- The opening plan is static (draft-time): it ignores the enemy's route and drops after round 150 or the first clear.
- 00contest (gems only from a monolith) and 01boss-battle remain timing races on the boss; the guard raid wins 01boss-battle
  only some of the time.
- Champions act from round 3–4 on big maps (initialisation ~300k fuel).
