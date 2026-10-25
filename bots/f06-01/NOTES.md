## Strategy (≤10 lines)
Scaling economy from f01-07 … f05-02 (unchanged outside the endgame): draft Vesper Vale, Vendetta, Marcella, Tavi,
Varg; home regions per champion; groups scored by value per champion-turn with a camp damage model; raids for
bosses; gold into shards; turn-by-turn focus-fire simulation decides fights; f05-02's one-cast-per-turn spell scoring;
altar locks carried by everyone, the last pick runs locks to far altars until round 1600.
This generation (endgame): camps that cannot be cleared, captured and banked before round 2000 are down-weighted;
once either team has 700 tributed, smaller bags are banked; a bag that would win the race makes its carrier worth the
game (fight value, stun on its tribute, lock runs from farther and at any health); no shopping trips once the game is
nearly over; altars walled off from our spawn are ignored; distance fields fill on a fixed budget (less chaos).

## Key parameters
- Finish-by-limit (choose(), from round 1500 only): a group is down-weighted ×0.1 when travel + respawn wait + capture
  (6) + walk to the altar (min(bank_d, 6 + spawn→altar)) + 4 spare ≥ rounds left, or that plus half the solo clear
  time plus ceil(gems/10) (tribute) does. Trips home are not counted (allies help; counting them gave up camps
  allies cleared in 20 rounds).
- Race banking (tribute()): when max(our, their tributed) ≥ 700, bank at ≥ 25 gems or ≥ 10 gems within 12 steps
  (parent: 45, or 20 within 10, or 10 with an enemy seen lately — all kept).
- Winning bag: tributed + carried ≥ 1000 → fight value +1500 (ours and theirs), stun on its tribute +2000;
  deny() locks such a tribute from up to 14 tiles (others 9) and ignores the 35 % health floor.
- Shop cutoff: no 240-gold shopping recall and no idle 60-gold recall from round 1850 or with ≥ 900 tributed.
- fill_fields(): ≤ 30k estimated fuel per call (3 per tile and field, at least one field), floor fuel(per + reserve/4).

## Changes this generation
- All tests: tools/bench2.py (release, both sides); 858 games, 0 engine faults and 0 `bot error:` lines; 0 `warn:`
  lines in the final build's 160 games.
- Parent baseline taken free from evo-results (r05-*.json): local games reproduce it exactly (deterministic).
- Deterministic fill_fields: the parent filled fields "while fuel lasts", so every code change moved how many fields a
  turn filled and reshuffled whole games. With it, 19/24 and 21/24 games identical to the parent; 23/48 vs parent
  22/48 on the test set (24 games vs f04-05, 24 vs f03-05, close races and 2000-round games). Kept as baseline.
- E1 finish-by-limit, paired vs that baseline on games diverging after round 1500: skip variant +77/+200 gems
  (f04/f03), ×0.1 with trips +105/+232, half clear time +27/+154, travel only +25/+184; +2 wins each. Kept (half).
- E2 race banking (with E1): 27/48 vs 23/48; f03-05 gems lost to kills 2373 → 1959. Kept.
- E5 winning bag: 3 of 48 games differ, +63 gems, same wins. Kept (logically decisive, rarely fires).
- E3 shop cutoff: 12 games differ, +60 gems, same wins. Kept.
- Unreachable altars (bug found in 00collision: an enemy lock on the shared altar plus a walled-in second altar made
  go() fail, so Vesper carried 300–440 gems for 600 rounds): 00collision 4/4 vs f04-05/f03-05 (parent 2/4).
- Final (full 40 maps, both sides): vs f04-05 45/80 (+47) vs parent 41/80 (+9); vs f02-01 71/80 vs 69/80;
  vs f03-05 (before the altar fix) 49/80 (+138) vs 48/80 (+130); head-to-head vs f05-02 39/80 (every map but
  01islands splits by side: both play alike until the endgame).

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: host calls cost ~30–40 fuel, but every bitboard temporary (&, |, ~, shift) ~600 and a grow(r) ~4k·r;
  local loops ~50 per element. Function-local statics add a guard check per call. Turns above 100k fuel resume
  next round with the champion possibly dead: position() then reads (-1,-1) and field reads throw engine error 5.
