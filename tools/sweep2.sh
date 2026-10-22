#!/bin/bash
# usage: tools/sweep2.sh BASE OPP OUTFILE "name=value[,name=value]" ...
cd "$(dirname "$0")/.." || exit 1
export CAMBC_NO_UPDATE_CHECK=1
base=$1; opp=$2; outfile=$3; shift 3
for spec in "$@"; do
  tmp=bots/sw_$(echo "$spec" | tr -c 'A-Za-z0-9' '_' | cut -c1-40)_$$
  rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
  IFS=',' read -ra kvs <<< "$spec"
  for kv in "${kvs[@]}"; do k=${kv%%=*}; v=${kv#*=}; sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"; done
  if cambc build "$tmp" --release >/dev/null 2>&1; then
    res=$(python3 tools/bench2.py "$(basename "$tmp")" "$opp" --jobs 4 2>&1 | tail -1 | sed 's/TOTAL [^:]*: //')
  else res="BUILD FAILED"; fi
  echo "$spec : $res" | tee -a "$outfile"
  rm -rf "$tmp"
done
