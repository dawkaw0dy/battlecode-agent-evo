## Strategy (≤10 lines)
Scaling economy from f01-07 … f05-02 (parent A, unchanged outside combat): draft Vesper Vale, Vendetta, Marcella, Tavi,
Varg; home regions per champion; groups scored by value per champion-turn with a camp damage model; raids for bosses;
gold into shards; A's spells subsystem (one scored cast per turn); turn-by-turn focus-fire simulation decides fights.
This generation (crossover with f05-03, focus combat): B's combat code is f04-05's older spell code (A's notes and
per-champion stats show A's spells farm faster and fight equally well), so the port is B's combat-relevant addition
from its endgame: contest() (kill a tributing enemy before its channel ends; it cannot act and its bag is ours).
Around it: the fight model knows a tributing champion cannot act, our own tribute is cancelled when the enemies in
sight kill us before it ends, and a ranged champion without a ready reset waits one step outside a ranged enemy's
range (up to 3 turns) instead of stepping in and handing over the first shot.

## Key parameters
- kChannelBusy: a tributing enemy (or ally) gets busy = remaining_channel_time(); its delay in the simulation is at
  least that, and threat_at() / flee damage skip it.
- kContest (from f05-03): channeller with ≥ 5 gems within 9; no free enemy within 7 of us or 5 of it; our hp ≥ 30%;
  reach + ceil((hp − reset hit − damage spells) / (str · (1 + allies within 3))) ≤ channel turns left. Damage only.
- kGuardTribute: cancel our tribute when some enemy hits us within one turn (delay ≤ 1, dashes count 2 steps) and the
  enemies' damage over the remaining channel turns ≥ our hp + shield; no new tribute for 4 rounds after.
- kWaitIn = 3: ranged, no ready attack reset, nobody in our range, every enemy within range + 1 is ranged with range ≥
  ours (not stunned, not channelling): stand still, at most 3 consecutive turns.
- Unchanged from A: kEngageBias −150, kStick 0, kDoomedFights off, kChaseTurns 5.

## Changes this generation
- All runs tools/benchmark.py, release, 40 maps × both sides. 850 games in total. 0 engine faults; 0 `bot error:` lines from
  our side (the one caught exception vs starter was the starter's); one caught `warn: submit: engine error 48` in
  400 v3 games (A's tribute() code, unchanged).
- Baseline A (f05-02) vs B (f05-03): 46/80 (+50). A lost both sides on 00collision, 00contest, 00elkring, 00fragment,
  00road-of-riches, 00rows, 01starfish.
- v1 (contest + busy + tribute guard): vs B 39/80 (+2), vs A 37/80 (−58). Diagnostics (20 games): busy enemies in 518
  fight evaluations, contest 29 times, tribute cancels 5 — rare; most of the swing is chaos.
- v2 (v1 + flee model, see below): vs B 31/80, vs A 22/80. Reverted (kFleeModel false).
- v3 = v1 + kWaitIn (submitted): vs B 50/80 (+142; 6/14 on A's 7 lost maps, 19/26 on A's 13 won-both maps), vs A
  38/80 (−43), vs f04-05 45/80 (+56; A's notes: 38/80, −93), vs f03-05 47/80 (+94), vs starter 79/79 valid (+994).
- Death traces (20 games on B's strong maps, 816 deaths): 318 engaged, 295 retreating, 77 hurt/walking home, 58
  stunned, 18 while channelling a tribute (226 gems), 9 on the banking walk. Most gem losses are small bags (~5 gems
  per death); about 120 gems per game per team change hands through kills.

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
- Decision-identical fuel refinements (f04-05, reverted): only shift fuel thresholds and opening timing, which
  reshuffles chaotic games: vs f03-05 45/80 (+135) against 59/80 (+300) before.
- 00gemfight is lost vs the parent in the mirror by ~1000 both sides with the rewrite: the first contested fight
  (round 35–95) decides it and our champions now start 2–3 rounds earlier; not a fuel bug.
- Host reads are not slower than reads in place for a single tile (~30 fuel each; a bounds-checked raw read costs
  as much): raw reads pay off only where they replace exports, copies, allocations or exceptions.
- Holding stuns and nukes off camps while an enemy champion is within 7 (kHoldRange 7): 44/80 vs f03-05 against
  49/80 without. The spell spent on camps is worth more than the burst kept for a fight that may not come.
- A ranged champion's walk → attack reset → attack while farming: same damage as the reset after a standing hit,
  but Vesper Vale's kills fell 824 → ~680 per 80 games vs f03-05 in every run with it. Reverted.
- Every version of the f05-02 spell rewrite sat at 36–41/70 on the same 35 maps vs f03-05 where f04-05 scored 50/70;
  Marcella's camp damage was ~10% lower (cause not found).
- (f05-03) Choosing the bank altar by per-altar cost, "no progress → give the altar up", every-turn endgame init on the
  first turns: all slower or more fuel-exhausted turns (see f05-03's notes).
- (f05-03) Locks are not a win condition: f00-03/f01-07/f02-01 break them with attack(altar_lock_at(a)), f03-05/f04-05
  by tributing at the altar.
- (f06-03) Porting B's fight spells (f04-05's older fight_spells / reset code) would rebuild f04-05: A vs B per-champion
  stats over 80 games show equal kills, deaths and champion damage, and A ahead on monster damage on every champion.
  Not ported.
- (f06-03) Flee model (kFleeModel): estimate the damage of fleeing all the way home (melee chasers hit every other
  round over min(home distance, 12) turns, dashers 2 steps more, ranged the shot in hand plus one walk-reset-shot);
  fight back when that kills us and the fight scores better: deaths 25 → 30 per game, vs B 31/80, vs A 22/80. Fleeing
  survives far more often than any "doomed" estimate says; do not turn retreats into fights.
- (f06-03) First tribute guard without the "someone hits us within one turn" condition: a long channel (100+ gems)
  with an enemy 9 tiles away counted as doomed, and cancel → restart → cancel could repeat. Fixed with that
  condition plus a 4-round tribute block after a cancel.

## Known weaknesses
- 00collision, 01boss-battle, 01islands: A's endgame (unchanged) has the bugs f05-03 fixed in its endgame.cpp (an
  altar in a sealed pocket counted as open; the only altar's tribute tile is a boss spawn tile; a champion stranded on
  an island never banks). Porting B's endgame is the obvious next crossover; it was outside this generation's focus.
- Snowball maps (00swirl, 01hooks, 01starfish, 00gemfight) are still decided by the first fights; ~25 deaths per team
  per game, 40+ on combat maps. Fights are per champion, no shared engage call; deaths cluster at fight values near the
  engage threshold (−150..+50), i.e. coin-flip trades.
- Retreat deaths: a third of all deaths happen while retreating, often after 5–19 turns of fleeing.
- One caught exception (engine error 48 "Altar is outside gem submission range") in 1 of 400 games: tribute() checks
  range against a stale pos_. Harmless (caught, prints warn:, not bot error:), inherited from A; fixing it shifts fuel
  and reshuffles every game, so the measured build was kept.
- Ally positions outside our own vision are unknown (vision is not shared; position() throws).
- No wards or teleport crystals.
- Spell choice is greedy per turn; no team-wide coordination of stuns.
