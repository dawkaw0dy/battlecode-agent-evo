#!/bin/bash
# usage: tools/sweep.sh BASE OPP[,OPP2] name=value [name=value ...]
# Copies bots/BASE to a temp bot with params.h overrides, builds, benches vs OPP.
cd "$(dirname "$0")/.." || exit 1
base=$1; opp=$2; shift 2
for kv in "$@"; do
  k=${kv%%=*}; v=${kv#*=}
  tmp=bots/sw_$$_${k}_${v//./_}
  rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
  sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"
  ./cambc build "$tmp" >/dev/null 2>&1
  out="$k=$v:"
  for o in ${opp//,/ }; do out="$out [$o] $(python3 tools/bench.py "$(basename "$tmp")" "$o" --quiet | tail -1 | sed "s/TOTAL //; s/ wins, mean gem margin//; s/, mean round.*//")"; done
  echo "$out"
  rm -rf "$tmp"
done
