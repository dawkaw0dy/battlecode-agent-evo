## Strategy (≤10 lines)
Split farm (from f00-03) with a new fight module (crossover with f00-00's engage/hold/retreat model).
The draft splits the camp groups on our side into one angular sector per champion; each champion farms alone,
scores groups by value / (travel + respawn wait + clear time), wears down strong groups over several trips,
tributes at the nearest reachable altar and recalls to heal/shop.
Fights: force ratio (Σhp)·(Σpower) of us (allies within 6) vs visible enemies within 8 walking steps decides
engage (≥1.3, or a finishable target at ≥0.6), hold, or flee (<0.85, or below 30% hp). Fleeing uses threat-aware
steps (out of each enemy's reach next turn, homewards) and dashes, recalls once 6+ tiles clear; we only stand and
trade blows when an enemy can hit us anyway AND we can kill a target before dying. Victims: killable this turn,
then gem carriers, then lowest hp; spells first, attack reset after a swing. Tiles in reach of enemies parked on
their own spawn are avoided.

## Key parameters
- Picks and farming parameters unchanged from f00-03 (see its notes): Vendetta, Marcella, Vesper Vale, Tavi, Varg.
- Ratio = (our hp sum)·(our power sum) / (their hp sum)·(their power sum); power = str + 0.3·focus, min 20.
- engage ≥ 1.3 or (finishable: weakest hp ≤ 1.2·burst within 4, ratio ≥ 0.6); flee < 0.85 or hp < 30%;
  hold in between; hold turns to flee while a retreat started in the last 10 rounds is running.
- Trade instead of fleeing iff in range, someone can hit us next turn whatever we do (melee adjacent,
  ranged within range-1) and turns-to-kill ≤ turns-to-die + 0.5 (burst = str·(1+reset) + ready spell).
- Chase on engage when the enemy is within 6; 12 chase steps without a hit → 30 rounds without chasing.
- Spawn campers: reach 2 for melee, attack range for ranged; our spawn tiles are never "hot".
- Assist allies' reports: ≤ 2 enemies, ≤ 12 tiles, hp > 60%, < 40 gems (kHelpRadius/kHelpCount).

## Changes this generation
- New handle_enemies (engage/hold/flee, trade rule, victim choice, fight spells, chase cap), flee_step with
  dash escape, retreat memory (flee_until_).
- Pathing fixes found while fleeing: dashers walk a no-dash home field when the dash is on cooldown (they used
  to wait up to 12 turns beside a hole while being hit); no 5-turn "gap" waits while running home.
- Spawn-camper zone: enemies on their spawn heal every turn; we now path around their reach and step out of it.
- Results (benchmark.py, release, both sides): vs f00-00 62/80 (parent A: 45/80), vs f00-03 44/80 (+81 mean,
  kills 923 vs 473), vs starter 39/40 on 20 maps. 0 bot errors / faults in the last 224 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Linear bitboard buffers are bit-packed (not one byte per tile as the API doc says).
- Every bitboard/field query is a host call (~100+ fuel); per-tile loops (add_box with set(), is_set/get in
  loops, camp_at for every visible camp) blew the 100k budget on maps with 30-camp groups. Use exported
  local copies and whole-board shifts.
- Turns that run out of fuel resume next round on stale state: moves into newly occupied tiles and attack
  resets throw. Guard every action with "still the same round".
- Recalling to shop at 150 gold between fights slowed farming; 250 gold with the "not engaged" check is fine.
- Requiring no incoming camp damage before tributing stalled champions next to altars; tributes are not
  interrupted by damage.
- Diagonal dashes can be refused by the engine ("unreachable within movement range"): try candidates in order;
  only straight one-tile holes are treated as jumpable when planning.
- Mixing a static field and a unit-aware BFS made champions flip-flop between two tiles; after a blocked static
  step use the dynamic route for 12 rounds, and clear a camp that plugs a gap instead of detouring.
- Tavi standing on spawn did not heal for us; never wait on spawn more than two turns.
- Altar tiles occupied by camps (boss beside the only altar) must not be planned as tribute spots.
- Raising the tribute threshold (25 gems within 8) was neutral; kept the lower one for safety.
- Region weight for another champion's sector: 0.6 was clearly slower (+5% rounds on 8 maps), 0.3 slightly
  slower; 0.45 kept — the split itself pays.
- Trading blows whenever fleeing cannot avoid hits (hp ≥ 12%): 0/24 on f00-03's best maps; the kill-reachable
  condition gave 10/24 and +6200 gems vs f00-00 on 16 maps.
- Also trading when home is farther than turns-to-die ("doomed"): worse vs both parents (fleeing champions
  often survive because the chaser gives up).
- Counting enemies seen in the last 4 rounds (out of sight) in the force ratio: worse vs both parents (flees too much).
- Keeping up a retreat out of sight at full health, together with the memory above: -3500 gems vs f00-00.
- Answering ally reports of up to 3 enemies within 15 tiles: +1700 vs f00-03 but -4500 vs f00-00 on the test
  sets; kept 2 / 12.
- No-progress rule (target alive, no move/attack for 25 rounds → skip it 150 rounds) did not fix
  01yellow-brick-road side B vs starter (reverted, untested elsewhere).

## Known weaknesses
- 01yellow-brick-road as team B vs starter: 0 gems all game (the parent also loses there, 160-229): the
  30-tile gold-cluster group next to both spawns is farmed but its capture never completes.
- Still a split farm: lone champions lose to packs (00lost-world, 00gemfight-A, 00rushbait-v2, 00elkring vs
  f00-00); 01islands/00split/01corridors-B/01starvation-B lost to f00-00 on farming, not fights.
- vs f00-03: 00pilgrimage, 01shortcut, 01concentrated-wealth, 01starvation lost on both sides.
- No wards, no altar locks of our own, no healing support logic.
