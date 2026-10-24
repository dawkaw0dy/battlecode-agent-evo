## Strategy (≤10 lines)
Split farm with the f01-03 fight module (unchanged structure): the draft splits our side's camp groups into one
angular sector per champion; each champion farms alone, scores groups by value / (travel + respawn wait + clear
time), wears strong groups down over several trips, tributes at the nearest altar and recalls to heal/shop.
Fights: force ratio (Σhp·Σpower, allies within 6 vs enemies within 8 steps) decides engage / hold / flee, with
threat-aware flee steps and a trade rule when a kill is reachable before dying.
This generation (focus: spells): single-target camp spells now hit the same camp as the basic attack (finish
kills, then the hardest hitter), and shields go up as soon as anything can hit us.

## Key parameters
- Picks unchanged: Vendetta, Marcella, Vesper Vale, Tavi, Varg (dash maps: Vendetta, Vesper, HoR, Ren, Marcella, Tavi).
- Camp spell target (kSpellFocusCamp): score = strength·10 − hp/100, +10000 if the spell's damage_amount() kills it.
  Area spells still aim where they hit the most camps.
- Shield (kShieldEarly): on camps whenever an awake camp can reach us (was: also hp < 95%); in fights whenever an
  enemy champion can hit us next turn (threatened_) or hp < 90%.
- Fight/farm parameters as in f01-03 (engage ≥1.3, flee <0.85 or hp <30%, recall/shop at 250 gold).

## Changes this generation
- Kept A, kSpellFocusCamp: Marcella's S1 / Tavi's stun on camps focus the basic-attack target instead of the
  healthiest camp. On 20 lost maps (10 vs f01-07, 10 vs f00-03, both sides): wins 4 -> 13 of 40.
- Kept B, kShieldEarly (on top of A): same 40 games 13 -> 18 wins, no new losses.
- A+B on all 40 maps, both sides (unbiased check): vs f01-07 49/80 (parent 42/80, margin +2561),
  vs f00-03 55/80 (parent 44/80, margin +8068), 0 bot errors / faults. Head-to-head vs the parent f01-03:
  33W/45L on 78 valid games (mirror matches are chaotic; 2 games on 01islands had a caught exception, see below).
- Reverted C, kStunFlee (ranged stunners stun the nearest chaser and shoot instead of stepping away): vs B
  wins 18 -> 16 on the 40-game set (margin +2471, flips both ways).
- Reverted N, kShieldFlee (shield before running away): vs B wins 18 -> 16, margin −1045.
- Untested: kStunChannel (spells prefer enemies channeling tribute/recall) — left off.
- All switches are constexpr flags at the top of bot.cpp.

## Tried and failed (keep entries from the parent's notes; add yours)
- Linear bitboard buffers are bit-packed (not one byte per tile as the API doc says).
- Every bitboard/field query is a host call (~100+ fuel); per-tile loops (add_box with set(), is_set/get in
  loops, camp_at for every visible camp) blew the 100k budget on maps with 30-camp groups. Use exported
  local copies and whole-board shifts.
- Turns that run out of fuel resume next round on stale state: moves into newly occupied tiles and attack
  resets throw. Guard every action with "still the same round".
- Recalling to shop at 150 gold between fights slowed farming; 250 gold with the "not engaged" check is fine.
- Requiring no incoming camp damage before tributing stalled champions next to altars; tributes are not
  interrupted by damage.
- Diagonal dashes can be refused by the engine ("unreachable within movement range"): try candidates in order;
  only straight one-tile holes are treated as jumpable when planning.
- Mixing a static field and a unit-aware BFS made champions flip-flop between two tiles; after a blocked static
  step use the dynamic route for 12 rounds, and clear a camp that plugs a gap instead of detouring.
- Tavi standing on spawn did not heal for us; never wait on spawn more than two turns.
- Altar tiles occupied by camps (boss beside the only altar) must not be planned as tribute spots.
- Raising the tribute threshold (25 gems within 8) was neutral; kept the lower one for safety.
- Region weight for another champion's sector: 0.6 was clearly slower (+5% rounds on 8 maps), 0.3 slightly
  slower; 0.45 kept — the split itself pays.
- Trading blows whenever fleeing cannot avoid hits (hp ≥ 12%): 0/24 on f00-03's best maps; the kill-reachable
  condition gave 10/24 and +6200 gems vs f00-00 on 16 maps.
- Also trading when home is farther than turns-to-die ("doomed"): worse vs both parents (fleeing champions
  often survive because the chaser gives up).
- Counting enemies seen in the last 4 rounds (out of sight) in the force ratio: worse vs both parents (flees too much).
- Keeping up a retreat out of sight at full health, together with the memory above: -3500 gems vs f00-00.
- Answering ally reports of up to 3 enemies within 15 tiles: +1700 vs f00-03 but -4500 vs f00-00 on the test
  sets; kept 2 / 12.
- No-progress rule (target alive, no move/attack for 25 rounds → skip it 150 rounds) did not fix
  01yellow-brick-road side B vs starter (reverted, untested elsewhere).
- Stunning the chaser instead of stepping away when fleeing (ranged with a stun, kStunFlee): −2 wins on 40 games.
- Raising Varg's shield while fleeing (kShieldFlee): −2 wins, −1045 gems on 40 games.
- Test sets built only from maps the parent lost overstate any change (regression to the mean); confirm on all maps.

## Known weaknesses
- 01islands: a caught exception (`bot error:`) in the mirror match vs f01-03 (both our bot as A and the parent as A
  hit it), not seen vs f01-07/f00-03. Not investigated (no time); counts as a loss when it happens.
- Head-to-head vs the parent f01-03 was 33/78 despite gains vs f01-07 and f00-03.
- 01boss-battle, 01gold-rush, 00pilgrimage, 01concentrated-wealth, 01shortcut still lost on both sides vs the panel.
- Still a split farm: lone champions lose to packs; no wards, altar locks or healing support.
