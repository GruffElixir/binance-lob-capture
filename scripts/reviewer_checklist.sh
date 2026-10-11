#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
git clone --quiet --local "$repo" "$scratch/review"
cd "$scratch/review"
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-3}
cmake -B build && cmake --build build
./build/binance_capture --venue spot --symbols BTCUSDT --duration "${BLC_CHECKLIST_SECONDS:-300}" > capture.log 2>&1
test "$(awk -F',' 'NR==2{print NF}' *_orderbook.csv)" = 26
pattern='api'"_key|sec"'ret|pass'"word"
if git grep -i "$pattern"; then echo 'literal checklist grep returned a match' >&2; exit 1; fi
scripts/check_credentials.sh
BLC_VERIFY=./build/blc_verify scripts/verify_outputs.sh --epoch-log capture.log ./*.csv
scripts/check_live_log.sh capture.log spot .
echo 'CHECKLIST PASSED'
