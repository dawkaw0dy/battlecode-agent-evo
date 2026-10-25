## Strategy (≤10 lines)
Camp sweep (immigrant, written from the starter's infrastructure only). Draft Varg, Vendetta, Vesper Vale, Marcella,
Tavi (fast camp clearers, gold makers, field shopping). Each champion farms alone: every 3 rounds it scores every camp
group by (gems + w·gold) / (walk + respawn wait + clear time + 6 capture + extra trips) using a strongest-first camp
damage model; claims in shared memory keep allies off small groups and let them join groups one cannot clear alone.
Walking uses per-group distance fields cached lazily (nearest-to-home first), so selection and travel are cheap reads.
Banks in batches scaled by altar distance (cap 50), breaks enemy altar locks, banks during respawn waits.
Foes: a turn-by-turn focus-fire model (us within 7, them within 10, arrival delays) decides engage vs retreat;
retreat steps outside every foe's reach towards allies and home; shared sighting byte steers targets away.
Shop: Monster emblem of strength, Jungle emblem of health, then power shards of strength.

## Key parameters
- kEngage 60 (fight value to engage), kThreatPad 2 (retreat when a foe is within reach + 2, melee + 3), sim 8 turns,
  death = 450 + carried gems × 10.
- Banking: gems ≥ min(50, max(8, 1.5 × altar distance)) (× 0.6 per tile 15 rounds after a sighting); always at ≤ 3.
- Heal: < 30% hp (< 50% within 15 rounds of a sighting) or < 2.5 × incoming camp damage + 40; walk home instead of
  recalling after a hit from an unseen attacker.
- Scoring: gold weight 0.2 / 0.1 / 0.03 (rounds < 600 / < 1200 / later), enemy-side groups × 0.6, sighting area
  (7 tiles, 20 rounds) × 0.3, +25% per ally target within 10 tiles, current target × 1.25.
- Switches left off (measured worse): kSquads (1 or 2 squads), kHelp, kCornered, kAvoidAwake, kTheirSide 0.35.
- Fuel: first turn only builds the map model (~175k of 200k); group fields filled while ≥ 60k fuel remains.

## Changes this generation
- New bot (immigrant, archetype camp-sweep). Evaluation sets: A = 00eight 00gem-fields 00quadrants 00road-of-riches
  00split 01corridors 01gold-rush 01islands 01shortcut 01the-farm; B = 00ambush 00collision 00contest 00corners
  00expanse 00interlock 00lanes 00packs 00rows 01elk-arena (both sides each, 20 games per set).
- Final vs f09-01: A 4/20 (−315), B 0/20 (−559). vs f00-03 (A) 9/20 (−2); vs f04-05 (A) 6/20 (−138).
  vs starter: wins at round 843 (00eight), 642 (00quadrants), 1302 (01big-world). 0 faults, 0 `bot error:`.
- Milestones vs f09-01 on set A: first version 0/20 (−857); fight model + moving retreat 3/20 (−368);
  altar-lock breaking (we never banked on several maps: f09-01 locks altars) and bank cap: 4/20 (−315).
- Game budget: ~625 games played.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel facts: a full-map distance_field compute is ~27k fuel on 100×100, camp_groups() ~32k, a bitboard op ~500,
  a field get ~60. A turn that runs out of fuel is suspended mid-function and resumes next round: fuel_remaining()
  read after that point reports the NEW turn's budget, so profiles look fine while the engine log shows 100000.
  Check the .fuel file in the replay (u64 per turn from byte 13) instead. A per-tile std::vector init on 100×100 cost
  ~40k fuel.
- Danger tiles (foe reach) OR'd into the same mask retreat used: every retreat step was blocked and champions stood
  still until dead (most deaths in the first versions). Keep a separate physical mask for retreat.
- Fight model that let us hit foes 8 tiles away (no arrival check on targets) engaged lost 1v1s.
- Squads (kSquads 2: −540, 1: −583 on A+B vs −437 solo): packs farm too slowly and still lose fights.
- Answering allies' sightings within 12 (kHelp): −640 vs −437 (A+B).
- Avoiding awake camps of other groups in pathing: −644 vs −315 (A).
- Fight back when cornered by an adjacent melee chaser: −453 vs −315 (A).
- Retreat pad 4 instead of 2: −514 vs −437 (A+B). Engage threshold −150: −469. Heal at 70% after sightings: −438 (equal).
- Enemy-side groups × 0.35, contested × 0.8: −460 vs −315 (A).

## Known weaknesses
- Deaths: 15–80 per game vs f09-01 (they die 2–15). Two thirds happen while retreating from packs; full-hp
  champions die within 6 turns to focus fire; melee chasers with dashes cannot be outrun.
- No boss raids: monolith/golem groups (100–228 gems) are left to the enemy; 00contest has no other gem source.
- Respawn waits take 11–23% of turns on cluster maps (00quadrants); farming there is ~30% slower than f09-01.
- No wards: hidden shooters in bushes are only avoided (walk home), never revealed.
