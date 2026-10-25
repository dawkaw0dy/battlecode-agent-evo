## Strategy (≤10 lines)
Defense-first economy from the parent (f03-05): altar locks, deny, tribute under fire, square-law fights, draft
Vesper Vale, Vendetta, Marcella, Tavi, Varg with one home region per champion. This generation rewrote the camp
subsystem (camps.cpp, same calls: build_threats / choose / farm / estimate_damage / hit_group):
- A clear is simulated kill by kill (most strength removed per hit first, clusters last). A champion only fights down
  to the health the hurt rule in Bot::turn needs to walk out of reach (strength in reach × (exit steps + 2) + 40), so a
  trip's useful health is what lies above that reserve. In a team only the last champion in reach pays it; the others
  hand off to a nearer ally (2 turns of fire). Groups whose trips buy < 3 turns of fighting are not farmable by that team.
- Plans: solo (non-boss), shared with a claimer, or the smallest raid team that succeeds; rate = value / team per turn.
- Engage gate before waking a group: the champions actually near must carry it (no trips) or the planned team must be
  there; otherwise wait on the rally ring (≤ 30 rounds), then go if ≤ 1 trip, else block the group. Heal first alone
  when our health now costs an extra trip. Groups the enemy reaches first are discounted (less for raids).

## Key parameters
- Reserve = max(0.2·max hp, hit·(exit + 2) + 40); hand-off reserve = max(0.2·max hp, 2·hit + 40); exit = range + 1 − ours.
- Trips: solo ≤ 3 (4 with ≥ 3 champions); raids ≤ 3 refills, charged at half a trip each (the team rotates).
- Trip time = 8 + walk from spawn + exit; capture 6; gather 8 when the raid needs champions not yet claimed.
- Territory (our walk − theirs): ≤ −3: 1.0, ≤ 3: 0.9, ≤ 10: 0.7, else 0.5; square root for teams of 2+.
- Hit order: kill-shots on fighters, then strength/health; clusters only when no fighter is in range;
  Varg adds splash on neighbours. The last camp is finished from one of the group's own spawn tiles.
- kPairs (slots 0-1 and 2-3 farm the leader's claim together) is in the code, switched off.

## Changes this generation
bench2.py, 40 maps × both sides; parent figures are its recorded results (reproduced exactly locally).
- v1 rewrite: vs f02-01 64/80 (+483; parent 69/80, +498): camp deaths 190 → 124, camp damage −15%, same gems.
- v3 (+ team hand-off reserve; v1 never raided the double monolith on 00rows): vs f02-01 67/80 (+531),
  vs f01-07 72/80 (+621; parent 67, +548), vs f00-03 71/80 (+664; parent 69, +498), vs the parent 43/80 (+17).
  Gems collected by round 600 vs f02-01 254 (parent 245), our champion deaths 10/game (parent 12).
- vs f03-04 (not in the panel) on 10 maps where the parent lost 19/20: 9/20.
- v4 (+ last camp from a spawn tile): vs f02-01 65/80 (+450), vs starter 20/20, 20449 rounds to win on 10 maps × 2
  (parent 21009). 0 faults and 0 `bot error:` in all games; 3 caught warnings in 180 games, 2 now guarded.

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
- Lock detours of 14 tiles + idle champions running locks to far altars (≤40): 57/80 vs 65/80 at 8 tiles.
- me.attack(altar_lock_at(a)) throws "Target must be a visible enemy or neutral camp" (the parent's lock code
  never worked); a tribute at the locked altar is what hits the lock.
- Hunting any enemy carrying ≥40 gems (prey even when far): 65/80 (+430), same as without; not kept.
- Pair farming (melee + ranged share the leader's target, plan as a unit of two): vs f02-01 66/80 with fewer gems
  (221 vs 246 by round 600) and no extra kills; vs f03-04 8/20 vs 9/20 alone, deaths 19 vs 14 per game. The pair
  rarely stands together (banking, recalls), so it only splits income.
- Charging every raider the full walk-out reserve: no team could ever raid a double monolith (00rows lost late to
  raiders on both sides). Only the last champion in reach needs it.
- Win counts swing ±5/80 between versions that change nothing systematic (17 map flips each way); compare
  aggregates (gems by round 600, deaths by cause, gold) from the stats files instead.

## Known weaknesses
- Outcomes are still decided by PvP: f03-04's fight simulation beats our square law (00gemfight, 01hooks: 40-60
  deaths vs 9). Contested centre groups (00packs, 00gemfight) are lost to early fights, not to camp choice.
- The team model assumes allies as strong as us; raids still wait up to 30 rounds on the rally ring.
- Single elks are slow alone (the reserve leaves ~10 turns per trip); nothing plans spell damage from range 4.
- 00eight, 01elk-arena and 01concentrated-wealth remain coin flips against the lineage bots.
