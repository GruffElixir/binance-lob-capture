# Measurements

Measured on 2026-10-11 on an Intel Core i7-13700HX (24 logical CPUs), Ubuntu 22.04 under WSL2,
GCC 12.3, C++17, Release (`-O3`), Boost 1.74 and OpenSSL 3.0.2. These are local measurements,
not throughput guarantees. Live captures and build activity were present during the initial
microbenchmarks. The fixtures and workloads are committed, so reviewers can rerun them:

```bash
cmake -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBLC_BUILD_BENCH=ON
cmake --build build/bench --target blc_bench --parallel
./build/bench/blc_bench book
./build/bench/blc_bench payload
./build/bench/blc_bench decimal
./build/bench/blc_bench ring
./build/bench/blc_bench replay samples/market_data_spot_BTCUSDT_2026-10-11.csv
```

## Book mutations

Each workload has ten repetitions of one million mutations on one bid side. Near-touch prices
cycle over the best five; deep prices cycle over the worst five. Update workloads overwrite
existing quantities. Delete workloads alternate erase/reinsert, so the book stays near its
initial size. Construction is excluded. Results are ns/mutation. **p99 here is the maximum of
ten repetition averages**, using the nearest-rank method; it is not a per-mutation tail latency.

| Levels | Workload | Flat median | Flat p99 repetition | Map median | Map p99 repetition |
|---|---|---:|---:|---:|---:|
| 1,000 | Touch update | 4.50 | 5.18 | 12.58 | 14.20 |
| 1,000 | Touch delete/reinsert | 6.64 | 6.81 | 25.98 | 29.18 |
| 1,000 | Deep update | 14.43 | 16.04 | 6.99 | 7.40 |
| 1,000 | Deep delete/reinsert | 123.75 | 131.38 | 18.68 | 20.50 |
| 5,000 | Touch update | 4.77 | 5.15 | 15.21 | 16.09 |
| 5,000 | Touch delete/reinsert | 7.07 | 7.45 | 31.13 | 34.63 |
| 5,000 | Deep update | 15.66 | 16.50 | 7.74 | 7.99 |
| 5,000 | Deep delete/reinsert | 915.31 | 960.28 | 21.90 | 22.99 |

The flat representation favors near-touch work and contiguous top-five extraction. Deep
insertions/deletions move O(n) bytes, and `std::map` wins decisively there. Existing quantity
updates only search/overwrite; they don't move levels. This trade-off is explicit rather than
a claim of O(updates) worst-case behavior.

## Payload and network validation

One million iterations per fixture/venue, with reusable parser/event storage. Typed decoding
validates fields and produces events; the separate network pass validates complete JSON and
splits the combined-stream envelope. These costs are additive in capture. MB/s uses decimal
megabytes and the actual bytes processed by the respective pass.

| Fixture | Typed ns/message | Typed MB/s | Network ns/message | Network MB/s |
|---|---:|---:|---:|---:|
| Spot diff | 613 | 576 | 743 | 529 |
| Spot depth5 | 539 | 681 | 685 | 594 |
| Spot trade | 223 | 600 | 385 | 452 |
| Spot snapshot | 553 | 663 | 722 | 564 |
| USD-M diff | 2,194 | 554 | 2,838 | 443 |
| USD-M depth5 | 630 | 590 | 944 | 436 |
| USD-M trade | 227 | 591 | 408 | 427 |
| USD-M snapshot | 482 | 614 | 641 | 524 |

## Decimal, ring and replay

The decimal workload cycles eight exact fixed-point strings for one million calls, ten
repetitions. The two-thread ring workload transfers one million records per repetition,
checks payload boundaries and sequence order, and spins/yields when full or empty; it
measures copy/transfer throughput, not idle wake-up latency.

| Workload | Median ns/op | p99 repetition ns/op | Records/s | Payload MB/s |
|---|---:|---:|---:|---:|
| Decimal parse | 16.38 | 21.61 | — | — |
| Ring, 256-byte payload | 39.41 | 42.36 | 25,374,600 | 6,496 |
| Ring, 4,096-byte payload | 217.21 | 224.54 | 4,603,850 | 18,857 |

Replay uses the committed 120-second sample: 7,107 market records, its REST sidecar,
and real CSV output to a new temporary directory. Total throughput includes input/header
reading, processor construction, sidecar injection, decoding, book updates, output and final
flush/close. A library-only observer times each market record from CSV reading through its
attached snapshots and processing. Its clock/callback overhead is included; final trailing
sidecar records and flush are included in throughput but outside the per-record percentile.
Percentiles use nearest rank on individual observations, not repetition averages.

