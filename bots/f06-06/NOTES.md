## Strategy (≤10 lines)
Safe banker (immigrant; only the starter's main loop and draft skeleton kept). Draft Vendetta, Marcella, Vesper Vale,
Varg, Tavi. Every champion farms alone: groups scored by (gems + 0.35·gold)·share / (walk + respawn wait + clear time
+ extra trips + 0.3·bank distance); camps never heal, so big groups are worn down over up to 4 trips, and claims in
shared memory keep one champion per group (one more per 3000 camp hp). Gems are banked early (walk when the altar is
within 10 + 1.5·gems, at once after a recent enemy sighting), recalls only when hurt or carrying gems, full heal on
spawn. Every spawn visit buys a 10-gold altar lock; our locks are placed on any free altar within 2 tiles and block
only enemy tributes. Shards: jungle(str) → durability(hp) → home(str) → power str/hp 2:1 (Tavi shops anywhere).
Fights use a square law (Σhp·Σdps, allies within 6): fight at 1.2×, hit back when a melee foe has us and no dash is
ready, otherwise run home/towards allies with a dash first; below 35% health only finish kills.

## Key parameters
- kTrip 0.55 (max health one trip may cost), kMaxTrips 4, kHelperHp 3000, kReplan 12 rounds, current target ×1.5.
- kBankBase 10, kBankPerGem 1.5, threat banking: sighting within 30 and ≥4 gems; always at ≥40 gems.
- kFight 1.2, kTrapped 0.25, kHurtFight 0.35, kHurtRecall 0.45, flee-recall only below 60% hp or with ≥12 gems.
- Disengage from camps when hp < incoming·mult·(exit steps + 1) + 10% max hp.
- Shop trip at 200 gold (not Tavi); heal on spawn to 90%. Sightings (x, y, round) in shared bytes 5–7, 20 rounds.

## Changes this generation
- New bot. v1 parties (leader picks, followers copy) lost 0/80 to f05-02: parties walk too much and lose fights.
- v2 solo farming with multi-trip clears, remembered camp health, camp-aware routes (unseen camps of groups we believe
  alive are walls; a group closing the way to the altar becomes a target).
- Fuel: target choice cost ~60k on 80-camp maps (item_count per group, per-call sort); cached per turn and camps
  presorted at init → no capped turns after round 2. A capped turn had leaked a `bot error:` (engine error 5).
- Altar locks: vs f00-03 −310 → −165 (10 maps), vs f05-02 −888 → −738.
- Final, all 40 maps both sides: vs f03-05 7/80 (−562), 0 faults, 0 caught exceptions (two visibility guards added
  after it; 8-game smoke test: no `warn:` lines left); 10-map subsets: f00-03 9–10/20,
  f02-01 7/20, f03-05 5/20, f04-05 2/20 (other 10 maps), f05-02 0–1/20; starter 19/20 (rounds-to-win 1192).

## Tried and failed (keep entries from the parent's notes; add yours)
- Parties of 2–3 or one 5-champion deathball: 0/20 vs f05-02, farming ~5× slower than the panel.
- Waiting on a respawning group while standing on its spawn tiles: an occupied tile blocks the respawn forever.
- Remembering "reward available" without expiry: champions crossed the map to rewards captured long ago (8 rounds now).
- Re-choosing targets every 3 rounds (×1.3 hysteresis): constant flip-flops between far groups; 12 rounds ×1.5 now.
- Treating "stronger" as a reason to spend the turn: champions stood still or chased; act only when a hit lands.
- Fleeing a melee chaser on foot without hitting back (died after 10 turns of free hits); fleeing away from home.
- Lock detours (walk ≤8 tiles to lock an altar): vs f05-02 −833 vs −738, vs f02-01 −542 vs −292 (more deaths).
- Help calls (healthy allies within 14 walk to a fleeing ally): −846 vs −763 vs f05-02, less farming, same deaths.
- Buddy pairs (slots 0+1, 2+3 share a target): vs f04-05 −674 vs −533, no fewer deaths.
- Recall at 55% instead of 45%: −656 vs −533 vs f04-05. Tavi instead of Ryth: neutral (kept for no shop trips).
- Single 20-game runs swing by ±150 gems on identical logic: only large effects are measurable.

## Known weaknesses
- The lineage bots (f01-07 … f05-02) hunt our lone farmers in our own half (75% of our deaths): 20–50 deaths per game,
  most carried gems stolen, and their kill gold snowballs into items. No group defence that works yet.
- Contested centres (01elk-arena, 01hooks, 00gemfight, 00eight) are lost against every champion bot.
- No wards, no teleport crystals; dashes used only to escape; spell use is greedy per turn.
- Our own enemy-lock handling is basic: we break an enemy lock only when standing next to it.
