#!/bin/bash
# usage: tools/variant_params.sh BASE NAME name=value ...
# Copies bots/BASE/src to bots/NAME/src and overrides constants in params.h.
cd "$(dirname "$0")/.." || exit 1
base=$1; n=$2; shift 2
rm -rf "bots/$n"; mkdir -p "bots/$n"; cp -r "bots/$base/src" "bots/$n/src"
for kv in "$@"; do
  k=${kv%%=*}; v=${kv#*=}
  grep -q "constexpr [a-z]* ${k} = " "bots/$n/src/params.h" || echo "MISSING $k"
  sed -i -E "s/(constexpr [a-z]+ ${k} = )[^;]+;/\1${v};/" "bots/$n/src/params.h"
done
