# binance-lob-capture

C++17 capture of Binance public spot and USD-M combined streams, with an audit CSV, REST
snapshot sidecar and deterministic top-five order-book CSV per symbol. Subscriptions are
`depth@100ms`, `depth5@100ms`, and `trade`. Replay uses the same processor and makes **no network
calls**. [Design](docs/design.md), [requirements](docs/requirements.md), and
[benchmarks and live results](docs/benchmarks.md) explain the implementation and its evidence.

## Toolchain and build

Target: Ubuntu 22.04, GCC 12.3, C++17; also checked with Clang 15. Dependencies: CMake ≥3.22,
Boost ≥1.74, OpenSSL (tested with 3.0.2), zlib and the system CA bundle. simdjson is vendored
(see `third_party/simdjson/VERSION.txt`). The default test build downloads GoogleTest through
CMake; `-DBLC_BUILD_TESTS=OFF` builds the application without that download.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-12 -DBLC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
cmake --build build --target tests
./build/tests/unit_tests
```

Release is also the default for `cmake -B build && cmake --build build`. Presets `debug`,
`release`, `asan` (ASan/UBSan) and `tsan` are available. CI checks both compilers, formatting,
clang-tidy, sanitizers, Valgrind replay, CSV contracts and absence of sockets in replay.
Run Linux permission tests as a regular user.

## Capture and files

One invocation writes all three files for each symbol:

```bash
./build/binance_capture --venue spot --symbols BTCUSDT --output-dir ./output --duration 120
./build/binance_capture --venue usdm --symbols BTCUSDT,ETHUSDT --output-dir ./output --duration 120
```

`--symbols` is a comma-separated list of 2–20 ASCII letters/digits, case-insensitive, with
surrounding whitespace allowed; duplicates are rejected. CSV symbols are uppercase, stream
names lowercase. Venue defaults to spot, output directory to `.`, duration to unlimited.
Duration includes validation and is measured from process start. Ctrl-C/SIGTERM requests an
orderly shutdown; a second signal forces network shutdown while committed input still drains.
Exit codes: 0 normal, 1 runtime/I/O failure, 2 invalid options or invalid symbol.

Other options: `--symbols-per-connection 50`, `--trade-rows on|off` (default on),
`--stats-interval 10` (0: final summary only), `--fault-reconnect-every N` (0: disabled), and
`--help`. Files are `market_data_<venue>_<SYMBOL>_<UTC-start-date>.csv`, the same stem with
`_orderbook.csv`, and `depth_snapshots_<venue>_<SYMBOL>_<UTC-start-date>.csv`. A run keeps one
set through midnight/reconnects. Existing files move together into `replaced_<UTC-stamp>[-k]/`.
The sidecar records shard/epoch and `apply_before_row`; keep it beside its market file for replay.

## CSV, scaling and timestamps

UTF-8, RFC 4180 escaping, LF endings, exact headers. Market rows store the inner `data` JSON,
minified to one physical line, in consumer processing order:

```
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
```

Receive time is `system_clock` when the complete socket message or REST response arrives.
Seconds/nanoseconds split it exactly (`0 ≤ tnsec < 10^9`); wall-clock adjustments may make time
decrease. Steady time measures duration/latency. Book rows use the causing record's timestamp:
an `R` uses the snapshot if it completes sync, or the later bridging diff. Initial buffered
updates emit one final `R`, rather than invented historical receive times.

The order-book header has exactly 26 columns:

```
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
```

| Field | Meaning |
|---|---|
| `seqNo` | Per-symbol emitted-row counter, starts at 0 and continues across resyncs |
| `id` | FNV-1a-32 of `venue:SYMBOL`, masked to 31 bits as int32; collisions rejected |
| `type` | `R` synchronized/reset book or snapshot refresh; `D` diff; `P` depth5; `T` trade |
| `side` | `B` bids only, `S` asks only, `N` both/neither; trade side is the resting maker's side |
| price/size | Exact int64 decimal ×100,000,000; excess nonzero precision/overflow rejected |
| levels | Best first; absent price/size is 0 |

CSV integers use no binary floating point. `conn_seq` starts at 0 per shard/epoch, counts
complete received frames, and may have gaps for losses. The separate book `seqNo` has no gaps.

## Semantics and recovery

REST depth (`limit=1000`) establishes the book; diffs set absolute quantities and zero removes
a price. Spot bridges `L+1`, then permits no missing IDs. USD-M bridges `U ≤ L ≤ u`, then
requires `pu` matching the preceding `u`. Stale diffs don't mutate the book. Successful
reconnects increment the shard epoch; each symbol resets on its own first new-epoch record.
Gaps, malformed diffs and exhausted coverage suppress book rows until authoritative sync.

`depth5` describes the exchange top five at its update ID. It cross-checks the diff book and
never patches/replaces it. Spot often permits full checks; a futures ID inside a diff range
permits checks of untouched levels only. Mismatches trigger REST resync. Trades don't mutate
the book; optional `T` rows repeat its top five, with `m` determining the maker's resting side.

Finite snapshots cannot prove unseen levels. Each side tracks a completeness boundary;
fewer than five known levels inside it forces resync. Below 200 known levels, coverage refresh
synchronizes a preallocated shadow book before swapping. A snapshot swap emits `R`; a swap
completed by a later diff emits `D`. Equal/different tops are counted. Large depletion bursts
can still require suppression/resync.

## Threading, I/O and performance

One Asio thread owns sockets/TLS/timers, envelope validation and weighted REST dispatch. One
consumer owns parsing/books/files. A bounded SPSC byte ring transfers padded payloads for
in-place parsing; atomic request slots carry snapshot requests in the reverse direction.
Books aren't shared. The ring's busy path has no contended lock; its idle consumer parks.
The consumer writes 64 KiB buffered market/book output and immediately flushes shared snapshot
scratch. A writer thread would add another queue/shutdown protocol; disk back-pressure fills
the ring instead, and overflow invalidates/reconnects that shard. Input closes after I/O exits;
the consumer drains and checks final flush/close. Write failures exit 1.

All processor storage is reserved at startup. A real-file test requires zero C++ heap
allocations from the first processed record, including resync/refresh. Networking, logger
formatting, startup/shutdown and replay's CSV reader are outside that claim. JSON is validated
on the network thread and decoded once on the consumer; benchmarks measure both passes.

The flat book costs O(log k) search plus O(k) moves for insertion/deletion, k being the number
of better levels; it isn't worst-case O(updates). At 5,000 levels, measured near-touch
update/delete cost was approximately 5/7 ns versus 15/31 ns for `std::map`, while deep deletes
cost 915 ns versus 22 ns. Existing quantity updates don't shift storage. Full workloads,
repetition percentiles and machine details are in [benchmarks](docs/benchmarks.md).

## Replay, metrics and limits

```bash
./build/binance_capture --replay samples/market_data_*.csv --output-dir ./replay_output
./build/blc_verify samples/*.csv replay_output/*.csv
cmp samples/*_orderbook.csv replay_output/*_orderbook.csv
```

Replay identifies/skips book files included by the glob. Each market file works independently
with its adjacent sidecar; malformed input fails with a line diagnostic and removes partial
output. Replay makes no network calls (`strace` CI gate). `blc_verify --epoch-log capture.log
output/*.csv` also reconciles frame accounting per epoch. Replay equality proves consistency;
hand-derived fixtures and independent full-book oracles separately check correctness.

Periodic `stats` and final `summary` report frames/rows, parsing/drops, gaps/resyncs, coverage,
refresh, depth5 attempts and levels compared, REST weight/failures/cooldowns, reconnects,
retained storage and latency percentiles. `epoch_closed` reports epoch accounting; `totals`
reports Linux peak RSS (`VmHWM`, KiB) and budget (MiB). A zero mismatch count needs positive
comparison counts to be meaningful.

Limits: 256 symbols, 64 shards, 8,192-byte targets, 1 MiB WebSocket/REST bodies, 20,000 diff
levels, 1,000 snapshot levels per side, JSON nesting 128, recent window 2,000 diffs/20,000 levels,
and 5,000 stored levels per side (trim to 4,000). Ring size is 16/32/64 MiB at ≤64/≤128/>128
symbols. Budgets including network transients and a 48 MiB allowance are 84 MiB (1 symbol/1
shard), 429 MiB (256/64), and 353 MiB (256/6). Attempts are capped at 200 per sliding five
minutes process-wide. Reconnects use jittered bounded backoff; REST uses half the weight budget
and pauses globally on 429/418. TLS always verifies the system CA chain and hostname.

## Public configuration and samples

No credentials are needed. `.env.example` has comments only; the program reads CLI options
and no environment configuration. Ordinary CSV/build artifacts are ignored; curated samples
and test fixtures are tracked. The brief's literal `git grep -i "api_key|secret|password"`
uses basic regex (unescaped `|` is literal). The intended extended-regex version also matches
documentation words. CI checks the extended interpretation outside documentation/ignore
files and ensures the example has no values.

`samples/` contains all three files from a real two-minute BTCUSDT run, replayed byte-for-byte
in CI. `scripts/live_smoke.sh` checks real traffic, sanitizers, frame accounting, schema and
replay, including two shards. Binance regional availability makes live checks unsuitable as
a deterministic CI gate; loopback and scenario tests supply that gate.

First three data rows from the committed run (payload fields shortened for readability;
full payloads are in the linked CSVs):

[Market data](samples/market_data_spot_BTCUSDT_2026-10-11.csv):

```csv
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
1791741732,438450732,spot,depth5,0,0,0,BTCUSDT,"{""lastUpdateId"":101285817767,""bids"":[[""83811.50000000"",""6.52743000""],[""83811.49000000"",""0.00012000""],[""83811.48000000"",""0.00088000 ... [shortened]"
1791741732,438639235,spot,depth_diff,0,0,1,BTCUSDT,"{""e"":""depthUpdate"",""E"":1791741732614,""s"":""BTCUSDT"",""U"":101285817755,""u"":101285817767,""b"":[[""83811.50000000"",""6.52743000""],[""83810. ... [shortened]"
1791741732,539342141,spot,depth5,0,0,2,BTCUSDT,"{""lastUpdateId"":101285817807,""bids"":[[""83811.50000000"",""6.52743000""],[""83811.49000000"",""0.00012000""],[""83811.48000000"",""0.00088000 ... [shortened]"
```

[Order book](samples/market_data_spot_BTCUSDT_2026-10-11_orderbook.csv):

```csv
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
1791741732,882018360,0,2014473836,R,N,8381150000000,8381149000000,8381148000000,8381101000000,8381100000000,652717000,12000,88000,238000,400000,8381151000000,8381152000000,8381153000000,8381200000000,8381201000000,43207000,47000,12000,14000,2600000
1791741732,882022396,1,2014473836,T,S,8381150000000,8381149000000,8381148000000,8381101000000,8381100000000,652717000,12000,88000,238000,400000,8381151000000,8381152000000,8381153000000,8381200000000,8381201000000,43207000,47000,12000,14000,2600000
1791741732,921554563,2,2014473836,P,N,8381150000000,8381149000000,8381148000000,8381101000000,8381100000000,652717000,12000,88000,238000,400000,8381151000000,8381152000000,8381153000000,8381200000000,8381201000000,43207000,47000,12000,14000,2600000
```

[Snapshots](samples/depth_snapshots_spot_BTCUSDT_2026-10-11.csv) (the run contains one snapshot row):

```csv
recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_json
1791741732,751688159,spot,0,0,BTCUSDT,5,"{""lastUpdateId"":101285817819,""bids"":[[""83811.50000000"",""6.52519000""],[""83811.49000000"",""0.00012000""],[""83811.48000000"",""0.00088000 ... [shortened]"
```


The appendix preserves the assignment's headings/commands with the documented adaptations:
GitHub username, real sample date, added sidecar, CA package and escaped Markdown table pipes.
Its live commands run until Ctrl-C; add `--duration 120` for a timed two-minute capture.

## How to Submit on GitHub

### 1. Initialize and push your repository

```bash
# From your project root
git init
git add .
git commit -m "Initial submission: Binance WebSocket capture + LOB"

# Create a new repo on GitHub (via web UI or CLI), then:
git remote add origin https://github.com/GruffElixir/binance-lob-capture.git
git branch -M main
git push -u origin main
```

### 2. Make sure .gitignore excludes build artifacts and secrets

```gitignore
# .gitignore
build/
*.csv
*.o
*.a
*.so
.env
config/secrets.json
```

Never commit API keys, private keys, or credentials. All config should use environment variables or a .env.example
template (no real values).

### 3. Tag your submission

```bash
git tag -a v1.0.0 -m "Submission v1.0.0"
git push origin v1.0.0
```

## Full Build & Run Commands (Reproducible)

### Prerequisites

```bash
# Ubuntu 22.04 / Debian — install all dependencies
sudo apt-get update
sudo apt-get install -y \
  cmake ninja-build \
  g++-12 \
  libssl-dev \
  libboost-all-dev \
  zlib1g-dev \
  ca-certificates
```

Compiler: GCC 12 (g++-12), Standard: C++17. Also tested with Clang 15.

### Clone and build

```bash
git clone https://github.com/GruffElixir/binance-lob-capture.git
cd binance-lob-capture
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -O2"
cmake --build build --parallel
```

### Run — Live capture (both CSVs)

```bash
# Single symbol, spot venue, 2-minute capture
./build/binance_capture \
  --venue spot \
  --symbols BTCUSDT \
  --output-dir ./output
# Multi-symbol
./build/binance_capture \
  --venue spot \
  --symbols BTCUSDT,ETHUSDT \
  --output-dir ./output
# USD-M futures
./build/binance_capture \
  --venue usdm \
  --symbols BTCUSDT \
  --output-dir ./output
```

Outputs written to ./output/:

```
output/
  market_data_spot_BTCUSDT_2026-10-11.csv
  market_data_spot_BTCUSDT_2026-10-11_orderbook.csv
  depth_snapshots_spot_BTCUSDT_2026-10-11.csv
```

### Run — Verify CSV schema

```bash
# Check header and row count
head -2 ./output/market_data_spot_BTCUSDT_2026-10-11.csv
head -2 ./output/market_data_spot_BTCUSDT_2026-10-11_orderbook.csv
# Confirm orderbook has exactly 26 data columns (25 commas per data row)
awk -F',' 'NR==2{print NF}' ./output/market_data_spot_BTCUSDT_2026-10-11_orderbook.csv
# Expected output: 26
```

### Run tests (if applicable)

```bash
cmake --build build --target tests
./build/tests/unit_tests
```

### Reviewer Checklist

| Step | Command |
|---|---|
| Build from clean | `cmake -B build && cmake --build build` |
| Live 300s capture | `./build/binance_capture --venue spot --symbols BTCUSDT --duration 300` |
| Column count check | `awk -F',' 'NR==2{print NF}' *_orderbook.csv` → 26 |
| No secrets in repo | `git grep -i "api_key\|secret\|password"` → empty |
