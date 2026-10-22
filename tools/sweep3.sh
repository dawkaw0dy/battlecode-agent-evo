#!/bin/bash
# usage: tools/sweep3.sh BASE OUTFILE "name=value[,name=value]" ...  -> mean rounds to beat starter-cpp (34 games)
cd "$(dirname "$0")/.." || exit 1
export CAMBC_NO_UPDATE_CHECK=1
base=$1; outfile=$2; shift 2
for spec in "$@"; do
  tmp=bots/sw_$(echo "$spec" | tr -c 'A-Za-z0-9' '_' | cut -c1-40)_$$
  rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
  IFS=',' read -ra kvs <<< "$spec"
  for kv in "${kvs[@]}"; do k=${kv%%=*}; v=${kv#*=}; sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"; done
  if cambc build "$tmp" --release >/dev/null 2>&1; then
    res=$(python3 tools/bench2.py "$(basename "$tmp")" starter-cpp --jobs 4 2>&1 | awk '/WIN|loss/{split($0,a,"r "); r=a[2]+0; if ($3=="WIN") {n++; s+=r} else l++} END {printf "wins=%d losses=%d mean_rounds=%.1f", n, l, s/(n?n:1)}')
  else res="BUILD FAILED"; fi
  echo "$spec : $res" | tee -a "$outfile"
  rm -rf "$tmp"
done
