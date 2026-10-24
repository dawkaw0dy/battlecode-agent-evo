## Strategy (≤10 lines)
Crossover of f01-07 (scaling economy, parent A) with f01-03's fight and spell handling (parent B).
Draft, regions, target scoring, shopping, tributes and pathing are A's: Vesper Vale, Vendetta, Marcella, Tavi, Varg
farm home regions, turn gold into shards at every spawn visit and raid bosses once they can tank them.
Fights are B's: force ratio (Σhp·Σpower, allies within 6) against enemies within 8 walking steps picks
engage / hold / flee; trade blows only when running cannot avoid them and we kill first; victims are
finishable, then gem carriers, then lowest health; enemies on their spawn are ignored and their reach avoided.
Spells on top: stuns skip the stun-immune (Marcella) and go first to an enemy channelling a tribute; a lone
chaser we cannot outrun is stunned instead of stepped away from; while fleeing out of everyone's reach we cast
damage/stun spells (kiting); Varg shields while running and as soon as camps hit him; camp spells are held
for 8 rounds after any enemy sighting so the fight starts with them ready.

## Key parameters
- Fight: engage at ratio ≥ 1.3 (or a finishable target, weakest hp ≤ 1.2·burst within 4, at ≥ 0.6); flee < 0.85
  or below 30% hp; keep fleeing 10 rounds; chase ≤ 12 steps then 30 rounds without chasing. Power = str + 0.3·focus.
- Spell reserve: no damage/stun spell on camps for 8 rounds after an enemy sighting (resets still used).
- Kite cast: when every threat is outside its next-turn reach (melee 2, ranged its range) and home > 2 steps.
- Flee stun: exactly one non-stunned threat in reach, not stun-immune, within the stun's range.
- Stun target score: −hp + 3·gems + 0.5·power, +5000 finishable, +3000 tributing, −400 already stunned.
- Farming, shopping, tribute and boss parameters unchanged from f01-07.

## Changes this generation
- Ported f01-03's handle_enemies (engage/hold/flee, trade rule, pick_victim, fight_spells with shield,
  fight_attack with reset, flee_step with dash, retreat, spawn-camper zone) in place of A's enemies().
  Port alone: 38/80 vs f01-03 (A: 38/80), no change.
- Spell timing: stun targeting (immune/tribute), flee stun, eager camp shield, 8-round camp-spell reserve.
- Kite casts while fleeing and walk-compatible shield on the run: kills/deaths vs f01-03 706/466 (was 646/608).
- Results (bench2, release, all 40 maps both sides, final version): vs f01-03 42/80 +65 (A: 38/80 −109),
  vs f00-03 47/80 +157 (A: 42/80 +62), vs f01-07 57/80 +269 (kills 1232/473), vs starter 40/40 (20 maps).
  0 faults and 0 bot errors in 876 games. 01hooks side B vs f01-03 now 1001–55 (A: 44–1012).

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
- (f01-03) Trading blows whenever fleeing cannot avoid hits: much worse; the kill-reachable condition is kept.
- (f01-03) Counting enemies seen in the last rounds (out of sight) in the force ratio: flees too much.
- Ranged kite attacks (basic attack instead of a flee step when nobody reaches us): 35/80 vs f01-03,
  kills 642/593 (kite casts only: 42/80, 706/466). Standing still to shoot exposes the ranged champions.
- No camp-spell reserve: 39/80, kills 599/600. A 16-round reserve: 34/80. Also reserving attack resets: 33/80.
- Results vs f01-03 flip on ~30 of 80 games after any behaviour change; judge changes by kills/deaths and
  champion damage taken over a full sweep, not by single maps.

## Known weaknesses
- Farming is less efficient than f01-03: ~47 monster damage taken per collected gem vs ~38, and fewer gems on
  contested maps (00contest still 0–1000 both sides vs f01-03, 00quadrants, 01broken-curve).
- Still a split farm: no grouping before a contest; fights are decided per champion.
- No wards, no altar locks of our own, no teleport crystals; no spell focus builds (all strength/health).
- Debug cast counters (kSpellStats) are compiled in but off.