- A route-tracing loop with host field reads cost ~70k fuel; use an exported local copy (unreached = 65535).
  (This generation: read the field in place instead; no export needed.)
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
- Decision-identical fuel refinements, reverted (commits 910cc45, 40ebae0): clear_cost() as 64-bit threat masks
  (a 20-camp group cost ~20k per evaluation, ~5k after), camp tiles' raw words precomputed for observe and the
  camp-block board, initialisation 300k → 225k on 80-camp maps (reserved vectors, single-pass threat masks). They
  removed the last capped turns on 01yellow-brick-road (0 in 2000 rounds), but only shift fuel thresholds and opening
  timing, which reshuffles chaotic games: vs f03-05 45/80 (+135) against 59/80 (+300) before. Games are
  deterministic, so the measured version was kept.
- 00gemfight is lost vs the parent in the mirror by ~1000 both sides with the rewrite: the first contested fight
  (round 35–95) decides it and our champions now start 2–3 rounds earlier; not a fuel bug.
- Host reads are not slower than reads in place for a single tile (~30 fuel each; a bounds-checked raw read costs
  as much): raw reads pay off only where they replace exports, copies, allocations or exceptions.
- Holding stuns and nukes off camps while an enemy champion is within 7 (kHoldRange 7): 44/80 vs f03-05 against
  49/80 without. The spell spent on camps is worth more than the burst kept for a fight that may not come.
- A ranged champion's walk → attack reset → attack while farming: same damage as the reset after a standing hit,
  but Vesper Vale's kills fell 824 → ~680 per 80 games vs f03-05 in every run with it. Reverted; kills stayed ~8/game
  without it (v4), so the drop may be chaos rather than the reset.
- Every version of this rewrite sits at 36–41/70 on the same 35 maps vs f03-05 where the parent scores 50/70; the
  parent's own notes saw decision-identical changes move it 59 → 45/80, so the parent's figure is a high draw.
  Marcella's camp damage is ~10% lower than the parent's in all four versions (cause not found: candidates are
  the kill bonus that roots her at range 4 on low camps, and the parent's "wasted" resets, which left an enhanced
  (stunning) attack buff for her next hit).
- (f06-01) Lock runner kept running until round 1950 (parent 1600): late-diverging games −161 gems vs f04-05 and
  −403 vs f03-05 on the 48-game test set, no win gained. Reverted (kRunnerLastRound = 1600).
- (f06-01) Finish-by-limit that skips infeasible camps outright: champions with nothing feasible idle at spawn and
  cede the map (01starvation A vs f04-05 −116 gems); down-weighting ×0.1 keeps them contesting.
- (f06-01) Even endgame-only code perturbs fuel: a `late` branch per group in choose() (taken only from round 1500)
  still flipped 00quadrants A (diverged at round 447) and 00monolith A (round 293). Heavy turns sit near the 100k
  limit and other fuel(N) checks; the deterministic fill removed the largest chaos source but not all.
- (f06-01) The mirror (vs f05-02) is uninformative for endgame changes: 39/80, every map but one splits by side.

## Known weaknesses
- Snowball maps (00swirl, 01hooks, 01starfish, 01yellow-brick-road, 00gemfight) are still decided by the first
  fights; 40–60 deaths in lost games. Fights are per champion, no shared engage call.
- 00contest: the only gems are the monolith's; we die ~60 times per game around it and score 0 (opponents 200–800).
- 01boss-battle: the only altar's one standing tile is a monolith tile; whoever kills the pair and stands there banks
  everything (bags of 800 gems) — no plan to take or hold that tile.
- Race ends are mostly lost on farming speed (a big capture lands just after the opponent reaches 1000); team bags
  are invisible (no shared memory left), so banking cannot be timed on the team total.
- Maps with many camps (01yellow-brick-road, 01boss-battle: 80 camps) still cap a few turns.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals; spell choice is greedy per turn.
