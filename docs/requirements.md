# Requirements traceability

Every requirement from the assignment brief, with the evidence that proves it is met.
`Status` moves from **Planned** → **Done** only when the evidence exists (test, CI job, or
README section).

Release gate for the `v1.0.0` tag:

- **Functional, Submission and Review focus** rows are mandatory: all must be **Done**.
- **Optional stretch** rows must be **Done** or marked **Deferred** with a reason.
- Enhancements chosen in the design (marked "(enhancement)" in [design.md](design.md)) are not
  submission gates.

## Exact contracts

Order-book CSV header (single line, 26 columns):

```
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
```

Market-data CSV header (9 columns, fixed order):

```
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
```

Subscriptions per symbol (lowercase in the URL): `<symbol>@depth@100ms`, `<symbol>@depth5@100ms`,
`<symbol>@trade`.

| Venue | Combined-stream base URL |
|---|---|
| `spot` | `wss://stream.binance.com:9443/stream?streams=` |
| `usdm` | `wss://fstream.binance.com/public/stream?streams=` |

## Functional

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| F1 | Live capture from Binance combined streams: `depth@100ms`, `depth5@100ms`, `trade` per configured symbol | URL-builder unit test; live sample run | Planned |
| F2 | Venues `spot` and `usdm` with the base URLs above | URL-builder unit test; live run per venue | Planned |
| F3 | Market-data CSV: one row per message applied from the socket, storing the inner `data` payload (envelope removed) | Envelope unit tests; frame-accounting invariant per (shard, epoch): `frames_received = md_rows + malformed_frames + ring_drops`, with every `conn_seq` either in the CSV or counted as a loss (design §8) | Planned |
| F4 | File naming `market_data_<venue>_<SYMBOL>_<UTC-date>.csv`; choice documented | Writer unit test; README | Planned |
| F5 | CSV: UTF-8, RFC 4180 quoting (comma/quote/newline → quoted, `"` → `""`), exact header first | CSV writer + reader round-trip tests | Planned |
| F6 | Column types/ranges: `recv_tnsec` in [0, 999 999 999]; `venue` ∈ {spot, usdm}; `stream_kind` ∈ {depth_diff, depth5, trade}; `shard_id` ≥ 0; `conn_epoch` +1 per reconnect; `conn_seq` monotonic within (shard_id, conn_epoch); `symbol` uppercase; `payload_json` minified one line | Unit tests per column; CSV validator script on sample | Planned |
| F7 | Receive-time policy chosen and documented | README "Timestamp policy" | Planned |
| F8 | Market-data rows in processing order (the order the LOB consumer sees) | Single consumer design; replay test | Planned |
| F9 | Order-book row after each processed event (min: every applied `depthUpdate` and `depth5`; trades optional, documented) | Processor tests; README | Planned |
| F10 | Order-book header exactly as above; 26 data columns per row | Header constant test (`static_assert` on column count); `awk -F',' 'NR==2{print NF}'` = 26 in CI | Planned |
| F11 | Column semantics: `seqNo` +1 per emitted row; `id` stable int32 (derivation documented); `type` single letter (mapping documented); `side` ∈ {B, S, N}; prices/sizes scaled int64, best first | Unit tests; README tables | Planned |
| F12 | Order-book timestamp policy documented (same source as market row) | README; replay equality test | Planned |
| F13 | Fixed price/qty scale documented, applied deterministically; no binary floating point in CSV integer columns | Decimal parser tests (exactness, overflow, rejection) | Planned |
| F14 | Reconnect/gap behaviour documented: `conn_epoch` and `U`/`u`/`pu` effects (reset/resync/drop) | Book state-machine tests; README | Planned |
| F15 | Diff semantics: absolute quantities, qty 0 removes level; top-5 correct | Book unit tests; exchange-simulator property test (rows equal the true top 5, including snapshot-coverage limits); hand-derived scenario fixtures; live depth5 cross-check metric | Planned |
| F16 | Output reproducible from fixed input | Replay determinism test: live output == replay output byte-for-byte (consistency); scenario fixtures with hand-derived expected output (correctness) | Planned |

