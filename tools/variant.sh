#!/bin/bash
# usage: tools/variant.sh BASE OPPS name=value ...   (all overrides applied together)
cd "$(dirname "$0")/.." || exit 1
base=$1; opps=$2; shift 2
tmp=bots/var_$$; rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
for kv in "$@"; do k=${kv%%=*}; v=${kv#*=}; sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"; done
./cambc build "$tmp" >/dev/null 2>&1
out="$*:"
for o in ${opps//,/ }; do out="$out [$o] $(python3 tools/bench.py "$(basename "$tmp")" "$o" --quiet | tail -1 | sed "s/TOTAL //; s/ wins, mean gem margin//; s/, mean round.*//")"; done
echo "$out"; rm -rf "$tmp"
