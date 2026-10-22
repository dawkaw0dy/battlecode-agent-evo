#!/bin/bash
# usage: tools/quick.sh BASE OUTFILE "k=v[,k=v]" ...  -> wins vs v12-2 and v12-1 (34 games each) plus unopposed mean rounds
cd "$(dirname "$0")/.." || exit 1
export CAMBC_NO_UPDATE_CHECK=1
base=$1; outfile=$2; shift 2
opps=${OPPS:-"v12-2 v12-1"}
for spec in "$@"; do
  tmp=bots/qk_$(echo "$spec" | tr -c 'A-Za-z0-9' '_' | cut -c1-40)_$$
  rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
  if [ "$spec" != "-" ]; then IFS=',' read -ra kvs <<< "$spec"; for kv in "${kvs[@]}"; do k=${kv%%=*}; v=${kv#*=}; sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"; done; fi
  if cambc build "$tmp" --release >/dev/null 2>&1; then
    line="$base $spec:"; tot=0
    for opp in $opps; do
      r=$(python3 tools/bench2.py "$(basename "$tmp")" $opp --jobs 4 2>&1 | tail -1 | sed -E 's/.*: ([0-9]+)\/34 wins, mean margin ([^,]+),.*/\1 \2/')
      w=${r%% *}; tot=$((tot+w)); line="$line [$opp $r]"
    done
    u=$(python3 tools/bench2.py "$(basename "$tmp")" starter-cpp --jobs 4 2>&1 | awk '/WIN|loss/{n++; match($0, /r *[0-9]+ +faults/); s+=substr($0, RSTART+1, RLENGTH-8)+0} END{printf "%.0f", s/(n?n:1)}')
    echo "$line  WINS=$tot unopposed=$u" | tee -a "$outfile"
  else echo "$spec : BUILD FAILED" | tee -a "$outfile"; fi
  rm -rf "$tmp"
done