## Submission

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| S1 | C++ only; CMake; README with compiler, standard, dependencies, full build commands, exact CLI for both CSVs, symbol-list format, scaling + timestamp policies, reconnect/gap summary, threading + I/O design note | README review checklist | Planned |
| S2 | Short sample run (1–2 min, one symbol): `market_data_*.csv` + matching `*_orderbook.csv` (same window) | `samples/` committed; README excerpt | Planned |
| S3 | Replay mode reads `market_data_*.csv` and reproduces the order book with no network calls; stated in README | Replay test runs with network disabled | Planned |
| S4 | `.gitignore` excludes `build/`, `*.csv`, `*.o`, `*.a`, `*.so`, `.env`, `config/secrets.json`; no credentials in repo | `.gitignore`; CI grep | Done (`.gitignore`) |
| S5 | Annotated tag `v1.0.0` ("Submission v1.0.0") pushed | `git tag -n` | Planned |
| S6 | README ends with "How to Submit on GitHub" and "Full Build & Run Commands" (Ubuntu 22.04 packages; GCC 12, C++17; tested with Clang 15) | README; CI uses the same package list | Planned |
| S7 | CLI: `./build/binance_capture --venue spot\|usdm --symbols BTCUSDT[,ETHUSDT] --output-dir ./output [--duration 300]` | CLI parser tests; README | Planned |
| S8 | Tests: `cmake --build build --target tests` then `./build/tests/unit_tests` | CI step runs exactly these commands | Done (scaffold) |
| S9 | Reviewer checklist passes verbatim: clean build `cmake -B build && cmake --build build`; live `--duration 300` without `--output-dir` (outputs land in the working directory); `awk` column check = 26; `git grep -i "api_key\|secret\|password"` empty | CI `reviewer-clean-build` job (build); release checklist script (rest) | Planned |

## Review focus (code quality and performance)

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| Q1 | Diff vs depth5 replace semantics; trades do not mutate a diff-based book unless documented | Book tests; README | Planned |
| Q2 | `U`/`u`/`pu` sequencing and new-`conn_epoch` behaviour consistent with CSV output | State-machine tests; replay test across an epoch change | Planned |
| Q3 | Determinism: integer scaling, no `double` on the path to CSV integers, stable level ordering | Code review; tests | Planned |
| Q4 | RFC 4180 escaping of `payload_json`, no silent truncation, exact headers | CSV tests incl. adversarial payloads | Planned |
| Q5 | RAII ownership; no dangling references across async callbacks; clean SIGINT/SIGTERM teardown; sanitizer/Valgrind clean | ASan/UBSan + TSan CI jobs; Valgrind on replay | Planned |
| Q6 | Reconnect paths, partial reads, parse failures are logged and counted, never UB | Metrics; fault-injection tests | Planned |
| Q7 | Modern C++: `constexpr`, `enum class`, scoped types, no unnecessary macros, const-correct hot structures | clang-tidy CI job | Planned |
| Q8 | `-Wall -Wextra` (stricter used), Release documented, zero warnings | CI builds with `-Werror` on GCC 12 and Clang 15 | Done (scaffold) |
| Q9 | Efficient JSON parsing (simdjson On-Demand), parse each payload once, reuse buffers | Code; benchmark | Planned |
| Q10 | Minimal per-message heap allocation (`reserve`, ring buffers, reuse) | Allocation-counting test with no warm-up: zero allocations in the processor for every record kind and state, as scoped in design §10 (network path, logging, startup/shutdown and the replay reader excluded) | Planned |
| Q11 | Efficient top-5 maintenance per message (the brief asks for O(updates)); trade-offs vs a full-map book explained | Design §5.1 states the actual cost: O(log k) search + O(k) moves per level, k = levels better than the price, worst case O(n) for deep levels. Benchmark of flat vector vs `std::map` covering near-touch and deep updates and deletions; README states the result and trade-off | Planned |
| Q12 | File/socket back-pressure; buffered writes vs dedicated writer thread decision documented | README design note; ring-buffer high-water metric | Planned |
| Q13 | Clear thread boundaries, no data races, no contended locks per tick | SPSC ring tests under TSan; README | Planned |
| Q14 | Metrics: messages processed, parse errors, reconnects, rows written | Periodic stats line + final summary | Planned |
| Q15 | Replay mode regenerates `*_orderbook.csv` from saved `market_data_*.csv` | Replay CI job | Planned |
| Q16 | No secrets in repo; TLS peer + hostname verification enabled | Code; README | Planned |
| Q17 | Limits on URL length, streams/symbols per shard, frame size, buffers | Config validation tests | Planned |

## Optional stretch

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| X1 | REST depth snapshot + buffered-diff resync per Binance docs | State-machine tests; live resync metrics | Planned |
| X2 | Multi-symbol with correct sharding | Sharding unit tests; multi-symbol live run | Planned |
| X3 | Performance notes (throughput, latency) | Benchmarks + README numbers | Planned |
