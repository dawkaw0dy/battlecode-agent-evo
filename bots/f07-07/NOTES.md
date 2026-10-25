## Strategy (≤10 lines)
Immigrant (early aggression) built on the starter's infrastructure. Draft early-power champions: Vendetta, Marcella,
Ryth, Tavi, Vesper Vale (then Varg, Heart of Ruin, Willow, Ren, ...). Each champion farms on its own: camp groups are
scored by value per turn (multi-trip clears for groups too strong for one trip, big groups shared, claims in shared
memory), attacked from the group's edge, captured, gems banked at altars (sooner when enemies were seen, under fire
when we outlive the channel), gold turned into shards (jungle emblem, then power shards). Whenever enemy champions
are in sight a turn-by-turn focus-fire simulation (each side pours damage into the weakest unit it can reach, units
join after the turns they need to close in) decides: winning fights are taken and announced (healthy allies within
14 tiles converge), kills are chased for up to 6 non-closing turns, losing fights are left early. Hidden damage
(enemies in bushes) triggers a retreat; wards go into hidden bushes next to us.

## Key parameters
- Fight: simulation 8 turns; value = 300 + 2·gems per kill on either side, 0.1 per hp lost; engage if value > 150
  and we do not die without a kill (or value ≥ 300 when we die). Allies/enemies remembered 2–3 rounds (bushes).
- Chase: 6 turns without closing in (3 measured worse), dropped at the enemy spawn's doorstep.
- Retreat when enemies are within (melee 4 / ranged range+2); recall at 30% hp or when camp fire would kill us on
  the way out; walk home instead of recalling while enemies were seen in the last 4 rounds.
- Bank at 20 gems (10 when enemies were seen within 10 rounds), any gems when an altar is ≤ 5 tiles away.
- Shopping recall at ≥ 300 gold when nothing is going on.
- Target value = gems + gold·w (w = 0.5 → 0.3 → 0.15 over the game); big groups (> 3000 hp) shared by claimers.

## Changes this generation
- New bot (immigrant). First version moved as one pack behind a leader: 0/12 vs f06-05, 0/6 vs f00-03 (economy at
  a quarter of theirs, vision flicker in bushes split every fight). Rebuilt as solo farmers + fight layer.
- Square-law fight test → focus-fire simulation: 0/12 (−777) → 5/12 (−183) vs f06-05 on the same 6 maps.
- Fixes that mattered: escape from camp fire (champions froze in elk range and died), leaving spawn before the heal
  tick (recall loop), hidden-damage retreat, camp-aware target distances + stuck detection, waiting inside a group's
  area is not "stuck", retrying a step when a hidden ally holds the tile (removed ~80 caught exceptions per 20 games).
- Later fixes: teleport-crystal island crossing (01islands, only when islands hold ≥ 30% of the map's value), waiting
  at a respawning best target, big groups planned as raids (two helpers assumed: S10 8/20, −175, best measured),
  claim penalty only for lower-index allies (all five herded onto one group and left it together every 8 rounds:
  01big-world 45 → 574/505 gems, 00contest 0-400 → 1000-0 both sides; S10 itself 5/20, −311, within its noise).
- vs f06-05: S10 5/20, other 24 maps 15/48 (before the claim fix). vs f05-02 9/10, f04-05 10/10, f03-05 10/10,
  f02-01 9/10, f00-03 10/12 (5–6 maps each). vs starter 8/8, rounds-to-win 1036 mean (f06-05: 978) on 3 maps.
- Game budget: 718 games played (counted conservatively).

## Tried and failed (keep entries from the parent's notes; add yours)
- Whole-team pack with a leader broadcasting its position: allies in bushes are invisible, the leader dove alone,
  fights split; 0/12 vs f06-05. A "near the leader" mask and a leash did not fix it.
- Opening invasion (all five to the enemy's nearest camp until round 110): 3/20 (−579) vs 5/20 (−398); by round 200
  kills 34 / deaths 55 vs 26 / 40 without.
- Retreating champions calling for help (allies converge on a chase): 2/20 (−510).
- Chase limit 3 (and dropping chases through camp fire): kills 172 vs 316 on S10, 1/20 vs 5/20.
- Leaving early when the simulation is losing and enemies are within 8: 2/12 vs 5/12.
- Our simulated damage × 0.75 (fight less): 3/20 (−574) vs 5/20 (−398). First-turn burst in the simulation: worse.
- Retreat steps weighted homewards (home 10 : threat 3): deaths 292 vs 253.
- Engage threshold 300 (5/20, −314) and Varg instead of Ryth (4/20, −378) vs 7/20 (−232) at the time.

## Known weaknesses
- 01boss-battle: the only altar is walled in except one tile held by a monolith; we bank only after it falls.
- No dash pathing over one-tile holes; teleports only on island-heavy maps.
- Farming races on big maps (00ambush, 00quadrants, 01shortcut) still lost to f06-05.
- f06-05 snowballs some maps from the first fights (01the-farm B, 01gold-rush B: 1–3 kills vs 14–22 deaths).
