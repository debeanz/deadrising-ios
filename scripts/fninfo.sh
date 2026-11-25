#!/bin/bash
# Usage: fninfo.sh ADDR [lines]  -> size, callees (imports named) and device-field accesses.
# Device fields = loads/stores relative to r3 or to registers copied from r3 (mr rN,r3).
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A=$(echo "$1" | tr 'a-f' 'A-F' | sed 's/^0X//;s/^SUB_//')
END=$(awk -v t="$A" '($1 "") > (t "") { print $1; exit }' "$ROOT/analysis/funcs.txt")
awk -v s="$A" -v e="$END" '($1 "") >= (s "") && ($1 "") < (e "")' "$ROOT/analysis/text.dis" > /tmp/fn.dis
echo "== sub_$A  size 0x$(printf %X $((0x$END - 0x$A)))"
calls=$(grep -oE 'bl +0x[0-9a-f]+' /tmp/fn.dis | awk '{print toupper(substr($2,3))}' | sort | uniq -c)
echo -n "calls: "
echo "$calls" | while read n t; do [ -z "$t" ] && continue; name=$(awk -v t="$t" '$1==t {print $2}' "$ROOT/analysis/import_thunks.txt"); printf "%s(x%s) " "${name:-$t}" "$n"; done; echo
bases=$(grep -oE 'mr +r[0-9]+,r3$' /tmp/fn.dis | sed -E 's/mr +(r[0-9]+),r3/\1/' | sort -u | tr '\n' '|'); bases="r3|${bases}"; bases=${bases%|}
echo "device regs: $bases"
echo "fields: $(grep -oE "(lwz|stw|lhz|sth|lbz|stb|lfs|stfs|ld|std|lwzx|stwx) +[rf][0-9]+,-?[0-9]+\(($bases)\)" /tmp/fn.dis | awk -F'[ ,(]+' '{printf "%s@0x%X\n", $1, $3}' | sort -t@ -k2 -u | tr '\n' ' ' | cut -c1-600)"
[ -n "$2" ] && head -n "$2" /tmp/fn.dis
exit 0
