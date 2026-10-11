#!/usr/bin/env bash
set -euo pipefail
release=${BLC_RELEASE_DIR:-build/release}
asan=${BLC_ASAN_DIR:-build/asan}
root=${BLC_LIVE_OUTPUT:-live_output}
seconds=${BLC_LIVE_SECONDS:-60}
export BLC_VERIFY="$release/blc_verify"
check() {
    local dir=$1 venue=$2 log=$3
    scripts/verify_outputs.sh --epoch-log "$log" "$dir"/*.csv
    scripts/check_live_log.sh "$log" "$venue" "$dir"
}
replay() {
    local dir=$1
    "$release/binance_capture" --replay "$dir"/market_data_*.csv --output-dir "$dir/replay"
    for f in "$dir"/*_orderbook.csv; do cmp "$f" "$dir/replay/$(basename "$f")"; done
}
for venue in spot usdm; do
    a="$root/$venue-asan"
    r="$root/$venue-release"
    mkdir -p "$root"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      setarch "$(uname -m)" -R "$asan/binance_capture" --venue "$venue" --symbols BTCUSDT --stats-interval 0 --output-dir "$a" > "$a.log" 2>&1 & pid=$!
    trap 'kill -INT "$pid" 2>/dev/null || true' EXIT
    sleep "$seconds"
    kill -INT "$pid"
    wait "$pid"
    trap - EXIT
    if grep -E 'LeakSanitizer|ERROR: AddressSanitizer|runtime error:' "$a.log"; then exit 1; fi
    # ASan's memory overhead is excluded from the Release RSS gate.
    scripts/verify_outputs.sh --epoch-log "$a.log" "$a"/*.csv
    BLC_SKIP_RSS=1 scripts/check_live_log.sh "$a.log" "$venue" "$a"
    "$release/binance_capture" --venue "$venue" --symbols BTCUSDT --duration "$seconds" --stats-interval 0 --output-dir "$r" > "$r.log" 2>&1
    check "$r" "$venue" "$r.log"
    replay "$r"
    printf 'LIVE SMOKE PASSED %s\n' "$venue"
done
multi="$root/multi-spot"
"$release/binance_capture" --venue spot --symbols BTCUSDT,ETHUSDT --symbols-per-connection 1 --duration "$seconds" --stats-interval 0 --output-dir "$multi" > "$multi.log" 2>&1
check "$multi" spot "$multi.log"
replay "$multi"
printf 'LIVE SMOKE PASSED multi-symbol\n'
