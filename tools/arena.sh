#!/bin/bash
# usage: tools/arena.sh BOT [OPP ...]  -> wins and deaths (ours/theirs) on every arena map against each opponent
cd "$(dirname "$0")/.." || exit 1
export CAMBC_NO_UPDATE_CHECK=1
bot=$1; shift; opps=${@:-"v12-2 v12-1"}
S=/tmp/claude-0/-home-user-cambridge-battlecode/2fab6dd9-1ffb-57c6-a8fa-b7b28170c277/scratchpad/bench/arena_$$
tw=0; tn=0; td=0; to=0; line="$bot:"
for opp in $opps; do
  python3 tools/bench2.py "$bot" "$opp" --map-dir arenas --jobs 4 --keep "$S/$opp" > "$S.$opp.log" 2>&1
  w=$(grep -c "WIN" "$S.$opp.log"); n=$(grep -cE "WIN|loss" "$S.$opp.log")
  r=$(python3 tools/teamstats.py "$S/$opp" | tail -1 | python3 -c "import sys,re; l=sys.stdin.read(); m=re.findall(r\"'deaths': (\d+)\", l); print(m[0], m[1])")
  d=${r% *}; o=${r#* }; tw=$((tw+w)); tn=$((tn+n)); td=$((td+d)); to=$((to+o)); line="$line [$opp w$w/$n d$d/$o]"
done
echo "$line  WINS=$tw/$tn DEATHS=$td/$to"
rm -rf "${S:?}" "$S".*.log
