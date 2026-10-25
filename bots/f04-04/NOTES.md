## Strategy (≤10 lines)
Scaling economy (from f01-07/f02-02, unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions per
champion; groups scored by value per champion-turn with a camp damage model, raids for bosses, gold into shards.
Combat from the parent f03-04 (turn-by-turn focus-fire simulation, engage at value > −150, retreat/recall otherwise).
This generation (camps focus): raids are sized from every living ally, not only the ones in view, and a monolith or
golem raid takes at least 3 champions (or everyone, if fewer are alive). Outside the focus, two small tribute rules:
enemy altar locks are broken (submit_gems at a locked altar is converted to an attack on the lock) instead of
skipped, and walled-off altars no longer count when deciding that every usable altar is locked.

## Key parameters
- kBossRaidMin = 3: smallest raid on a monolith/golem (min with the living team size, raised to claimers + 1).
- Raid sizes count living, non-recalling allies through their handles (positions are readable for teammates).
- Unchanged camp parameters: kCaptureTurns 6, kTripTurns 8, kRallyRounds 35, kGatherTurns 10, kBossTurns 8,
  kShareTurns 15, home-slot factors 0.8 (shared) / 0.6 (other's), enemy-side 0.7, reward 1.3, current target 1.25,
  sighting 0.5, gold weight 0.35 (Vesper 0.5) × max(0.4, 1 − round/1500).
- Locks: a lock within 4 is broken on the spot; an altar is skipped while we last saw an enemy lock on it; when every
  reachable altar is locked we walk to the nearest one and break it.

## Changes this generation
All runs tools/benchmark.py, both sides, release; 0 faults and 0 `bot error:` lines in all 859 games.
- Parent copy vs f03-05 (40 maps): 37/80, margin −130. The evidence losses (0 gems on 01boss-battle, 00stampede,
  00collision) were mostly f03-05's altar locks: the parent skipped locked altars, recalled to bank, found nothing
  and walked out again, carrying up to 1900 gems to the end.
- Kept, lock breaking (tribute section, outside the focus): 50/80, +152.
- Kept, ignore walled-off altars in the "all locked" rule (00collision's second altar sits in a closed pocket, so
  Vesper carried 870 gems for 1000 rounds): 50/80, +158; 00collision margins −1027/−862 → −245/−371.
- Reverted, enemy-side group factor 0.7 → 0.45: 46/80, +142.
- Kept, R1 raid size from all living allies (choose() counted only allies in view; traces on 00monolith showed
  2-champion monolith raids taking it from 20960 to 13400 hp and giving up): 49/80, +221 (banked 65.5k vs 62.7k).
- Kept, R2 boss raids ≥ 3 champions: 53/80, +238 (wins on 00monolith A, 00reverse-spiral A/B, 00contest A, 00stampede A).
- Reverted, R3 kBossRaidMin 4: 52/80, +235.
- Reverted, kRallyRounds 35 → 60 (18 boss maps): 27/36, +336 vs 28/36, +290.
- Final version: vs f03-05 53/80 (+238), vs f03-04 39/80 (+9), vs f02-01 60/80 (+255), vs f01-07 29/40 (first 20
  maps, +278), vs f00-03 32/40 (last 20 maps, +427), vs starter 20/20 (10 maps, +893).

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
- Hysteresis again, on the simulated fight value (±60 in favour of the last decision): 14/32 vs 17/32 without.
- Engage bias −300 (fight almost anything): 16/32 vs 23/32 at −150. Bias 0 or +60 (cautious): 16–17/32.
- Trading blows when the estimated damage of fleeing exceeds our health: most deaths in fights came from it.
- Retreat scoring without camp damage: champions stood under elk fire choosing between two equally "safe" tiles.
- Allies counted only within 7 of us: one step changed the count, so a champion flip-flopped engage/retreat
  beside a camp until it died. Count allies near the enemies too.
- Enemy-side camp factor 0.45 (from 0.7) to cut deaths on contested maps: 46/80 vs 50/80 against f03-05.
- Boss raids of at least 4 (instead of 3): 52/80 vs 53/80; rally wait 60 rounds (instead of 35): 27/36 vs 28/36.
- One parameter change flips 15–35 of 80 games against f03-05 (games are chaotic); a ±4-win difference is noise.

## Known weaknesses
- Fight-heavy maps are still lost on both sides vs most opponents (00contest, 00swirl, 01hooks, 01elk-arena,
  00reverse-spiral): deaths far exceed kills, and contested central camps (00stampede's only gem group) go to the
  side that wins the fights.
- Against lock users each tribute costs five hits on a lock and a relock cancels the channel: banks in big bags are
  slow; no locks of our own.
- 01boss-battle's only altar is reachable only from the monolith's spawn tile: tributes there need the monolith pair
  dead; the plug rule still sends single champions at it (we lost this map on both sides to the parent f03-04).
- The simulation ignores walls between fighters and assumes everyone stays to the end; allies decide separately.