| Repetition | Records/s | p50 ns | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|
| 1 | 74,973 | 1,842 | 36,942 | 138,868 |
| 2 | 84,014 | 1,803 | 34,466 | 130,718 |
| 3 | 85,510 | 1,779 | 35,331 | 128,091 |
| 4 | 82,765 | 1,715 | 33,696 | 122,043 |
| 5 | 71,633 | 1,665 | 30,633 | 122,147 |
| 6 | 78,043 | 1,665 | 32,909 | 124,108 |
| 7 | 85,106 | 1,697 | 32,985 | 128,582 |
| 8 | 86,054 | 1,699 | 31,078 | 123,695 |
| 9 | 83,325 | 1,746 | 30,848 | 133,629 |
| 10 | 93,198 | 1,749 | 35,335 | 128,672 |

These filesystem timings were collected on WSL's mounted Windows drive while sanitizer
builds and live captures ran. They are reproducible workload measurements, not a latency SLA.

## Live verification before coverage refresh

These runs used the real Release capture, public BTCUSDT endpoints, Resync mismatch policy,
and no fault injection. They ran concurrently; each has separate files and independent
process-wide limits. Every set passed `blc_verify --epoch-log`, positive bid/ask and comparison
gates, and byte-identical replay. All frame accounting reconciled; no malformed frames, drops,
gaps, parse errors or depth5 mismatches occurred. The one-symbol budget is 84 MiB.

| Venue | Seconds | Market rows | Book rows | Syncs | Resyncs | Coverage exhaustion | Full depth5 checks | Partial depth5 checks | Peak RSS KiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Spot | 600 | 25,192 | 25,185 | 1 | 0 | 0 | 6,029 | 0 | 30,552 |
| USD-M | 600 | 26,351 | 26,303 | 2 | 1 | 1 | 257 | 5,235 | 30,816 |
| Spot | 1,800 | 81,532 | 76,457 | 2 | 1 | 1 | 17,939 | 0 | 30,724 |

Actual compared levels: 60,290 (600 s spot); 2,570 full + 39,114 partial (USD-M);
179,390 (1,800 s spot). The 30-minute run's suppressed interval is the finite-snapshot coverage
case: output resumes only after authoritative resync. Its live/replay match includes that pause.

`scripts/live_smoke.sh` also passed 60-second ASan/UBSan/LSan signal-ended captures and Release
captures on both venues, plus a 60-second spot BTCUSDT/ETHUSDT run on two connections. All
outputs verified; Release and multi-symbol outputs replayed identically. Full sanitizer test
suite: ASan/UBSan/LSan 287 tests and TSan 288 tests passed before adding coverage refresh;
the final sanitizer gate is rerun after subsequent changes.

## Live verification with coverage refresh

These runs used the refresh-enabled Release binary, with the same gates and mismatch policy.
Each three-file set verified against epoch accounting and replayed byte-for-byte. All runs
had zero malformed frames, dropped frames, gaps, parse errors and depth5 mismatches.

| Venue | Seconds | Market rows | Book rows | Syncs | Resyncs | Coverage exhaustion | Refresh swaps | Full depth5 checks | Partial depth5 checks | Peak RSS KiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Spot sample | 120 | 7,107 | 7,098 | 1 | 0 | 0 | 0 | 1,185 | 0 | 30,684 |
| USD-M | 600 | 23,634 | 23,623 | 1 | 0 | 0 | 0 | 62 | 5,405 | 30,940 |
| Spot | 1,800 | 61,064 | 61,055 | 1 | 0 | 0 | 0 | 18,120 | 0 | 30,692 |

Actual compared levels were 620 full + 41,626 partial for USD-M and 181,200 full for the
30-minute Spot run. The one-symbol budget was 84 MiB. Price movement did not reach the refresh
threshold during these runs, so the zero swap count is not evidence of refresh execution.
Independent property, hand-derived and allocation tests exercise equal/different swaps,
delayed bridging, stale snapshots and exhaustion with a pending refresh.

On 2026-10-11, the GitHub-hosted availability probe returned HTTP 451 for both public REST
endpoints ([workflow run](https://github.com/GruffElixir/binance-lob-capture/actions/runs/38162922191)).
Live checks remain a local release gate; real TLS loopback tests provide deterministic CI coverage.
