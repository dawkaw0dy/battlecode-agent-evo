#!/bin/bash
# usage: tools/arena_variants.sh BASE OUTFILE "k=v[,k=v]" ...  -> arena score per parameter variant
cd "$(dirname "$0")/.." || exit 1
export CAMBC_NO_UPDATE_CHECK=1
base=$1; outfile=$2; shift 2
for spec in "$@"; do
  tmp=bots/ar_$(echo "$spec" | tr -c 'A-Za-z0-9' '_' | cut -c1-40)_$$
  rm -rf "$tmp"; mkdir -p "$tmp"; cp -r "bots/$base/src" "$tmp/src"
  IFS=',' read -ra kvs <<< "$spec"; for kv in "${kvs[@]}"; do k=${kv%%=*}; v=${kv#*=}; sed -i -E "s/(${k} = )[^;]+;/\1${v};/" "$tmp/src/params.h"; done
  if cambc build "$tmp" --release >/dev/null 2>&1; then tools/arena.sh "$(basename "$tmp")" | sed "s/^[^:]*:/$spec:/" | tee -a "$outfile"; else echo "$spec: BUILD FAILED" | tee -a "$outfile"; fi
  rm -rf "$tmp"
done
