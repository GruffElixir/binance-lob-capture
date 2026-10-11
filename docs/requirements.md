# Requirements traceability

Every requirement from the assignment brief, with the evidence that proves it is met.
`Status` moves from **Planned** → **Partial** (some of the evidence exists, named in the cell)
→ **Done** only when all the evidence exists (test, CI job, or README section).

Release gate for the `v1.0.0` tag:

- **Functional, Submission and Review focus** rows are mandatory: all must be **Done**.
- **Optional stretch** rows must be **Done** or marked **Deferred** with a reason.
- Enhancements chosen in the design (marked "(enhancement)" in [design.md](design.md)) are not
  submission gates.
- Before creating the tag, all mandatory rows except S5 must be Done. S5 is verified after
  the annotated tag is pushed, then recorded in a documentation follow-up on main before
  publication; the release tag is never moved for that bookkeeping.

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
| F1 | Live capture from Binance combined streams: `depth@100ms`, `depth5@100ms`, `trade` per configured symbol | [endpoint tests](../tests/net_endpoints_test.cpp); both-venue [live runs](benchmarks.md) | Done |
| F2 | Venues `spot` and `usdm` with the base URLs above | [endpoint tests](../tests/net_endpoints_test.cpp); both-venue [live runs](benchmarks.md) | Done |
| F3 | Market-data CSV: one row per message applied from the socket, storing the inner `data` payload (envelope removed) | [router tests](../tests/pipeline_frame_router_test.cpp); verified frame accounting in [live runs](benchmarks.md) | Done |
| F4 | File naming `market_data_<venue>_<SYMBOL>_<UTC-date>.csv`, with the order book at `market_data_<venue>_<SYMBOL>_<UTC-date>_orderbook.csv` (the brief's output listing); choice documented | [file tests](../tests/pipeline_output_files_test.cpp); run-start date regression; [README](../README.md) | Done |
| F5 | CSV: UTF-8, RFC 4180 quoting (comma/quote/newline → quoted, `"` → `""`), exact header first | CSV writer + reader round-trip tests | Done — `csv_writer_test`, `csv_reader_test` (round trip, adversarial fields) |
| F6 | Column types/ranges: `recv_tnsec` in [0, 999 999 999]; `venue` ∈ {spot, usdm}; `stream_kind` ∈ {depth_diff, depth5, trade}; `shard_id` ≥ 0; `conn_epoch` +1 per reconnect; `conn_seq` monotonic within (shard_id, conn_epoch); `symbol` uppercase; `payload_json` minified one line | [row verification](../src/blc/verify/verify.cpp); scenario and real sample verification | Done |
| F7 | Receive-time policy chosen and documented | [README timestamp policy](../README.md#timestamps-and-integer-columns) | Done |
| F8 | Market-data rows in processing order (the order the LOB consumer sees) | Single consumer design; replay test | Done — single consumer (§6.1); `Processor.SidecarApplyBeforeRow`, `Replay.LossIntegrationReplaysIdentically` |
| F9 | Order-book row after each processed event (min: every applied `depthUpdate` and `depth5`; trades optional, documented) | [processor tests](../tests/pipeline_processor_test.cpp); [README book policy](../README.md) | Done |
| F10 | Order-book header exactly as above; 26 data columns per row | Header constant test (`static_assert` on column count); `awk -F',' 'NR==2{print NF}'` = 26 in CI | Done — `CsvSchema` tests + `static_assert`; `scripts/verify_outputs.sh` (26 columns) in the `output-contract` job |
| F11 | Column semantics: `seqNo` +1 per emitted row; `id` stable int32 (derivation documented); `type` single letter (mapping documented); `side` ∈ {B, S, N}; prices/sizes scaled int64, best first | [processor tests](../tests/pipeline_processor_test.cpp); [README integer/type tables](../README.md) | Done |
| F12 | Order-book timestamp policy documented (same source as market row) | sync-by-later-diff scenario; [README](../README.md); exact sample replay | Done |
| F13 | Fixed price/qty scale documented, applied deterministically; no binary floating point in CSV integer columns | Decimal parser tests (exactness, overflow, rejection) | Done — `core_decimal_test` (exactness, overflow, rejection); design §4.2 |
| F14 | Reconnect/gap behaviour documented: `conn_epoch` and `U`/`u`/`pu` effects (reset/resync/drop) | [sync tests](../tests/book_sync_test.cpp); gap/epoch scenarios; [README](../README.md) | Done |
| F15 | Diff semantics: absolute quantities, qty 0 removes level; top-5 correct | [independent property oracle](../tests/book_property_test.cpp); [refresh oracle](../tests/book_refresh_test.cpp); hand-derived scenarios; positive [live depth5 checks](benchmarks.md) | Done |
| F16 | Output reproducible from fixed input | hand-derived scenarios; [sample](../samples/); byte-identical live/replay in both venues and two shards | Done |

## Submission

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| S1 | C++ only; CMake; README with compiler, standard, dependencies, full build commands, exact CLI for both CSVs, symbol-list format, scaling + timestamp policies, reconnect/gap summary, threading + I/O design note | [README](../README.md); `scripts/check_readme.sh` passed | Done |
| S2 | Short sample run (1–2 min, one symbol): `market_data_*.csv` + matching `*_orderbook.csv` (same window) | [committed sample set](../samples/); actual 120-second capture, verifier and replay equality; README excerpt | Done |
| S3 | Replay mode reads `market_data_*.csv` and reproduces the order book with no network calls; stated in README | [README replay](../README.md#replay); no-socket assertion and strace in CI | Done |
| S4 | `.gitignore` excludes `build/`, `*.csv`, `*.o`, `*.a`, `*.so`, `.env`, `config/secrets.json`; no credentials in repo; config via environment variables or a `.env.example` template | tracked comments-only [.env.example](../.env.example); `check_readme.sh` and extended credential check passed | Done |
| S5 | Annotated tag `v1.0.0` ("Submission v1.0.0") pushed | `git tag -n` | Planned |
| S6 | README ends with the brief's submission section, in order: "How to Submit on GitHub" (1. initialize and push, 2. `.gitignore`, 3. tag), then "Full Build & Run Commands (Reproducible)" with Prerequisites, Clone and build, Run — Live capture, Run — Verify CSV schema, Run tests (if applicable) and the Reviewer Checklist table (Ubuntu 22.04 packages; GCC 12, C++17; tested with Clang 15). **Verbatim policy:** headings and commands are kept verbatim, except for these documented adaptations: `<your-username>` → `GruffElixir`; example dates → the real sample date; the output listing adds the depth-snapshot sidecar; the package list adds `ca-certificates`; pipes inside the Reviewer Checklist table are escaped (Markdown requires it). | [README appendix](../README.md#how-to-submit-on-github); [ordered manifest](submission_commands.txt); `check_readme.sh` passed | Done |
| S7 | CLI: `./build/binance_capture --venue spot\|usdm --symbols BTCUSDT[,ETHUSDT] --output-dir ./output [--duration 300]` | [option tests](../tests/cli_options_test.cpp); [README examples](../README.md) | Done |
| S8 | Tests: `cmake --build build --target tests` then `./build/tests/unit_tests` | CI step runs exactly these commands | Done (scaffold) |
| S9 | Reviewer checklist passes verbatim: clean build `cmake -B build && cmake --build build`; live `--duration 300` without `--output-dir` (outputs land in the working directory); `awk` column check = 26; `git grep -i "api_key\|secret\|password"` empty | CI `reviewer-clean-build` job (build); release checklist script (rest) | Planned |

## Review focus (code quality and performance)

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| Q1 | Diff vs depth5 replace semantics; trades do not mutate a diff-based book unless documented | [book tests](../tests/book_sync_test.cpp); depth5/trade policy in [README](../README.md) | Done |
| Q2 | `U`/`u`/`pu` sequencing and new-`conn_epoch` behaviour consistent with CSV output | [sequencing tests](../tests/book_sequence_test.cpp); epoch scenarios and real reconnect tests | Done |
| Q3 | Determinism: integer scaling, no `double` on the path to CSV integers, stable level ordering | [decimal tests](../tests/core_decimal_test.cpp); integer CSV formatting and sorted book tests; exact replay | Done |
| Q4 | RFC 4180 escaping of `payload_json`, no silent truncation, exact headers | CSV tests incl. adversarial payloads | Done — `csv_writer_test` adversarial payloads; `Verify.RejectsEachViolation`; no-truncation rule in the writer |
| Q5 | RAII ownership; no dangling references across async callbacks; websocket + I/O teardown on SIGINT/SIGTERM without leaks; sanitizer/Valgrind clean | `capture_loopback` integration test (real `WsShard`, REST scheduler and TLS against an in-process TLS server; SIGINT, second SIGINT, `--duration`, forced reconnect) under ASan+LSan and TSan in CI; `live_smoke.sh` with an ASan build ended by SIGINT (zero LSan reports); Valgrind on replay | Planned |
| Q6 | Reconnect paths, partial reads, parse failures are logged and counted, never UB | 28 TLS [loopback tests](../tests/net_capture_loopback_test.cpp), fault reconnect and oversized/invalid input cases | Done |
| Q7 | Modern C++: `constexpr`, `enum class`, scoped types, no unnecessary macros, const-correct hot structures | clang-tidy CI job | Planned |
| Q8 | `-Wall -Wextra` (stricter used), Release documented, zero warnings | CI builds first-party code with `-Werror` on GCC 12 and Clang 15 (in place); CI fails on any `warning:` line in the README-command build log, including vendored/fetched code; README documents the Release build | Planned |
| Q9 | Efficient JSON parsing (simdjson On-Demand), decode each payload into events once and reuse buffers; the deliberate network syntax-validation pass keeps invalid JSON out of CSVs | [payload parser](../src/blc/parse/payload_parser.cpp); [typed/network benchmark measurements](benchmarks.md) | Done |
| Q10 | Minimal per-message heap allocation (`reserve`, ring buffers, reuse) | [allocation tests](../tests/pipeline_allocation_test.cpp), real file output including mismatch/refresh paths; zero processing allocations | Done |
| Q11 | Efficient top-5 maintenance per message (the brief asks for O(updates)); trade-offs vs a full-map book explained | four flat/map workloads, 1,000/5,000 levels; [numbers and trade-offs](benchmarks.md), README summary | Done |
| Q12 | File/socket back-pressure; buffered writes vs dedicated writer thread decision documented | [README threading/back-pressure note](../README.md); bounded ring/drop-loss tests and high-water metrics | Done |
| Q13 | Clear thread boundaries, no data races, no contended locks per tick | SPSC ring tests under TSan; README | Planned |
| Q14 | Metrics: messages processed, parse errors, reconnects, rows written | live periodic/final summaries; [metric tests](../tests/pipeline_consumer_test.cpp); README metrics | Done |
| Q15 | Replay mode regenerates `*_orderbook.csv` from saved `market_data_*.csv` | all hand-derived scenarios and real [sample](../samples/) replayed identically; CI sample/scenario gate | Done |
| Q16 | No secrets in repo; TLS peer + hostname verification enabled | credential check passed; real in-memory CA rejection tests; TLS peer/hostname verification enabled; README | Done |
| Q17 | Limits on URL length, streams/symbols per shard, frame size, buffers | [endpoint limits tests](../tests/net_endpoints_test.cpp); oversized/fragmented loopback tests; bounded ring and decode limits | Done |

## Optional stretch

| ID | Requirement | Evidence | Status |
|---|---|---|---|
| X1 | REST depth snapshot + buffered-diff resync per Binance docs | snapshot/buffered-diff sync tests; positive sync/resync in both-venue [live runs](benchmarks.md) | Done |
| X2 | Multi-symbol with correct sharding | sharding tests; real 60-second BTCUSDT/ETHUSDT two-connection capture, verifier and per-file replay equality | Done |
| X3 | Performance notes (throughput, latency) | book, payload, decimal, ring and per-record replay [measurements](benchmarks.md); background refresh and independent oracle | Done |
