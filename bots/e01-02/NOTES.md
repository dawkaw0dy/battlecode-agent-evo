## Strategy (≤10 lines)
Camp sweep (immigrant from starter infrastructure). Draft rates every camp group by gems per unit of clear work
and writes one efficiency byte per group to draft memory ('S', count, bytes); picks strong basic attackers
(Vendetta, Vesper Vale, Varg first when >= 3 multi-fighter groups, then Ryth, Marcella, Tavi, Willow, ...).
Champions score groups by (gems + 0.05*gold) / (distance + clear time + 5), scaled by the draft rating, skip
respawning groups and monoliths, skip groups whose camp damage, shared with allies on it, exceeds 80% hp.
Each champion publishes its target in shared memory (byte = champion type % 8); small groups claimed by an ally
are discounted x0.4 so farmers spread instead of splitting rewards. Gems are banked in small sticky batches
(>= 8 near an altar, >= 30 anywhere, low hp, enemies seen, late game). AoE spells aim at the tile covering most
camps; attack resets follow basic attacks. Jungle emblem first, then power shards. Recall < 33% hp.

## Key parameters
bank: gems >= 30, or >= 8 with altar within 7, or altar within 2; kDangerShare 0.8 (per-champion share of
clear damage vs current hp; helpers = max(allies within 5, claimants)); kGoldWeight 0.05; recall 33%; heal on spawn to 90%;
claimed small group x0.4, big (>2000 hp / golem) x1.1; target hysteresis x1.3; stuck 8 turns -> skip 40 rounds.

## Changes this generation
(e01-02, parameter tweaks on camp choice; each measured vs e00-01 on 00rows, 01elk-arena, 00packs, both sides)
- KEPT: danger filter now divides clear damage (and clear time) by the champions sharing the group
  (max of allies within 5 and allies claiming it in shared memory), threshold 0.5 -> 0.8 of hp. The old filter
  rejected every 3-wolf group and golem on 00rows (champions idled, 0 gems). 0/6 -> 2/6, margin sum -2396 -> -1929.
  Validation: 6/6 vs parent e00-03 (00rows, 01elk-arena, 01big-world), 4/6 vs starter (loses 00rows).

## Tried and failed (keep entries from the parent's notes; add yours)
- vision::camps()/champions() can contain units whose reads throw (engine errors 97/98): check is_visible()
  first, and wrap each decision block in a local guard so a stale handle does not abort the turn.
- Casting "excludes walking" spells after a move throws: casts are skipped once we walked this turn.
- e01-02 (all vs e00-01 on 00rows/01elk-arena/00packs, baseline after kept change: 2/6, -1929):
  gold weight 0.05 -> 0.25 in group value: 0/6, -2942. Claimed-big-group bonus 1.1 -> 1.6 (gather): 0/6, -3051.
  Danger share 0.5: 0/6, -3544. Danger share 1.2: 1/6, -2816. Target hysteresis 1.3 -> 1.6: 0/6, -3142.

## Known weaknesses
- vs e00-01 our champions die ~10 times each on 01elk-arena/00packs (enemy Vesper Vale 20+ kills): losses
  are decided by champion fights and the enemy's gold lead, not camp choice. 00rows still lost vs starter.
- Lost 00lanes badly (-773/-515) and 01boss-battle 0-0 (tiebreak) vs r00-01; not investigated for lack of time.
- No retreat logic vs aggressive enemies beyond the 33% recall; spread-out farmers can be picked off.
- Shared-memory slot by champion type % 8 can collide (harmless: only mis-weights claims).
- Self-eval vs r00-01 on 4 maps: 2/8 wins; vs r00-00 earlier version won most valid games.
