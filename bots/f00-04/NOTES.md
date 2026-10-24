## Strategy (≤10 lines)
f00-04, immigrant (big-game hunter) built from the starter's infrastructure (main loop, draft/bot split).
The whole team hunts as one pack: shared memory byte 0 holds the target camp group; any champion whose target is
invalid scores every group (gems and gold over path time + kill time, a risk factor from camp damage vs team health)
and writes the winner. Golems and monoliths are attacked only when the estimated camp damage fits the team's health
(kBigRisk); the pack banks gems first and gathers before waking them. A champion carrying >= 60 gems (a monolith
haul) broadcasts its position and the pack escorts it to the altar. Harmless cluster groups within 3 tiles are
farmed and captured solo while the pack moves on. Enemy champions: fight when the local health x damage ratio
favours us, back off otherwise; enemies on their own spawn are ignored. Builds: tanks Jungle(health) then Monster,
damage dealers Monster(strength) then Jungle, then unlimited Power shards. Maps whose altars are walled in behind
camps (01boss-battle) switch the scoring to gold (the tiebreak).

## Key parameters
- Draft order: Vesper Vale, Ryth, Vendetta, Willow, Marcella, Varg, Tavi, Ren (first free entries).
- Target score: value = gems x (1.5 before r300, else 3) + gold x (0.5 before r1200, else 0.2); score =
  value / ((eta + respawn wait + hp/team_dps + 6) x (1 + taken/ehp)); team_dps = n x own strength x 1.25,
  ehp = n x own max health; skip if taken > 0.8 x ehp (small) or kBigRisk = 1.0 x ehp (golem/monolith).
- Big camps: tribute first if carrying >= 5 gems; wait for 2 (golem) / 4 (monolith) allies within 6 and own health
  >= 80%; after 30 waiting turns need one fewer; after 70 give the camp up for 150 rounds.
- Tribute: >= 25 carried gems (12 if an enemy champion was seen in the last 40 rounds), >= 6 with an altar within
  6-8, any amount from round 1850 or when it would win. Walk to the nearest reachable altar approach tile.
- Retreat: kRetreat 35% health, kRetreatEnemy 50% with an enemy champion within 5; step out of range, then recall.
- Engage enemy champions: ratio (our hp sum x dps sum)/(theirs) >= 0.9, or >= 0.6 if one is already in range.
- Escort: kEscortGems 60. Side farming: kSideRange 3, give up after 25 turns or 8 turns without an attack
  (then banned 100 rounds). Shop recall at >= 240 gold with no gems and nothing threatening.
- Shared memory: [0] target group + 1, [1..3] escort (x+1, y+1, round), [4..7] two (group + 1, avail/16) notes.

## Changes this generation
Generation 0 immigrant: whole new strategy (see above). Final build vs starter (benchmark.py, all 40 maps, both
sides): 79/80 wins, mean margin +779, 0 faults, 0 caught exceptions of ours (the one flagged game, 01yellow-brick-
road B, is a starter `bot error` and our only loss). Sparring (not the evaluation panel): vs e02-01 9/12 on 6 maps,
vs e05-05 6/12, vs e07-01 2/16 (it out-fights and out-farms us).
Fixes that mattered most, in order found: standing on a captured group's spawn tiles blocks its respawn (deadlock);
paths flipped as camps left vision (remember camp tiles as blocked); targets walled off behind other camps (route
target scoring around living camps); fighting enemies who heal on their own spawn; capture contested by an enemy at
the far end of a 30-tile group (go and fight it); shop-recall loop once the build list ran out; altars only reachable
after killing a monolith (gold mode); nearest altar by Chebyshev distance unreachable through holes (01islands, path
search over all altars); gather deadlock at the monolith; lone monolith carrier killed on the way to the altar
(escort); side-farming re-picking an unreachable cluster (ban).

## Tried and failed (keep entries from the parent's notes; add yours)
- Two squads (3 + 2) for parallel farming, big camps pulling both: vs e07-01 0/16, 414 deaths vs 297 for one pack.
- Single capturer (first to see the reward stays, pack moves on): more gems collected but lone carriers died with
  them: lost gems 4683 vs 2063, 0/16 vs e07-01.
- Local strength ratio engage/back-off rule alone (before the earlier retreat): no gain vs e07-01 (-545 vs -388).
- Tribute thresholds 30 / 20 (danger) instead of 25 / 12: fewer trips but more gems lost, -604 vs -388.
- Side-farming radius 5 instead of 3: vs e02-01 5/12 (+36) vs 9/12 (+193) on the same 6 maps.
- Cheaper `observe_groups` (per-tile checks, `vision::camp_groups()` only for seen groups without camps; it costs
  ~35k of the 100k fuel per turn now) plus a per-turn alive-camp mask: same margins vs starter on 20 games (+878 vs
  +880), 1.4% more rounds, and 01islands A fell from 1020 to 88 gems. Not adopted; worth retrying with more games.
- Pitfalls: a text edit turned `me.recall()` inside `safe_recall()` into a self call (stack overflow traps, counted
  as faults); a failed target search repeated every turn exceeded fuel (now retried every 8 rounds); `restrictions()`
  is used to skip stunned turns (recall while stunned threw "Champion cannot act", a `bot error`).
- `bitboard::export_linear_buffer()` is not a plain W x H row-major layout; use `is_set` for map dumps.

## Known weaknesses
- Strong fighting teams (e07-01) kill us ~20 times a game; our fight logic is simple (no shared focus target, no
  kiting, spells fire in a fixed order) and the pack loses gems on deaths.
- 01yellow-brick-road: slow and erratic (30-tile camp rows split the map); behaviour there shifts with fuel use.
- Turns can use 60-80k fuel while travelling (observe_groups ~35k, distance fields ~20k); a turn that runs out is
  resumed next round on stale data (rare "Known occupied destination" warnings, caught).
- Team strength for target scoring is estimated from the deciding champion's own stats only.
