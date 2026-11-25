#!/bin/bash
# Usage: containing.sh ADDR... -> prints the recompiled function that contains each address.
F="$(cd "$(dirname "$0")/.." && pwd)/analysis/funcs.txt"
for a in "$@"; do
  a=$(echo "$a" | tr 'a-f' 'A-F' | sed 's/^0X//')
  awk -v t="$a" '($1 "") <= (t "") { best=$2 } END { print t, "in", best }' "$F"
done
