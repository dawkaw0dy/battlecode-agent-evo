## Strategy (≤10 lines)
Immigrant (vision-ambush archetype), written from the starter's infrastructure only. Draft Vesper Vale, Vendetta,
Marcella, then Willow (4+), Tavi (5). Every champion farms on its own but publishes its target group in shared memory
(bytes 0-4); a group's score is our damage share of its value over (walk, respawn wait, clear, capture, trips home),
and the coalition a strong group needs is sized from the claimers plus whoever else would have to come (focus-fire
damage model: incoming drops as attackers die, absorbed by the coalition's health and Willow's heal). Bytes 5-7 hold
the last enemy sighting (pos, round, carrying ≥ 10 gems, several): allies converge on lone enemies, avoid groups near
packs. Fights: an 8-turn focus-fire simulation with first-hit burst, allies near us or near the enemies, against the
cost of fleeing. Altar locks are dropped on enemy tributes (cancels the channel); wards go into unseen bushes nearby.
Bank by altar distance (15-40 gems), and everything once a race to 1000 is close.

## Key parameters
- Gold worth 0.35 gems (0.5 for Vesper Vale) × max(0.4, 1 − round/1500) (kRichGold).
- Fight: engage when simulated value > flee value − 60 (kEngageBias); retreat when doomed, low (< 40%) under threat,
  or hit by champions with value < −60; allies under 25% hp are not counted.
- Banking: gems ≥ clamp(altar distance, 15, 40), within 3 of an altar, ≥ 15 with enemies in view, our submitted +
  carried ≥ 1000, or enemy submitted ≥ 850 and ≥ 5 carried.
- Trips: a group needs ≤ 3 trips home (damage / (0.75·(tank + 0.5·others) + heal·t)); staging waits up to 30 turns
  for claimers when > 1.2 trips; bosses (str ≥ 120) wait up to 40 turns for every claimer (kRallyBoss).
- Re-plan every 6 rounds (12 when the turn is already past 35k fuel); scoring stops at 30k fuel left.
- Shop trip home when gold ≥ 150 + 4·walk home; recall below 35% hp out of reach of camps and enemies.

## Changes this generation
- New bot (no parent). Evaluation: tools/benchmark.py, release, both sides, 20-map halves (set A = every other map
  alphabetically from 00ambush, set B = the rest). Final build: vs f07-02 9/40 on set A (mean −269); vs starter 19/20,
  vs f01-07 16/20, vs f05-02 6/20 on set B halves; an earlier build 19/40 vs f03-05 on set A. 0 engine faults and
  0 `bot error:` lines in the last 140 games.
- Gap to the panel is farming, not fighting: vs f07-02 over 40 games champion damage 54k/52k and kills 26/27, but
  camp damage 93k/126k and gold 7.5k/9.1k. We lead until round ~800 (submitted 106 vs 66 at round 400); they pull
  away after 1000 (boss kills, more shards).
- Fixes that mattered (each found from a stuck champion in a trace): unreachable groups (holes) skipped; LOS approach
  falls back to adjacent tiles for camps in bushes; shadow_cast from an opaque origin throws (target in a bush);
  greedy static-field steps and BFS detours oscillated (detour mode for 8 turns); camps plugging a corridor need an
  unbounded BFS; altar spots plugged by a boss (01boss-battle) give up banking for 40 rounds; a "pickup" cluster
  behind a wall flipped every turn with the capture target (walking distance now, capture never interrupted).
- Fuel: observe() read groups through vision::camp_groups() (22k/turn): stored handles + visibility test instead;
  the coalition clear cost is a closed form K/dps cached per round (was ~75k per re-plan on 160-camp maps).

## Tried and failed (keep entries from the parent's notes; add yours)
- Two fixed squads following a leader: the leader banked alone with all the captured gems while members idled;
  the second squad spent both champions on gold clusters. Replaced by claims.
- Share penalty (a short clear somebody claims is worth 0.3× to others, as f03-05 does): 9/39 vs 19/40 vs f03-05.
- The opposite, a 1.3× bonus for groups allies claim: 15/40 vs 19/40.
- Strong groups waiting 40 turns for every claimer (kRallyAll): 9/39; bosses only (kRallyBoss): 9/40 vs 8/40 off.
- Capture priority (Marcella captures gem groups for her +2 gold per tributed gem, Vesper Vale gold groups):
  10/39 (−321) vs 10/40 (−241): the walk to the tile costs more than the bonus.
- Engage bias −150: 13/40 but mean −265 vs −217 and more deaths; kept −60.
- Converging on every sighting within 10: walked into packs (gold-rush 7 kills vs 28 deaths); only lone enemies now.
- Retreat only when hit by champions (kRetreatOnHit): no measurable change.
- Square-law fight check (hp·dps ratio): incoherent team decisions and 58 deaths on 01hooks; the focus-fire
  simulation took 01hooks from −924 to +946 vs f03-05.
- Dash then walk can be refused ("cannot move") although can_move() is true; walks are wrapped and stop the turn.
- Single-map results are noise: nearly identical builds flip half the maps; judge on 40-game runs and mean margin.

## Known weaknesses
- Farming throughput after round ~800: no effective boss raids (monoliths die late or never; two adjacent
  monoliths hit for 400 per turn), fewer shards than the panel.
- 01boss-battle (the only altar's tiles are a double monolith's spawn): 0 gems vs ~1100.
- Contest maps decided by the central group (01elk-arena, 01gold-rush, 00contest) are lost once behind in gold.
- Fight decisions are per champion from its own vision; allies still disagree sometimes and fight piecemeal.
- Game budget: about 905 games played (benchmark runs plus single debug games), slightly over the 900 allowed.
