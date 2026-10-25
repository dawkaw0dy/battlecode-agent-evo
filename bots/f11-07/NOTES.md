## Strategy (≤10 lines)
Scaling economy, immigrant built from the starter's main loop. Draft Vesper Vale, Vendetta, Marcella, Varg, Tavi.
Every champion farms alone in a "lane": the camp groups on our side of the map, sorted by bearing from our spawn
and cut into value-balanced slices. Groups are scored value / (walk + kill + capture + respawn wait + extra trips):
camps never heal, so a group too strong for one trip is worn down over several (each trip costs a walk home and
back); a group better done by two posts a raid in shared memory and a second champion joins before it is woken.
Gold goes into shards on spawn (Tavi anywhere): Treasure, then Monster (ranged) / Jungle (melee), then Durability,
then Power shards, stat chosen by relative gain (strength favoured 1.6x). Bags go to one chosen altar; enemy locks
are broken from attack range; every unlocked altar we pass gets our altar lock (10 gold). Fights are decided by a
turn-by-turn focus-fire simulation; spawn tiles (full heal each turn) are held when enemies wait outside them.

## Key parameters
- Target value: gems × (4 / 5 / 8 before round 500 / 1300 / after) + gold × (1.33 for Vesper); lane factor 1.0 own,
  0.6 other lane, 0.45 enemy side; ×0.5 when an enemy was seen at the group in the last 25 rounds.
- Respawn wait ≤ 20 rounds; hysteresis 1.3 on the current target; multi-trip: ≥ 5 turns of camp fire per trip,
  ≤ 6 trips, trip cost 2·(walk from spawn) + 10; raids: two-champion score with +15 turns, join bonus 1.3, hold ≤ 30.
- Fight sim: 10 turns, every unit opens with its burst (reset hit + damage spell) and hits the weakest opponent;
  worth of a death 160 + 3·gems; fight if value > 20 + gems/2 (and we survive, or the trade is > 1.5× our worth);
  hunt unengaged enemies at value > 120; chase gives up after 4 turns without a hit (12-round cooldown).
- Bank: ≥ 40 gems, ≥ 15 when enemies are about, altar ≤ 4 away, ≥ 12 within 10, game end, or the winning bag.
- Home: hp < 50% (60% when enemies are about), 90+ gold with an empty bag, 240+ gold; walk if spawn ≤ 6 away or
  someone unseen is shooting, else recall. Leave camp range when hp < incoming·min(turns left + 2, 6).
- Routing: static host fields (camp tiles passable) per group/altar/spawn, computed lazily; a blocked static step or
  any dynamic step switches to unit-aware dynamic routing for 8–12 rounds; remembered live camps block it.
- Fuel: target scoring pruned by an upper bound; no dynamic search under 35k fuel; every action checks that the turn
  is still in the round it started (a fuel-exhausted turn resumes later on stale state).

## Changes this generation
- New bot (immigrant, scaling-economy archetype). Main loop, draft and shared-memory plumbing from the starter only.
- Measured milestones vs f05-02 on 8 maps × 2 sides (16 games, mean gem margin): Lanchester fights −790,
  strength-leaning shards −709, per-altar banking −557/−585, sim + routing fixes −475..−569, raids −427,
  altar locks −275 (6/16). vs f00-03 on the same 8 maps: 3/8 → 7/16 → 10/16 (+117) → final 11/16 (+174).
- Final build (benchmark.py, maps 00eight 01gold-rush 00interlock 01yellow-brick-road, both sides): starter 7/7 valid
  (the 8th game is flagged invalid by the starter's own exception), f00-03 3/8, f01-07 2/8, f02-01 1/8, f03-05 2/8,
  f04-05 1/8, f05-02 2/8, f06-05 0/8, f07-02 0/8, f08-04 2/8, f09-01 0/8, f10-03 0/8 (first six on a slightly
  older build). 0 faults, 0 `bot error:` from this bot in the final runs.
- Game budget: about 840 games.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter parent: none recorded.
- Static fields with every camp tile as a wall: pockets behind camps read as unreachable (100000), which tripped the
  end-of-game bank rule and froze champions far from any altar. Camp-passable fields + sticky dynamic routing fixed it.
- Mixing a static field (through live camps) and a dynamic route at a fixed distance threshold flip-flopped between
  two tiles; so did a dynamic router that treated out-of-sight camps as free. Make dynamic routing sticky and block
  remembered live camps.
- Choosing the bank altar by the union field while the dynamic goal excluded locked altars: two-tile oscillation for
  hundreds of turns. Pick one altar and walk to that one.
- Squads (2 or 3 groups farming together, leader picks the target): vs f05-02 −864 vs −790 solo.
- Pre-emptive retreat from any enemy within reach + 5: −880 vs −811.
- Lower bank thresholds (25 gems, 10 when enemies about): no change (−702 vs −709).
- Shopping at 60 or 150 gold instead of 90: slower vs starter (1183 / 1191 vs ~1130 rounds).
- Wider lanes (other lane 0.8, enemy side 0.6): slower farming (1231 vs 1199 rounds vs starter); tighter: no gain.
- Strength bias 3.0 in shard choice: −795 vs −709 at 1.6.
- Lanchester (dps × hp) fight test: deaths came from low-hp champions staying in "won" team fights; replaced by the sim.
- Melee chasing a kiting ranged enemy lost 400 hp for no hit; chase limit added (gain not measurable alone).
- choose_target with raid scoring on 01yellow-brick-road (40+ groups) used ~70k fuel; the dynamic step then ran out of
  fuel, resumed next round and threw "known occupied destination". Prune by an upper bound, reserve fuel, check round.
- Results are extremely chaotic: single maps swing by ~1000 gems between near-identical builds; 16 games are noise
  for differences under ~150 mean margin.

## Known weaknesses
- Fights against the f05+ lineage: they converge on our lone farmers and spawn-camp on small maps (01hooks: ~50 deaths).
- Big sparse maps (01big-world): champions spend 35–55% of turns waiting for respawns in their lane.
- No boss (golem/monolith) raids; big groups are excluded from lanes and rarely feasible.
- Elk raids are two-champion only and the joiner is whoever scores it best, not the nearest.
- Vs the starter we are slower than the panel (≈1000–1300 rounds to 1000 gems vs ≈820–1050 for f10-03).
