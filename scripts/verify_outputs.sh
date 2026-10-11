#!/usr/bin/env bash
# Checks binance_capture output files: exact headers, 26 order-book columns, then blc_verify.
#
#   scripts/verify_outputs.sh [--epoch-log <stderr-log>] <file>...
#
# BLC_VERIFY overrides the checker binary (default: ./build/blc_verify).
set -euo pipefail

verify_bin="${BLC_VERIFY:-./build/blc_verify}"
verify_args=()
files=()
while [ $# -gt 0 ]; do
    case "$1" in
        --epoch-log)
            verify_args+=(--epoch-log "$2")
            shift 2
            ;;
        *)
            files+=("$1")
            shift
            ;;
    esac
done
if [ ${#files[@]} -eq 0 ]; then
    echo "usage: $0 [--epoch-log <stderr-log>] <file>..." >&2
    exit 2
fi

md_header='recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json'
ob_header='tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4'
sc_header='recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_json'

status=0
for f in "${files[@]}"; do
    name="$(basename "$f")"
    case "$name" in
        market_data_*_orderbook.csv) want="$ob_header" ;;
        market_data_*.csv) want="$md_header" ;;
        depth_snapshots_*.csv) want="$sc_header" ;;
        *)
            echo "$f: not an output file name" >&2
            status=1
            continue
            ;;
    esac
    got="$(head -n 1 "$f" | tr -d '\r')"
    if [ "$got" != "$want" ]; then
        echo "$f: unexpected header" >&2
        status=1
    fi
    if [ "$want" = "$ob_header" ] && [ "$(wc -l < "$f")" -gt 1 ]; then
        columns="$(awk -F',' 'NR==2{print NF}' "$f")"
        if [ "$columns" != "26" ]; then
            echo "$f: first row has $columns columns, expected 26" >&2
            status=1
        fi
    fi
done

if ! "$verify_bin" "${verify_args[@]}" "${files[@]}"; then
    status=1
fi
exit "$status"
