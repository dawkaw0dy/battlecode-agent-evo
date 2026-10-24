## Strategy (≤10 lines)
Map reader with squads. The draft (10x fuel) measures every camp group (gems, gold, health, strength, walking
distance from both spawns) and writes the plan to draft memory: picks, plan kind (farm / boss), squad size and
the squad of each body, and an angular sector (owner squad) per group. Squad size = value-weighted median of the
champions a group needs so nobody tanks more than ~600 health (min 2; one squad on maps ≤45x45; pairs on maps
≥55x55). Each squad's leader (lowest living slot) scores groups by value/(travel + wait + clear + capture + trips)
and writes its target to shared memory; members follow, or farm a cheap neighbouring group ("loose squad").
Dangerous sleeping groups are only woken once enough squad mates are close. A boss (the camp boxing in the only
altar, the draft's boss, else the nearest monolith) is raided by the whole team once our stats say we outlast it.
Fights: hit what is in reach (focus the weakest, kill shots first), step in only on foes 1-2 steps away or on a
foe attacking a friend, retreat home when low or outnumbered; Willow heals; gems are banked early.

## Key parameters
- Picks: Vendetta, Willow, Varg, Marcella, Vesper Vale (boss plan: Vendetta, Marcella, Varg, Ryth, Vesper).
- Need per group = ceil(sqrt(str·0.7·hp / (70·600))); planning assumes the drafted squad (allies out of sight
  cannot be inspected): team dps = dps·(1 + 0.85·(n−1)), team hp = hp + 0.8·max·(n−1).
- Score = (gems + w·gold)·f / (d + wait + clear + 5 + trips·(d_home + 10) + 6), w = max(0.08, 0.25·(1 − r/1600)),
  f: own sector 1, shared 0.8, other squad 0.6, enemy side ×0.85; keep the current target unless 1.4× better.
  Damage estimate counts at most ~6 attackers of a big group; ≤10 trips.
- Gather before a sleeping dangerous group: wait outside its range up to 12 turns (raids: team, 25 turns).
- Fight: ratio = Σhp·power within 7 (allies remembered 2 rounds); retreat if low (hp<30% or <2.5 turns of
  threat) and ratio<2, else if ratio<0.8 (0.6 while committed); kill shot = hp ≤ 0.9·burst always taken.
- Tribute: ≥32 gems, ≥14 within 8 tiles, ≥6 within 3, ≥8 with a foe in sight, endgame, or the winning bag.
- Build: jungle(str) → monster(str) → power(hp) → power(str) → durability(hp) → alternating power shards.
- Turn 1 only initialises (static fields); leftover fuel computes group fields; costly paths guarded by fuel.

## Changes this generation
- New bot (immigrant, map-reader archetype) on the starter's main loop.
- Local results (final versions): vs starter 12/12; vs f02-01 19/80 on all 40 maps mid-generation, later subsets
  5/16 and 6/16; vs f00-03 2–3/12, vs f01-07 2/12 on 6-map subsets. 0 faults, 0 bot errors.

## Tried and failed (keep entries from the parent's notes; add yours)
- Fuel: iostream output costs ~10k fuel per line; per-turn traces pushed turns over 100k and every other round was
  lost. Trace only inside debug_block (free in debug builds) and test traces with `cambc run` (debug).
- Local loops cost ~60 fuel per iteration (bitboard set() per tile ~40): building approach tiles per tile cost
  60–70k; whole-board shifts (grow) cost a few thousand.
- A heap overrun (a per-camp byte array sized before the last group was added) corrupted other vectors and
  trapped in later rounds (botFault + fuelExhausted after the restart). Check sizes when adding per-camp arrays.
- Ally state is only readable in vision: squad strength from visible allies flickered and targets flip-flopped
  every turn; plan with the drafted squad size instead and remember allies for 2 rounds.
- Static fields treat camp spawn tiles as walls; standing on a dead camp's tile read "unreachable" (target flips).
- Solo farming (squad size 1): lone champions get picked off; Cassi & Issac: Cassi dies constantly (linked death).
- Blob of all 5 everywhere: melee crowd the camps, everyone waits through captures.
- Chasing a kiting enemy for 10+ turns leads into its team; approach only 1–2 steps (6 when it is on a friend).
- Raiding only the camp that boxes in the altar (01boss-battle) instead of its whole group: worse (the group's
  400-gem capture is worth more); raiding earlier: worse.
- Roster Vesper before Varg on 3–4 champion maps: mixed, kept Varg.

## Known weaknesses
- Early skirmishes on small contested maps (starfish, gemfight, collision, packs, swirl) snowball against us.
- Raids on boss maps are slow (champions rotate home to heal); 01boss-battle as team A banks nothing.
- No wards, altar locks or teleport crystals; no shop trips (gold waits for the next spawn visit).
- Results are chaotic: a one-rule change flips several games both ways; judge on many games.
