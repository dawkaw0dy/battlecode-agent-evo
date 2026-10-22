#!/bin/bash
# usage: tools/panel.sh BASE OUTFILE "k=v[,k=v]" ...  -> wins per opponent (34 games each) and total
# Opponents: $OPPS (default "v12-2 v12-1 v11 v10"); use OPPS="v12-2 v12-1 v12 v11 v10 v09 v07" for the 7-opponent panel.
cd "$(dirname "$0")/.." || exit 1
export CAMBC_NO_UPDATE_CHECK=1
opps=${OPPS:-"v12-2 v12-1 v11 v10"}
base=$1; outfile=$2; shift 2
for spec in "$@"; do
  tmp=bots/pn_$(echo "$spec" | tr -c 'A-Za-z0-9' '_' | cut -c1-40)_$$
  rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
  if [ "$spec" != "-" ]; then IFS=',' read -ra kvs <<< "$spec"; for kv in "${kvs[@]}"; do k=${kv%%=*}; v=${kv#*=}; sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"; done; fi
  if cambc build "$tmp" --release >/dev/null 2>&1; then
    tot=0; n=0; line="$base $spec:"
    for opp in $opps; do
      r=$(python3 tools/bench2.py "$(basename "$tmp")" $opp --jobs 4 2>&1 | tail -1 | sed -E 's/.*: ([0-9]+)\/34 wins, mean margin ([^,]+),.*/\1 \2/')
      w=${r%% *}; tot=$((tot+w)); n=$((n+34)); line="$line [$opp $r]"
    done
    echo "$line  TOTAL=$tot/$n" | tee -a "$outfile"
  else echo "$spec : BUILD FAILED" | tee -a "$outfile"; fi
  rm -rf "$tmp"
done
