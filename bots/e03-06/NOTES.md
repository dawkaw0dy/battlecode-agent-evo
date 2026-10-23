## Strategy (≤10 lines)
Parent e02-05 (scaling economy, Lanchester combat) with the camp subsystem rewritten. Each champion scores every
camp group by its own share of the reward per round: (gems x (3 + round/1000) + gold) / allies sharing, over
travel + respawn wait + clear time + 5 capture rounds. Clear time and damage taken use the group's focus-fire
"exposure" (sum of strength x health killed before each member, highest strength/health first) with the
claimants' combined DPS. Groups that would push us under 35% HP need team help (x0.45, assumes the team) or are a
last resort (x0.03). Per-champion claims in shared memory spread cheap groups and gather claimants on hard ones.
Capture handoff: an ally already on a ready group's spawn tile keeps it, others leave (no split). Respawning
groups are waited for next to (not on) their spawn tiles. Kill order: awake, own group, strength/health, lowest HP.

## Key parameters
Gem weight 3 + round/1000, Vesper gold x1.33, safe health 35%, capture 5 + 4 overhead rounds, team assumption >= 3,
stickiness x1.25, enemy within 3 of group x0.6, wait horizon 120 rounds. DPS vs camps = Strength + 0.25 Focus.
Claims: shared-memory bytes 2..7, one per champion type slot (group + 1). Combat/economy parameters unchanged.

## Changes this generation
Camp subsystem rewritten (GroupInfo exposure, choose_group, farm_camps/walk_to_group, camp part of pick_target);
tiles_around is now bounds-checked. Sleeping camps are only woken when their group was judged clearable.
Results (local, before the final error guard): vs e00-01 14/16 (parent 8/15 on the same 8 maps), vs e02-05 10/16
(+179 mean gems), vs e02-01 9/16. Road-of-riches, quadrants and lost-world turned from losses into wins.

## Tried and failed (keep entries from the parent's notes; add yours)
- Starter step selection `field.get(tile) < field.get(step)` picks blocked/unreached tiles (they read 0): "Known occupied
  destination". Fix: only step to tiles that are free and in `visited`.
- Iterating `vision::champions()/camps()` handles and calling health()/is_alive() throws "No visible entity" when the
  unit died this turn; scan `champion_tiles()/camp_tiles()` and use `champion_at/camp_at` instead.
- Attack-reset abilities throw "An available attack cannot be reset" in cases `can_attack()` does not reveal; resets disabled.
- After a move returns false (hidden unit), moving again the same turn throws: stop walking for the turn.
- Hard danger filter (skip groups) left champions idle on 01big-world/00lanes; softened to a x0.05 score factor (small gain).
- (e02-05) Reading champion_at(tile).health() right after our attack killed the unit throws engine error 97 even when
  has_champion(tile) is true: check the handle's is_visible() first and guard each combat block locally.
- (e03-06) Engine error 5 ("Invalid metadata, alignment, overlap or argument") once per game on 00lanes B vs e02-01,
  in the walk after choosing a group; bounds-checked tiles_around and guarded single moves did not remove it, so the
  whole walk (walk_to_group) is guarded. Root cause not found.

## Known weaknesses
- 00eight and 00gem-fields vs e02-01 lost both sides (-547, ~-300); 00gem-fields vs parent 0/2.
- 00lanes vs e02-01 lost both sides after the final guard; not investigated (out of time).
- Claim slots are keyed by champion type (collisions possible for unusual drafts); dead champions keep stale claims.
