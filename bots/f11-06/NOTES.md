## Strategy (≤10 lines)
Map reader, immigrant built on the starter's main loop. The draft (10x fuel) measures the map once: groups (value,
health, damage, kill order), walking distance from both spawns, altars, spawn gap and team size, and writes a plan
into draft memory (picks, mode, bank threshold, group classes, raid targets). Champions farm solo: every group is
scored by value / (travel + respawn wait + clear time + capture), where a kill-order damage model decides between
"go now", "recall first" and multi-trip attrition (camps never heal: fight until health runs low, leave their reach,
recall, return). A short focus-fire simulation decides fights (never chase; always take a kill in reach; flee
towards home through the tiles fewest enemies and awake camps reach; hold our spawn when they camp it). Banking
picks an altar by walking distance plus penalties for enemy locks and recent enemy sightings; every bag is banked
when either side nears 1000. We carry an altar lock and lock altars we pass or where an enemy tributes.
Plan-level raids only where one group (or bosses) hold most of the map's gems: raiders gather outside its reach.

## Key parameters
- Picks (all maps): Vendetta, Vesper Vale, Marcella, Tavi, Varg (cut to team size). Shards: jungle(str) → power(hp)
  → monster(str) → power(str) → durability(hp) → alternating power; Vesper opens with treasure(str).
- Target score: (gems + w·gold) / (time + 3), w 0.4 → 0.25 (r700) → 0.1 (r1200), 0.05 near the win; ×0.6 enemy side,
  ×0.85 contested, ×0.8 enemies seen there in 30 rounds, ×0.2 near a fled-from spot (20 rounds). Re-chosen every 8
  rounds, never while engaged (hit in the last 3 rounds), switch only for 1.4× the current score. ≤3 extra trips.
- Trip budget: leave a group when hp − max(3, boss range − our range + 2)·incoming < 20% max hp (unless it dies in
  2 turns of our team's damage). Shop trip when gold ≥ 250 between targets; long respawn waits recall to shop/heal.
- Spawn heals only the turn a recall lands (walking onto spawn does not heal): wait there exactly that turn.
- Fight: engage at simulated value > −60 when someone is within strike reach (ranged: range; melee 2, +2 dash);
  flee when danger > 0 or (value < −100 and an enemy within 7). Enemy memory 2 rounds. Recall < 30% hp, < 50% hp
  with enemies seen in the last 30 rounds.
- Bank: plan threshold 25/40/60 by spawn gap, ≥15 after a recent sighting, ≥8 within 4 tiles of an altar, any bag
  once the enemy has 800 or we have 750. Altar cost: walk + 10 (enemy lock) + 15·(1 − age/40) (enemies seen).
- Raids: only when bosses hold ≥80% of the gems (golem pow 75 / monolith 110, from round 200) or one group holds
  ≥50% (pow 0; pow 90 / round 400 when it deals ≥200 per turn); need 2 raiders (4 for ≥200 dps groups).
- Fuel: target search capped at 70 steps (farther groups: spawn-distance estimate), kill orders precomputed, cached
  static routes for long walks on maps ≥ 80×80, optional re-evaluations skipped below 40–55k fuel.

## Changes this generation
- New bot (immigrant, map-reader archetype) on the starter's infrastructure; ~800 local games.
- Biggest fixes, each found by tracing: oscillation between two tiles when camps flicker in and out of sight (unseen
  camp tiles now count as blocked); champions leaving spawn the turn their recall landed (no heal → recall loop);
  waiting 30+ turns on spawn for a heal that never comes; a bail-out rule that left every strong group after two hits
  (recall/walk/hit loops); bank trips flipping between a locked altar and a far one (lock memory); stale incremental
  builds (header changes not recompiled: always rebuild from clean).
- vs starter mean rounds to win on 4 maps: 1141 → 790 (f10-03 791, f00-03 833).
- vs f05-02 (6 maps, 12 games): 7/12 → 8/12 with altar danger memory; vs f10-03: 0/16 at first, final 5/20 (benchmark,
  10 maps, both sides). 40-map sweeps across the 12-bot panel: 13/40, 17/40, 14/40; 0 faults, 0 `bot error:`.
- Altar locks (carry one, lock altars passed): vs f10-03 6/12 (−138) vs 4/12 (−388) without.
- Race banking (any bag once a side nears 1000): mean margin −187 → −137 on 5 maps vs f10-03.

## Tried and failed (keep entries from the parent's notes; add yours)
- (f11-06) Squads: one ball of all champions, or pairs, following the leader's claimed target: on 00gemfight+01hooks
  vs f10-03 0/4 (ball) and 0/4 (pairs) vs 1/4 solo; farming as a ball is too slow and the ball still splits.
- (f11-06) Walking home for 6 turns after every flee: deaths 96 → 51 vs f00-03 but farming collapsed (0/8).
- (f11-06) Boss raids on every map with a reachable golem/monolith: 00great-wall went from 2/2 wins to 0/2; raiders
  die to the enemy at the boss. Solo multi-trip on a monolith (range 5, 200/turn) makes ~3 hits per trip.
- (f11-06) Angular sectors per champion (draft assigns groups, ×0.5 outside your sector): 3/12 vs 8/12 (f05-02),
  1/12 vs 2/12 (f10-03).
- (f11-06) Pick sets: Ryth for Varg 6/12 (−93), Heart of Ruin for Varg 6/12 (−128) vs current 7/12 (+65).
- (f11-06) Lower bank thresholds (×0.6, 10 when wary): 4/12 vs 7/12; more trips to contested altars, same losses.
- (f11-06) Engage bias −150: 6/12 (−166); +60: 5/12 (−147); −60 kept (8/12, +188).
- (f11-06) Bonus for groups near an ally's claimed target (×1.3): 2/12 vs 6/12 vs f10-03.
- (f11-06) Value constant 12 instead of 3 and re-evaluation every 4 rounds: no difference vs starter (788/792 vs 790).
- (f11-06) Cached static routes on every map: more deaths (177 vs 117; they ignore visible enemies); big maps only.
- (f11-06) Hysteresis-free fight value flipped to "favourable" when one of two enemies stepped out of sight for a turn
  (walked into a burst): fixed with a 2-round enemy memory for fight/flee decisions only.
- (f11-06) Single 12-game samples move by ±3 wins between near-identical builds (chaos): judge on several sets.

## Known weaknesses
- Fights: we still die about twice as often as the lineage bots (they converge on sightings, stun-lock and chase);
  contested centres (01hooks, 00gemfight, 00rows, 00swirl, 01gold-rush) are often lost.
- 00contest and 01boss-battle score ~0: monolith raids rarely succeed; 01boss-battle's only altar sits behind the boss.
- We lose ~2× the opponent's carried gems on deaths; bags still die on the way to contested altars.
- No dash pathing over holes: island groups (e.g. 01big-world's four cluster islands) are never farmed.
- Debug traces (EVO_TRACE) and experiment switches (EVO_*) remain in the code, compiled out by default.
