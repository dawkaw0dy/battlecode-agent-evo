## Strategy (≤10 lines)
Scaling economy (from f01-07/f02-02, unchanged): draft Vesper Vale, Vendetta, Marcella, Tavi, Varg; home regions per
champion; groups scored by value per champion-turn with a camp damage model, raids for bosses, gold into shards.
This generation rewrote the combat subsystem (Bot::enemies() and its helpers in the enemies section of bot.cpp).
Every visible fighter near us is read into a model (health + shields, basic damage, ready burst from resets and
damage spells, ready stuns, turns to come into reach, camp fire it takes, kill value = 150 + 2 × carried gems), and a
turn-by-turn focus-fire simulation (both sides hit their weakest opponent, overflow moves on, stuns cost the
strongest opponent a turn, spawn tiles heal fully) gives the fight's value. Engage when value > −150; otherwise
retreat to the neighbouring tile the fewest enemies and awake camps reach (towards spawn, dash when it helps),
recall once nobody is within 7. Always take a kill in reach this turn. Hold our spawn when only its heal wins.

## Key parameters
- Fighters counted: enemies within 9 of us (not on their spawn), allies within 7 of us or 8 of an enemy, not recalling.
- Kill value 150 + 2·gems each side; +60 × (their − our) health fractions lost; 10 simulated turns.
- Engage: value > kEngageBias = −150 and not (we die and kill nobody); gems ≥ 40: no fight we die in unless value ≥ 2·gems.
- Kill now: an enemy within reach (ranged: range, melee: 2) with hp ≤ 0.85 × (strength + ready burst).
- Retreat score: 4 × (enemy threat + awake camp damage on the tile) (× 0.1 on spawn) + walking distance home.
- Recall when the nearest enemy is ≥ 7 away and home is > 6 steps; cancel a recall when an enemy comes within 5.
- Spawn hold: up to 20 turns, shooting from the spawn. Answer an ally's sighting within 14 (was 10).
- Spells: stun prefers channelling enemies (+400) and high damage; walk-excluding casts while retreating only for a
  kill, a stun on a chaser within 3, or a channel; shield when an enemy or camp can hit us.

## Changes this generation
- Combat subsystem rewritten behind the same enemies() call; old square-law rule, Foe/foe memory removed.
- Tuning (16 fight-heavy maps × 2 sides vs the parent f02-02): bias 0 → 17/32, −60 → 21/32, −150 → 23/32,
  −300 → 16/32; hysteresis (±60 for the last decision) 14/32. vs f00-03 on the same maps: parent 19/32, ours 24/32.
- Death tracing showed 153 of 238 deaths-while-engaged came from "fleeing looks fatal, so trade": dropped it
  (kDoomedFights = false): vs f02-01 15/32 → 19/32, our deaths 898 → 764.
- Results (benchmark.py, 40 maps × 2 sides) of the version before the last tweak (sighting radius 14):
  vs f02-01 58/80 (+206), vs f01-07 62/80 (+311), vs f02-02 56/80 (+75 to +150 across runs), vs starter 16/16
  sampled; vs f00-03 40/48 on the 24 maps outside the fight set. Radius 14 on the fight set: vs f02-01 19/32
  (+146 vs +31), vs f02-02 26/32 (vs 24/32). 0 faults and 0 caught exceptions in all 892 games.

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

## Known weaknesses
- Snowball maps (00swirl, 01hooks, 01starfish) are still lost on both sides vs most opponents: once the enemy
  outscales us in items, every contested camp fight in the middle is lost (35 deaths vs 12 kills on 01starfish).
- The simulation ignores walls between fighters (an enemy across a hole counts as close) and assumes everyone
  stays to the end; the −150 bias stands in for enemies that run away.
- Allies decide separately; there is no shared target or engage call beyond the one sighting slot.
- No wards, altar locks or teleport crystals.
