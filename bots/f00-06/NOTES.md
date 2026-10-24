## Strategy (≤10 lines)
Deathball immigrant built on the starter's infrastructure (main loop, shop on spawn, draft dispatch).
Draft for every team size: Vesper Vale, Ryth, Tavi, Willow, Vendetta (then Varg, Marcella, ...): stuns, area stun, heal, burst.
One leader (8-byte shared memory: id, x, y, heartbeat, target group, mode, hunt x/y; stale heartbeat -> next champion claims)
picks a camp group by reward / (walking distance + clear time + danger), gathers the ball, engages, captures, banks, team-shops.
Followers flow down the same static per-group distance field and may lead by up to 3 steps (no corridor jams behind the leader).
Static fields (per group, altars, spawn) are computed lazily from spare fuel; dynamic multi-root fields route around units.
Fights: enemy champions first (lowest HP), spells (damage, stun, area, heal, shield), attack reset, dash to finish fleeing enemies.
Hunt only with local health superiority; retreat to spawn when outmatched (health ratio < 0.55). Enemy altar locks are broken.
Golems from round 300, monoliths from round 500 (with a full team in sight); awake ones outside the plan are avoided.

## Key parameters
kBankGems 12 (x1.5 if altar 13-25 away, x2 beyond), near-altar bank at 4 gems within 7, endgame bank 140 rounds before the end.
kShopGold 150 (team recall after banking; +60 when travelling), Tavi shops in the field. Follow 2, gather 3, gather wait 4,
lead-ahead 3, stall 90 rounds, still 12 rounds -> skip target, solo recall < 35% HP (step out of camp/enemy reach first).
kHuntBalance 1.1, kRetreatBalance 0.55, hunt at most 15 rounds. Field fuel reserve 25000 + 5 x map area (cap 85000).

## Changes this generation
New bot (immigrant, deathball archetype). Main results with the final or near-final build (bench2, both sides):
starter 78/80 on all 40 maps (0 faults, 0 bot errors; the 2 gold-rush losses were fixed by the monolith rule, then 10/10
on the affected maps); e07-05 13/16 +212, e04-06 11/12 +488 (6 big maps) and 8/12 on 3-champion maps, e02-01 14/16 +505,
e00-01 / e00-03 24/24 (8 mixed maps unless stated).

## Tried and failed (keep entries from the parent's notes; add yours)
- Debug output: `bench2.py` (`--log-limit 200000`) drops stdout lines; a single `cambc run` keeps them, but only flushes
  output some rounds later, so run a few dozen rounds past the round you want to inspect.
- `vision::camp_tiles()` still reports dead camps; blocking those tiles froze champions next to captured groups. Block only
  living camps (from `camp_at` + `state()`).
- A static field step into a tile held by a hidden unit (bush) fails every turn; remember the tile for 12 rounds.
- Static field + dynamic detour alternating each turn oscillates between two tiles; keep the detour for 6 rounds.
- Banking into a corridor blocked by a camp group: detours oscillate; fight the blocking group instead.
- Followers within 2 tiles of the leader jammed corridors and the spawn corner (leader boxed in for 2000 rounds on 00interlock).
- Treating enemies standing on their own spawn as targets pinned our ranged champions forever (00reverse-spiral).
- Monoliths never: 0-0 on 00contest and gold-rush losses; allowing them later turned 00contest into 1000-0.
- Cassi & Issac first / Willow / Vendetta / Varg / Vesper draft: 1/12 vs e04-06 on 3-champion maps, 2/20 vs e07-05;
  the Vesper/Ryth/Tavi-first draft: 8/12 and 11/16.
- Regroup mode (leader retreats to spawn when fewer than 2 allies are near after recent enemy contact): 8/16 vs 11/16.
- More aggressive hunting (balance 0.9, 25 rounds): 8/16 vs 11/16. Bank at 16 gems: +104 vs +187. Retreat at 0.8: worse
  than 0.55 vs e02-01 (12/16 vs 15/16); retreat at 0.45: 11/16 +98 vs 13/16 +212 vs e07-05.
- Using Tavi's damaging stun on camps whenever no enemy is visible: 7/16 vs 10-11/16 vs e07-05 (stun not ready for fights).

## Known weaknesses
- Monolith maps vs strong bots (01gold-rush, 00contest, 00rows): 2/8 vs e07-05; contested middles cost many deaths.
- 3-champion maps vs e04-06 remain side-dependent (00packs, 00collision side B).
- One ball covers large maps slowly; capture waits 5 rounds with the whole team present.
- Respawned champions walk to the leader alone and can be picked off.
- No wards, no own altar locks, no teleport crystals; groups behind holes are skipped.
