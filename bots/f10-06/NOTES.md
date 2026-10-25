## Strategy (≤10 lines)
Immigrant (big-game hunter), written from the starter's infrastructure. Draft Vendetta, Ryth, Willow, Vesper Vale,
Marcella (hard hitter, tank, healer, gold carry, second tank). Each champion scores camp groups by value per
champion-turn ((gems·3→5 + gold) / allies sharing / (walking distance + clear time + capture + extra trips home
when the camps outlast its health)); camps do not heal, so ordinary groups are chipped solo across trips.
Golems/monoliths are raids: wait (10 rounds) for a party of 2 outside their reach, then commit; boss health is
shared in shared memory so every champion sees the raid's progress. Shards: Jungle health / Monster strength
emblems first, then power shards. Fights: engage when we kill them faster (×1.2) than they kill us, else walk
away; bank gems by a bag-vs-altar-distance rule; hidden shooters (unexplained damage) get a ward in the bush and
no recall; scouting wards while enemies are about; paths avoid the reach of awake camps we are not fighting.

## Key parameters
- kEngage 1.2 (fight when t_kill < 1.2·t_die; 0.7 with > 20 gems), kThreatPad 5 (retreat once an enemy is
  within its range + 5 and the fight is not ours).
- kFleeHp 0.30 (0.45 within 30 rounds of an enemy sighting), kFleeHits 4 (leave camp fire with 4 hits of hp).
- Banking: gems ≥ max(12, altar distance × 1.0), or ≥ max(6, distance × 0.5) when enemies were seen in the last
  25 rounds, or ≥ 45, or altar within 4, or round > 1800, or before a target with ≥ 100 camp strength.
- Raids: kRaidParty 2, kRallyWait 10. need_for(): champions needed so camp fire stays below 0.55 hp each.
- Stuck: kStuckTurns 12 (no step and no hit) and kNoProgress 120 (no hit on the target) → group blocked 80/150
  rounds. Groups unreachable on foot (terrain BFS) are skipped. kAvoidRounds 15. kPack 0, kHelpRange 0 (off).
- Shared memory: bytes 0–4 each slot's target group + 1; bytes 5–7 boss group + 1 and its health / 2.

## Changes this generation
- New bot (immigrant). Milestones vs f05-02 on the 8-map set (00great-wall, 00split, 01gold-rush, 00elkring,
  00lost-world, 01corridors, 00quadrants, 00collision; both sides): first version 1/16 (−764, 404 deaths vs
  f09-01 in 16 games) → flee fixes 3/16 → banking 4/16 → engage 1.2 6/16 (−178) → distance-scaled banking 7/16
  (−103) → ward reveal 8/16 (−66, deaths 274→211) → scouting wards 8/16 (+31) → final 7/16 (−5).
- Monolith maps (00contest, 00monolith, 00rows, 01big-world, 01boss-battle, 01the-farm, 01starvation,
  01shortcut) vs f05-02: 1/16 (−560) → shared boss hp + stuck detection 1/16 (−539) → rally 20: 3/16 (−450) →
  rally 10: 4/16 (−329) → party 2: 5/16 (−247).
- Final, 20 other maps vs f05-02 (earlier build): 26/40 (+202). Panel on 00pilgrimage + 01fracture (both sides):
  starter 4/4, f00-03 2/4, f01-07 2/4, f02-01 0/4, f03-05 1/4, f04-05 1/4, f05-02 1/4, f06-05 0/4, f07-02 1/4,
  f08-04 1/4, f09-01 0/4. 0 faults, 0 `bot error:` lines in all runs after the stun/recall guards.
- Games played: 841.

## Tried and failed (keep entries from the parent's notes; add yours)
- (f10-06) Pack bonus (score × (1 + k·allies on the group)): k 0.6 4/16, k 2.0 2/16 vs k 0 5/16. Spreading wins.
- (f10-06) Rally (wait for the needed party) on every dangerous group: ~600 of 1500 turns per champion spent waiting
  on 01corridors. Only bosses rally now; others are chipped solo.
- (f10-06) Answering an ally's fight call within 12 tiles: 1/16 (−435) vs 5/16 (−208). Helpers walk into losing fights.
- (f10-06) Skipping groups with an enemy seen in the last 40 rounds (vs 15): 3/16 (−376) vs 5/16 (−208).
- (f10-06) Engage ratio 0.75: 4/16 (−278); 0.95: 5/16 (−231); 1.6: 4/16 (−255); 1.2 best: 6/16 (−178).
- (f10-06) Bank per tile 1.6 (vs 1.0): 2/16 (−429) vs 7/16 (−103).
- (f10-06) Flee threshold 0.4 (vs 0.3): 7/16 (−85) vs 7/16 (−5). Vendetta Jungle-first build: 8/16 (−44), more deaths.
- (f10-06) No rally wait at all for bosses: 3/16 (−346) vs 5/16 (−247) with 10 rounds.
- (f10-06) Bugs found on the way: standing on a respawning group's tiles blocks its respawn forever; melee
  champions that attacked instead of stepping away while fleeing; recall in a bush ambush (every hit breaks it).

## Known weaknesses
- Loses to most recent champions on most maps (13/44 on the final panel check). Deaths now match f05-02's
  (215 vs 225 in 16 games), but fights use a crude total-hp/total-dps comparison, no focus-fire simulation.
- Fleeing champions are chased down by same-speed melee and burst casters; ranged champions never kite.
- Monolith-only maps (00contest, 01starvation) still lost: the enemy last-hits or out-farms our boss raids.
- Boss report holds one group only; per-champion memory of other camps goes stale.
- Draft is fixed; no counter-picking.
