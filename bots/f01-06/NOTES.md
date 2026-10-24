## Strategy (≤10 lines)
Camp sweep. The draft analyses every camp group (gems, gold, monster health/strength, walking distance from
both spawns), cuts the groups on our side into one equal-value angular sector per champion and writes picks +
group→slot owners into draft memory. Each champion sweeps a loop over the map: every 3 rounds it scores all groups
by value / (travel + respawn wait + clear time + 10), preferring its own sector, wears down groups too strong for
one trip over several trips (camps never heal), and announces its target in shared memory so team-mates leave
light groups to it (heavy groups get the helpers' damage in the estimate). Gems are banked in small batches
whenever the altar detour is short (1–10 gems take one tribute tick), or when idle, in danger, or carrying a lot.
Enemies: fight when our nearby strength beats theirs by 10%, otherwise run home and recall; reports in shared
memory pull nearby healthy team-mates in and push farmers away from enemy packs.

## Key parameters
- Picks: Vendetta, Marcella, Vesper Vale, Tavi, Varg (dash maps, <60% of value walkable: Vendetta, Vesper Vale,
  Heart of Ruin, Ren, Marcella).
- Group value gems + 0.3·gold; factor 1.0 own sector, 0.6 unassigned, 0.45 other sector, ×0.6 enemy side,
  ×1.3 reward waiting, ×1.3 current target, ×0.5 light group another champion already targets, ×0.3 near an
  enemy pack. Multi-trip if ≥4 turns of fighting per trip and ≤8 trips (trip cost 10 + distance from spawn).
- Bank: ≥70 gems, idle with ≥8, ≤2 tiles from an altar, detour ≤ 1 + gems/8, ≥40 gems within 12, ≥10 after an
  enemy report, or end of game. Tribute under camp fire only if we survive ceil(gems/10)+1 turns of it.
- Recall: hp < 30% (after leaving camp range) or hp < incoming·(exit steps + 2) + 40; hp < 45% before a fight;
  shop trip at 220 gold; wait ≤3 turns on spawn below 92% hp.
- Items: jungle emblem (strength; health for Ryth/Marcella) → monster(str) → power(str/hp) → alternating power.

## Changes this generation
- New bot (immigrant, camp-sweep archetype) on the starter's main loop. Final local results: vs f00-03 28/80
  (all 40 maps, both sides, mean margin −161); vs starter 32/32 (16 maps). No faults or bot errors.

## Tried and failed (keep entries from the parent's notes; add yours)
- Squads (a trio and a pair, or one trio) following a shared squad target: 15/80 vs f00-03. Champions spent ~40% of
  turns walking and 13% walking to altars; squads share travel without any gain. Per-champion sectors: 19→28/80.
- Waiting at the edge of a heavy group for an announced helper (≤12–25 turns) and a ×1.25 bonus for joining heavy
  groups: removing both took a 20-game set from 2/20 to 7/20 wins.
- Penalty for light groups another champion targets: ×0.5 kept; ×0.3 and ×0.75 both clearly worse on 20 games.
- Banking stricter (≥50, detour ≤ gems/10) and more eager (≥60, detour ≤ 2 + gems/6): neither better.
- Respawn wait counted 1.5× in the target score: worse. Recall at 25%/40% instead of 30%/45%: 23/80 vs 28/80.
  Heal-before-fight only when the fight looks dangerous: 24/80 vs 28/80. Never re-picking the current target
  while it would need >4 rounds of respawn wait: 22/80.
- Picks Vendetta, Ryth, Willow, Vesper Vale, Marcella (tank + healer for group fights): fewer gems than the
  f00-03 picks (11.5k vs 13.2k on 20 games).
- 01boss-battle: the only altar's free sides are all camp spawn tiles; excluding camp tiles from altar tiles left
  none, so 2,700 collected gems were never banked. Use camp tiles when an altar has no other free side.
- Keeping camp-group bitboards in a std::vector<cambc::bitboard> caused engine error 5 ("Invalid metadata,
  alignment, overlap") on 01yellow-brick-road; rebuild boards from tile lists when needed.
- Boss groups (monolith/golem) allowed through the multi-trip check: roughly neutral, kept (00contest gems come
  only from the monolith).

## Known weaknesses
- Farming rate is still ~80% of f00-03 on quiet maps (00expanse, 01big-world, 01the-farm): ~14% of turns waiting
  at respawning groups and ~8% walking to altars.
- 01hooks and 01corridors (contested elk in the middle) lose many fights; 01boss-battle banks little (one altar
  tile). A caught engine error is logged as "dbg err" and the turn is skipped (none seen in the final tests).
