#!/usr/bin/env bash
# Non-vacuous live correctness and memory gates; a successful process alone is insufficient.
set -euo pipefail
log=$1
venue=$2
dir=$3
summary=$(grep ' info summary ' "$log" | tail -1)
totals=$(grep ' info totals ' "$log" | tail -1)
metric() { printf '%s\n' "$summary $totals" | awk -v key="$1" '{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==key){print a[2];exit}}}'; }
positive() { local n; n=$(metric "$1"); test -n "$n" && test "$n" -gt 0; }
positive syncs
positive ob_rows
test "$(metric depth5_mismatch)" = 0
if [ "$venue" = spot ]; then
    positive depth5_verified
    positive depth5_full_levels_compared
else
    positive depth5_partially_verified
    positive depth5_partial_levels_compared
fi
if [ "${BLC_SKIP_RSS:-0}" != 1 ]; then
    test "$(metric VmHWM)" -le "$(( $(metric budget_mib) * 1024 ))"
fi
awk -F, 'FNR>1 {n++; if($7<=0 || $17<=0) exit 1} END {if(n==0) exit 1}' "$dir"/*BTCUSDT*_orderbook.csv
printf 'LIVE GATES PASSED %s\n' "$venue"
