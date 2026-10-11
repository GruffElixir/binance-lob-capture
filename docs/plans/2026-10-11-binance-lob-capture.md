# binance-lob-capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `binance_capture`, a C++17 tool that captures Binance spot/USD-M `depth@100ms`, `depth5@100ms` and `trade` streams into an audit CSV, maintains a REST-synchronised local order book, writes a 26-column top-5 CSV, and replays captures byte-for-byte without network.

**Architecture:** Two library targets: `blc_core` (everything except sockets: decimal/CSV/parsing/book/pipeline/replay) and `blc_net` (Boost.Beast/OpenSSL). Two threads at run time: an io thread (Asio) that splits envelopes and pushes records into a lock-free SPSC byte ring, and a processor thread that writes CSVs and runs the per-symbol sync state machine. Replay feeds the same processor from CSV files.

**Tech Stack:** C++17, CMake ≥ 3.22, GCC 12 / Clang 15, Boost 1.74 (Asio, Beast), OpenSSL 3, simdjson 4.6.11 (vendored), GoogleTest 1.17.0, GitHub Actions on `ubuntu:22.04`.

**Spec:** [docs/design.md](../design.md). Section references (§n) point there. [docs/requirements.md](../requirements.md) is the release gate.

## Global Constraints

- Language C++17 exactly (`CMAKE_CXX_STANDARD 17`, extensions off). Must compile warning-free with GCC 12 and Clang 15 at `-Wall -Wextra` plus the `cmake/CompilerWarnings.cmake` set, and with `-Werror` in CI.
- Use only Boost 1.74 APIs (the jammy package). No Boost.JSON. Nothing from C++20 (`std::span`, `std::format`, `<bit>` functions newer than C++17, `std::atomic::wait`).
- Everything is in namespace `blc`. Sources live in `src/blc/<module>/`; each test file is `tests/<module>_<unit>_test.cpp`, added to the `unit_tests` executable in `tests/CMakeLists.txt`.
- No binary floating point on any path that produces a CSV integer. Prices and quantities are `int64` scaled by `100'000'000`.
- **Exceptions:** allowed only at startup and in replay file handling (`blc::UsageError` → exit 2, `blc::IoError` → exit 1). A malformed replay row makes that one file fail; the capture path never throws. The per-record capture path uses status enums.
- **Files** are always opened in binary mode (`"wb"` / `"rb"`), so MSYS2/Windows never rewrites line endings, and byte comparisons hold on every platform.
- **Timing in tests:** no test asserts an exact count of time-driven events or a tight wall-clock deadline. Tests wait on events (for example "the server saw the ClientHello") and assert ordering plus generous upper bounds. Every deadline is multiplied by `BLC_TEST_TIME_SCALE` (default 1; the sanitizer CI jobs set 10).
- **Exact strings** are copied from §3.3/§3.4/§3.5 and nowhere retyped. The headers live only in `src/blc/csv/schema.hpp`.
- **Commits:** plain messages, imperative mood, **no `Co-Authored-By` or any Claude attribution**. Every commit leaves CI green.
- **Forbidden string:** the reviewer's `git grep` pattern must never appear as a literal in any tracked file (§9).
- **Formatting:** run `clang-format -i` on touched C++ files before each commit (enforced from Task 6). Use the CI version locally: `pip install clang-format==15.0.7`. `third_party/` is never formatted.
- **Local runs before WSL exists:** M1–M4 build and test on MSYS2 (GCC 16) plus CI. Sanitizer and Linux-only checks (TSan, Valgrind, strace, `/dev/full`, POSIX permissions) are gated by CI until WSL2 Ubuntu 22.04 exists; from M5 on everything also runs in WSL.
- **Level ranges** are passed as `LevelSpan` (pointer + count), never as freshly built vectors, so the processor path never allocates.
- **Test command used throughout:** `cmake --build build/debug --target unit_tests && ./build/debug/tests/unit_tests --gtest_filter=<Filter>`, where `build/debug` comes from `cmake --preset debug`.

## Review Focus

Inputs and failures the spec implies but no feature test would naturally hit. Each line's test is added to the task named.

1. `--symbols " btcusdt , ETHusdt "` (spaces, mixed case) must be accepted as `BTCUSDT,ETHUSDT`, while `BTCUSDT,` (trailing comma) and `BTCUSDT,,ETH` are usage errors naming the empty item. → **Task 5**
2. An unwritable or invalid `--output-dir` (the path is an existing regular file, or a directory without write permission) must fail at startup with exit 1 and a message naming the path, before any WebSocket opens. → **Task 18** (unit test; one CI job runs the tests as a non-root user so the permission case executes)
3. A replay input saved by a Windows editor (CRLF line endings, missing final newline, trailing blank line) must replay to the same order book as the original. → **Task 4** (reader) and **Task 21** (replay)
4. A receive timestamp that is an exact multiple of 1 s must give `tnsec == 0`, never `1000000000`. Dates around UTC midnight and 2000-02-29 / 2100-03-01 must format correctly. → **Task 1**
5. A capture whose `--duration` ends before any symbol synchronises must exit 0. It leaves header-only order-book files and a summary with `syncs=0`; it must not report an error. → **Task 27**

---

## Milestone M1 — Core primitives

### Task 1: Core types, time, instrument id; `blc_core` becomes a static library

**Files:**
- Create: `src/blc/core/types.hpp`, `src/blc/core/time.hpp`, `src/blc/core/time.cpp`, `src/blc/core/instrument_id.hpp`, `src/blc/core/errors.hpp`
- Modify: `src/CMakeLists.txt` (`blc_core` → `STATIC`, add sources, `blc_set_warnings(blc_core)`), `tests/CMakeLists.txt`
- Test: `tests/core_time_test.cpp`, `tests/core_instrument_id_test.cpp`

**Interfaces:**
- Produces:
  - `types.hpp`:
    - `enum class Venue : std::uint8_t { Spot, Usdm };` with `std::string_view to_string(Venue)` returning `"spot"` / `"usdm"`, and `std::optional<Venue> parse_venue(std::string_view)`.
    - `enum class StreamKind : std::uint8_t { DepthDiff, Depth5, Trade };` with `to_string` returning `"depth_diff"` / `"depth5"` / `"trade"`.
    - `struct Level { std::int64_t price; std::int64_t qty; };` with `operator==`.
    - `struct LevelSpan { const Level* data = nullptr; std::size_t size = 0; };` with
      `LevelSpan span_of(const std::vector<Level>&) noexcept`.
    - `inline constexpr std::int64_t kPriceScale = 100'000'000, kQtyScale = 100'000'000;`
    - `struct PaddedView { const char* data; std::size_t size; std::size_t capacity; };` (capacity ≥ size + padding).
  - `time.hpp`:
    - `struct SplitTime { std::int64_t sec; std::int32_t nsec; };` and `constexpr SplitTime split_ns(std::int64_t ns) noexcept` (floor division).
    - `std::string utc_date(std::int64_t ns)` → `"YYYY-MM-DD"`.
    - `std::string utc_compact_stamp(std::int64_t ns)` → `"YYYYMMDDTHHMMSSZ"`.
    - `std::int64_t wall_clock_ns() noexcept`.
  - `instrument_id.hpp`: `constexpr std::uint32_t fnv1a32(std::string_view) noexcept;` and `std::int32_t instrument_id(Venue, std::string_view upper_symbol);` (masked with `0x7FFFFFFF` over the bytes of `"<venue>:<SYMBOL>"`).
  - `errors.hpp`: `struct UsageError : std::runtime_error`, `struct IoError : std::runtime_error`.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(SplitNs, ExactSecondHasZeroNanos) { EXPECT_EQ(split_ns(1'791'669'451'000'000'000).nsec, 0);
  EXPECT_EQ(split_ns(1'791'669'451'000'000'000).sec, 1'791'669'451); }
TEST(SplitNs, Remainder) { auto t = split_ns(1'791'669'451'812'345'678);
  EXPECT_EQ(t.sec, 1'791'669'451); EXPECT_EQ(t.nsec, 812'345'678); }
TEST(UtcDate, Boundaries) { EXPECT_EQ(utc_date(0), "1970-01-01");
  EXPECT_EQ(utc_date(951'782'400'000'000'000), "2000-02-29");   // leap day
  EXPECT_EQ(utc_date(4'107'542'400'000'000'000), "2100-03-01");
  EXPECT_EQ(utc_date(1'791'676'799'999'999'999), "2026-10-10"); // last ns of 2026-10-10
  EXPECT_EQ(utc_date(1'791'676'800'000'000'000), "2026-10-11"); // first ns of 2026-10-11
  EXPECT_EQ(utc_compact_stamp(1'791'669'451'000'000'000), "20261010T215731Z"); }
TEST(InstrumentId, SpecValues) { EXPECT_EQ(instrument_id(Venue::Spot, "BTCUSDT"), 2014473836);
  EXPECT_EQ(instrument_id(Venue::Usdm, "BTCUSDT"), 1567771867);
  EXPECT_EQ(instrument_id(Venue::Spot, "ETHUSDT"), 1884648048); }
TEST(Types, VenueAndKindStrings) { EXPECT_EQ(to_string(StreamKind::DepthDiff), "depth_diff");
  EXPECT_EQ(parse_venue("usdm"), Venue::Usdm); EXPECT_FALSE(parse_venue("USDM").has_value()); }
```

- [ ] **Step 2: Run the tests to verify they fail.** Run `cmake --preset debug && cmake --build build/debug --target unit_tests`. Expected: compile errors (headers missing).
- [ ] **Step 3: Implement the headers and `time.cpp`.** Dates use the days-from-civil algorithm (Howard Hinnant's `civil_from_days`), not `gmtime`, so there is no thread or locale dependence. `wall_clock_ns` is `system_clock::now()` converted to ns.
- [ ] **Step 4: Run** `./build/debug/tests/unit_tests --gtest_filter='SplitNs*:UtcDate*:InstrumentId*:Types*'`. Expected: all pass.
- [ ] **Step 5: Commit** — `git commit -m "Add core types, time helpers and instrument id"`

### Task 2: Exact decimal parser

**Files:**
- Create: `src/blc/core/decimal.hpp`, `src/blc/core/decimal.cpp`
- Test: `tests/core_decimal_test.cpp`

**Interfaces:**
- Produces: `enum class DecimalError : std::uint8_t { Ok, Empty, InvalidChar, TooPrecise, Overflow };` and `DecimalError parse_e8(std::string_view text, std::int64_t& out) noexcept;` (grammar `[0-9]+(\.[0-9]+)?`, §4.2).

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Decimal, SpecTable) { std::int64_t v{};
  EXPECT_EQ(parse_e8("83044.00000000", v), DecimalError::Ok); EXPECT_EQ(v, 8304400000000);
  parse_e8("3.25216000", v); EXPECT_EQ(v, 325216000);  parse_e8("0.00009000", v); EXPECT_EQ(v, 9000);
  parse_e8("83000.10", v); EXPECT_EQ(v, 8300010000000); parse_e8("0.022", v); EXPECT_EQ(v, 2200000);
  parse_e8("0", v); EXPECT_EQ(v, 0); parse_e8("0.000000010", v); EXPECT_EQ(v, 1); }
TEST(Decimal, Rejects) { std::int64_t v{};
  for (auto s : {"", ".5", "5.", "-1", "+1", "1e5", "1,0", " 1", "1 "})
    EXPECT_NE(parse_e8(s, v), DecimalError::Ok) << s;
  EXPECT_EQ(parse_e8("0.000000001", v), DecimalError::TooPrecise);
  EXPECT_EQ(parse_e8("", v), DecimalError::Empty); }
TEST(Decimal, OverflowBoundary) { std::int64_t v{};
  EXPECT_EQ(parse_e8("92233720368.54775807", v), DecimalError::Ok); EXPECT_EQ(v, INT64_MAX);
  EXPECT_EQ(parse_e8("92233720368.54775808", v), DecimalError::Overflow);
  EXPECT_EQ(parse_e8("100000000000", v), DecimalError::Overflow); }
```

- [ ] **Step 2: Run** `--gtest_filter='Decimal*'`. Expected: FAIL (undefined `parse_e8`).
- [ ] **Step 3: Implement `parse_e8`.** Accumulate digits into `unsigned __int128`-free 64-bit arithmetic with explicit overflow checks (`__builtin_mul_overflow` / `__builtin_add_overflow`, available on GCC and Clang). Do not use `std::from_chars` for the fraction. `.5` and `5.` are `InvalidChar`.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add exact decimal-to-int64 parser"`

### Task 3: CSV schema and writer

**Files:**
- Create: `src/blc/csv/schema.hpp`, `src/blc/csv/csv_writer.hpp`, `src/blc/csv/csv_writer.cpp`
- Test: `tests/csv_schema_test.cpp`, `tests/csv_writer_test.cpp`

**Interfaces:**
- Produces:
  - `schema.hpp`:
    - `inline constexpr std::string_view kMarketDataHeader`, `kOrderBookHeader`, `kSnapshotHeader` (exact text of §3.3/§3.4/§3.5, no trailing newline).
    - `constexpr std::size_t column_count(std::string_view header)`.
    - `static_assert`s for 9 / 26 / 8 columns.
  - `csv_writer.hpp`:
    - `class OutputFile`:
      - `static OutputFile create(const std::filesystem::path&, char* buffer, std::size_t capacity);` — throws `IoError`; the buffer is owned by the caller.
      - `void write(std::string_view) noexcept;` — buffers; flushes when full.
      - `bool flush() noexcept;`, `bool close() noexcept;`, `bool failed() const noexcept;`
      - Move-only. Wraps an unbuffered `std::FILE*` (`setvbuf(_IONBF)`).
    - `class CsvRow`:
      - `explicit CsvRow(OutputFile&)`;
      - `CsvRow& integer(std::int64_t)`, `CsvRow& character(char)`, `CsvRow& text(std::string_view)` (quoted iff it contains `,` `"` CR LF; `"` → `""`);
      - `void end()` — writes `\n`;
      - inserts the separators itself.
    - `bool needs_quoting(std::string_view) noexcept`.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Schema, ExactHeaders) {
  EXPECT_EQ(kOrderBookHeader, "tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4");
  EXPECT_EQ(kMarketDataHeader, "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json");
  EXPECT_EQ(kSnapshotHeader, "recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_json");
  EXPECT_EQ(column_count(kOrderBookHeader), 26u); }
TEST(CsvWriter, QuotesOnlyWhenNeeded) {
  // writes to a temp file through a 16-byte buffer to force mid-row flushes
  // row: integer(1791669451).text("spot").text(R"({"e":"trade","p":"1,5"})").character('B').end()
  // expected file bytes:
  //   1791669451,spot,"{""e"":""trade"",""p"":""1,5""}",B\n
}
TEST(CsvWriter, EmbeddedNewlineAndCr) { /* text("a\nb") -> "\"a\nb\"", text("a\rb") quoted */ }
TEST(CsvWriter, NegativeAndZero) { /* integer(0) -> "0", integer(INT64_MIN) -> "-9223372036854775808" */ }
TEST(CsvWriter, FailedWriteIsSticky) { /* create on /dev/full (Linux only, GTEST_SKIP elsewhere):
  write + flush -> false; failed() == true; close() == false */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Schema*:CsvWriter*'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Integers go through `std::to_chars` into a 20-byte stack buffer. Escaping writes runs between quotes straight into `OutputFile::write`, so there are no temporary strings.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add CSV schema constants and RFC 4180 writer"`

### Task 4: CSV reader

**Files:**
- Create: `src/blc/csv/csv_reader.hpp`, `src/blc/csv/csv_reader.cpp`
- Test: `tests/csv_reader_test.cpp`

**Interfaces:**
- Produces:
  - `class CsvReader`:
    - `static CsvReader open(const std::filesystem::path&);` — throws `IoError`.
    - `bool next(std::vector<std::string_view>& fields);` — false at EOF. Views stay valid until the next call. Throws `CsvFormatError{line}` (derived from `IoError`) for an unterminated quote or a stray `"` in an unquoted field.
    - `std::size_t line() const;`
  - Accepts LF and CRLF, a missing final newline and trailing blank lines (skipped).

- [ ] **Step 1: Write failing tests** — round-trip every `CsvWriter` case from Task 3, plus:

```cpp
TEST(CsvReader, CrlfAndNoFinalNewline) { /* "a,b\r\n\"x,\"\"y\"\"\",2" -> {"a","b"}, {"x,\"y\"","2"} */ }
TEST(CsvReader, TrailingBlankLinesIgnored) { /* "a\n\n\n" -> one row */ }
TEST(CsvReader, EmbeddedNewlineInQuotes) { /* "\"l1\nl2\",z\n" -> {"l1\nl2","z"} */ }
TEST(CsvReader, UnterminatedQuoteThrowsWithLine) { /* "a\n\"b\n" -> CsvFormatError, line()==2 */ }
```

- [ ] **Step 2: Run** `--gtest_filter='CsvReader*'`. Expected: FAIL.
- [ ] **Step 3: Implement** a streaming reader: `fread` into a 1 MiB chunk buffer, plus a reusable row buffer that grows only to the longest row seen (§10). A single-pass state machine handles rows that span chunk boundaries and un-escapes `""` into the row buffer. Memory is independent of file size: a 31-day capture must replay in bounded memory. Add `TEST(CsvReader, RowSpanningChunkBoundary)`, which builds a file with a 3 MiB quoted field and several rows straddling the 1 MiB boundaries.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add RFC 4180 CSV reader"`

### Task 5: Command-line options, symbol list, logging, `main` wiring

**Files:**
- Create: `src/blc/cli/options.hpp`, `src/blc/cli/options.cpp`, `src/blc/core/symbols.hpp`, `src/blc/core/symbols.cpp`, `src/blc/util/logging.hpp`, `src/blc/util/logging.cpp`
- Modify: `src/main.cpp`
- Test: `tests/cli_options_test.cpp`, `tests/core_symbols_test.cpp`

**Interfaces:**
- Consumes: `Venue`, `parse_venue`, `instrument_id`, `UsageError` (Task 1).
- Produces:
  - `symbols.hpp`:
    - `struct SymbolSpec { std::string upper; std::string lower; std::int32_t id; };`
    - `std::vector<SymbolSpec> parse_symbol_list(Venue, std::string_view list);` — throws `UsageError`. Applies the §2 rules (trim, uppercase, `[A-Z0-9]{2,20}`, no duplicates, 1–256 items, distinct ids).
  - `options.hpp`:
    - `struct CaptureOptions { Venue venue = Venue::Spot; std::vector<SymbolSpec> symbols; std::filesystem::path output_dir = "."; std::int64_t duration_s = 0; bool trade_rows = true; int symbols_per_connection = 50; int stats_interval_s = 10; int fault_reconnect_every_s = 0; };`
    - `struct ReplayOptions { std::vector<std::filesystem::path> inputs; std::filesystem::path output_dir = "./replay_output"; bool trade_rows = true; int stats_interval_s = 10; };`
    - `enum class Mode { Capture, Replay, Help, Version };`
    - `struct Options { Mode mode; CaptureOptions capture; ReplayOptions replay; };`
    - `Options parse_options(int argc, const char* const* argv);` — throws `UsageError`.
    - `std::string usage_text();`
  - `logging.hpp`:
    - `enum class LogLevel { Info, Warn, Error };`
    - `void log(LogLevel, std::string_view message);` — stderr, ISO-8601 UTC prefix, thread-safe line writes.
    - `class RateLimitedLog { bool should_log() noexcept; };` — first 10, then every 1000th.
    - `using LogAllocationHook = void (*)(bool suspend) noexcept;` and
      `void set_log_allocation_hook(LogAllocationHook) noexcept;` — `log()` calls the hook with
      `true` before formatting and `false` after. The allocation test (Task 19) installs it to
      pause its counter; production leaves it null.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Symbols, NormalisesSpacesAndCase) { auto s = parse_symbol_list(Venue::Spot, " btcusdt , ETHusdt ");
  ASSERT_EQ(s.size(), 2u); EXPECT_EQ(s[0].upper, "BTCUSDT"); EXPECT_EQ(s[0].lower, "btcusdt");
  EXPECT_EQ(s[1].id, 1884648048); }
TEST(Symbols, Rejects) { for (auto l : {"", "BTCUSDT,", "BTCUSDT,,ETHUSDT", "B", "BTC-USDT",
                                       "BTCUSDT,btcusdt", "ABCDEFGHIJKLMNOPQRSTU"})
  EXPECT_THROW(parse_symbol_list(Venue::Spot, l), UsageError) << l; }
TEST(Options, Defaults) { const char* a[] = {"bc", "--symbols", "BTCUSDT"}; auto o = parse_options(3, a);
  EXPECT_EQ(o.mode, Mode::Capture); EXPECT_EQ(o.capture.output_dir, "."); EXPECT_EQ(o.capture.venue, Venue::Spot);
  EXPECT_EQ(o.capture.symbols_per_connection, 50); EXPECT_TRUE(o.capture.trade_rows); }
TEST(Options, EqualsForm) { const char* a[] = {"bc", "--venue=usdm", "--symbols=BTCUSDT", "--duration=300"};
  auto o = parse_options(4, a); EXPECT_EQ(o.capture.venue, Venue::Usdm); EXPECT_EQ(o.capture.duration_s, 300); }
TEST(Options, ReplayDefaults) { const char* a[] = {"bc", "--replay", "x.csv", "y.csv"}; auto o = parse_options(4, a);
  EXPECT_EQ(o.mode, Mode::Replay); EXPECT_EQ(o.replay.inputs.size(), 2u); EXPECT_EQ(o.replay.output_dir, "./replay_output"); }
TEST(Options, AllFlagsParsed) { /* --no-trade-rows -> trade_rows false; --stats-interval 0; --fault-reconnect-every 30;
  --help -> Mode::Help; --version -> Mode::Version */ }
TEST(Options, UsageErrors) { /* each throws UsageError:
  --symbols-per-connection 342; 257 symbols; --stats-interval -1; --fault-reconnect-every -1;
  unknown flag; --venue coin; --duration -1; --duration 2678401; --symbols missing in capture;
  --symbols-per-connection 0; 256 symbols with --symbols-per-connection 1 (65+ connections > 64);
  --replay with no files; --replay combined with --symbols; flag without value */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Symbols*:Options*'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Hand-rolled parser with no dependency. The venue cap (341 symbols per connection) and the connection cap (`⌈n/per⌉ ≤ 64`) are checked here; the URL-length check lives in Task 23. In `main.cpp`:
  - `--help` → `usage_text()` on stdout, exit 0;
  - `--version` → as now;
  - `UsageError` → message plus a hint on stderr, exit 2;
  - capture and replay print "not implemented yet" and exit 2 until Tasks 21 and 26.
- [ ] **Step 4: Run** the tests, then `./build/debug/binance_capture --symbols ,` and check the exit code: expect `2` and a message naming the empty item.
- [ ] **Step 5: Commit** — `git commit -m "Add option and symbol-list parsing, logging"`

### Task 6: CI — clang-format and ASan/UBSan jobs

**Files:**
- Modify: `.github/workflows/ci.yml`, plus every C++ file (formatting only)

- [ ] **Step 1:** Run `clang-format -i` over `src/` and `tests/`, and check that `git diff` shows formatting only.
- [ ] **Step 2: Add the `format` job.** It installs `git` (so `actions/checkout` produces a real repository) and `clang-format-15` in `ubuntu:22.04`, then runs `clang-format-15 --dry-run --Werror $(git ls-files '*.cpp' '*.hpp' '*.hpp.in' ':!:third_party/**')`. The pathspec excludes vendored code from the start.
- [ ] **Step 3: Add the `asan-ubsan` job.**
  - Install the README packages, set `CXX=g++-12` (every CI job sets `CXX` explicitly from now on), configure with `cmake --preset asan`, build, and run `./build/asan/tests/unit_tests`. The job uses `options: --cap-add=SYS_PTRACE` (LeakSanitizer and, later, `strace` need it) and `BLC_TEST_TIME_SCALE=10`.
  - Switch `gtest_discover_tests` to `DISCOVERY_MODE PRE_TEST` in `tests/CMakeLists.txt`, so discovery never runs a sanitized binary at build time.
  - Make the existing `gcc-12` job run `unit_tests` as a non-root user (`useradd -m tester && su tester -c ./build/tests/unit_tests`), so permission tests execute.
  - Environment: `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.
  - If the sanitizer runtime aborts with "unexpected memory mapping" (high `vm.mmap_rnd_bits` on the runner kernel), run the test binary under `setarch "$(uname -m)" -R` and note why in the workflow.
- [ ] **Step 4: Push** and run `gh run watch`. Expected: all jobs green.
- [ ] **Step 5: Mark** S8, Q8 (partial) and F13 tests as in progress in requirements.md only if their evidence now exists. Commit — `git commit -m "CI: add clang-format and ASan/UBSan jobs"`

---

## Milestone M2 — Parsing

### Task 7: Vendor simdjson 4.6.11

**Files:**
- Create: `third_party/simdjson/simdjson.h`, `third_party/simdjson/simdjson.cpp`, `third_party/simdjson/LICENSE`, `third_party/simdjson/VERSION`, `third_party/simdjson/CMakeLists.txt`, `third_party/.clang-format` (`DisableFormat: true`, `SortIncludes: Never`)
- Modify: root `CMakeLists.txt` (`add_subdirectory(third_party/simdjson)` before `src`), `src/CMakeLists.txt` (`blc_core` links `simdjson`)

- [ ] **Step 1: Fetch** `singleheader/simdjson.h`, `singleheader/simdjson.cpp` and `LICENSE` from tag `v4.6.11` (`https://raw.githubusercontent.com/simdjson/simdjson/v4.6.11/...`). Write `VERSION` with `4.6.11` and the three files' SHA-256 values.
- [ ] **Step 2: Add the target.** `add_library(simdjson STATIC simdjson.cpp)`, include directory marked `SYSTEM`, `SIMDJSON_EXCEPTIONS=0` as a PUBLIC definition. Build it with the README command: `cmake -B build-r -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wall -Wextra -O2" && cmake --build build-r 2>&1 | grep -c "warning:"`. Expected: `0`. If it is not 0, add `target_compile_options(simdjson PRIVATE -w)` with a comment explaining that it is vendored code, then re-check.
- [ ] **Step 3: Add a smoke test** `tests/simdjson_smoke_test.cpp`: parse `{"a":1}` with On-Demand and expect `a == 1`. Run it: PASS.
- [ ] **Step 4: Commit** — `git commit -m "Vendor simdjson 4.6.11"`

### Task 8: Stream table and envelope split

**Files:**
- Create: `src/blc/parse/stream_table.hpp`, `src/blc/parse/stream_table.cpp`, `src/blc/parse/envelope.hpp`, `src/blc/parse/envelope.cpp`
- Test: `tests/parse_envelope_test.cpp`, `tests/parse_stream_table_test.cpp`

**Interfaces:**
- Consumes: `SymbolSpec`, `StreamKind`.
- Produces:
  - `struct StreamEntry { std::uint16_t symbol_index; StreamKind kind; };`
  - `class StreamTable`:
    - `StreamTable(const std::vector<SymbolSpec>& shard_symbols, std::uint16_t first_symbol_index);`
    - `std::optional<StreamEntry> find(std::string_view stream_name) const noexcept;`
    - `void append_stream_list(std::string& out) const;` — `"btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade/…"`.
    - Storage: a sorted vector with binary search over `string_view`.
  - `struct Envelope { std::string_view stream; std::string_view data; };`
  - `class EnvelopeSplitter`:
    - `EnvelopeSplitter();` — reserves a 1 MiB + padding scratch and an On-Demand parser.
    - `bool split(std::string_view frame, Envelope& out) noexcept;`
    - `std::uint64_t fallbacks() const noexcept;`
    - Exactly one splitter exists per process. It is owned by the capture app and used only on
      the io thread, which is single-threaded, so all `FrameRouter`s share it. Per-shard
      splitters would cost 64 × (1 MiB scratch + parser), outside the §9 budget.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Envelope, FastPath) { EnvelopeSplitter s; Envelope e;
  ASSERT_TRUE(s.split(R"({"stream":"btcusdt@trade","data":{"e":"trade","p":"1"}})", e));
  EXPECT_EQ(e.stream, "btcusdt@trade"); EXPECT_EQ(e.data, R"({"e":"trade","p":"1"})"); EXPECT_EQ(s.fallbacks(), 0u); }
TEST(Envelope, ReversedFieldsUseFallback) { EnvelopeSplitter s; Envelope e;
  ASSERT_TRUE(s.split(R"({"data":{"x":1},"stream":"btcusdt@trade"})", e));
  EXPECT_EQ(e.stream, "btcusdt@trade"); EXPECT_EQ(e.data, R"({"x":1})"); EXPECT_EQ(s.fallbacks(), 1u); }
TEST(Envelope, Malformed) { EnvelopeSplitter s; Envelope e;
  for (auto f : {"", "{}", R"({"stream":"a"})", R"({"stream":1,"data":{}})", R"({"stream":"a","data":[1]})",
                 R"({"stream":"a","stream":"b","data":{}})", R"({"stream":"a","data":{})", "not json"})
    EXPECT_FALSE(s.split(f, e)) << f; }
TEST(StreamTable, LookupAndList) { /* symbols BTCUSDT, ETHUSDT, first index 3:
  find("ethusdt@depth5@100ms") == {4, Depth5}; find("ethusdt@depth@100ms") == {4, DepthDiff};
  find("btcusdt@aggTrade") == nullopt; list ==
  "btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade/ethusdt@depth@100ms/ethusdt@depth5@100ms/ethusdt@trade" */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Envelope*:StreamTable*'`. Expected: FAIL.
- [ ] **Step 3: Implement** the fast path and fallback exactly as in §6.2. The fallback is one pass over the object's fields with key dispatch, each key at most once, and `data` taken through `raw_json()` (which must be an object). It returns views into the scratch copy.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add stream table and envelope splitter"`

### Task 9: Payload parser with decoded-size limits

**Files:**
- Create: `src/blc/parse/events.hpp`, `src/blc/parse/payload_parser.hpp`, `src/blc/parse/payload_parser.cpp`
- Create fixtures: `tests/fixtures/payloads/{spot,usdm}_{diff,depth5,trade,snapshot}.json` (real payloads; see Step 1)
- Test: `tests/parse_payload_test.cpp`

**Interfaces:**
- Consumes: `Level`, `PaddedView`, `Venue`, `parse_e8`.
- Produces:
  - `events.hpp`:
    - `struct DepthDiff { std::uint64_t first_id, last_id, prev_last_id; bool has_prev; std::vector<Level> bids, asks; };`
    - `struct Depth5 { std::uint64_t id; std::array<Level,5> bids, asks; std::uint8_t n_bids, n_asks; };`
    - `struct Trade { bool buyer_is_maker; };`
    - `struct DepthSnapshot { std::uint64_t last_update_id; std::vector<Level> bids, asks; };`
    - Level order is the payload order (best first for depth5 and snapshots).
  - `payload_parser.hpp`:
    - `enum class ParseStatus : std::uint8_t { Ok, Malformed, BadDecimal, SymbolMismatch, Oversized };`
    - `inline constexpr std::size_t kMaxDiffLevels = 20'000, kMaxSnapshotLevelsPerSide = 1'000, kMaxPayloadBytes = 1u << 20;`
    - `class PayloadParser`:
      - constructor: allocates parser capacity `kMaxPayloadBytes` and reserves scratch vectors;
      - `ParseStatus diff(Venue, PaddedView, std::string_view upper_symbol, DepthDiff&);`
      - `ParseStatus depth5(Venue, PaddedView, std::string_view upper_symbol, Depth5&);`
      - `ParseStatus trade(Venue, PaddedView, std::string_view upper_symbol, Trade&);`
      - `ParseStatus snapshot(Venue, PaddedView, DepthSnapshot&);`
      - `static DepthDiff make_diff();` and `static DepthSnapshot make_snapshot();` — return
        events whose `bids` and `asks` are each reserved to `kMaxDiffLevels`, because one side
        may hold every level, and to `kMaxSnapshotLevelsPerSide`. Every reused event is created
        through them.
  - Field rules:
    - spot diff: `U`, `u`, `b`, `a`, `s`;
    - usdm diff: plus `pu`;
    - spot depth5: `lastUpdateId`, `bids`, `asks` (no `s`);
    - usdm depth5: `u` is the id, `b`, `a`, `s`;
    - trade: `m`, `s`;
    - snapshot: `lastUpdateId`, `bids`, `asks`;
    - unknown keys are skipped, required keys must appear exactly once, and the document must reach `at_end()`.

- [ ] **Step 1: Capture fixtures** with a throwaway script (not committed): one real frame's `data` object per venue and kind from the live streams (BTCUSDT), plus `curl` depth snapshots with `limit=5` (small). Store them minified under `tests/fixtures/payloads/`.
- [ ] **Step 2: Write failing tests**

```cpp
TEST(Payload, SpotDiffFixture) { /* Ok; first_id/last_id match the file; has_prev false; bids[0] scaled exactly */ }
TEST(Payload, UsdmDiffHasPrev) { /* has_prev true, prev_last_id == fixture "pu" */ }
TEST(Payload, SpotDepth5IdIsLastUpdateId) { /* id == lastUpdateId; n_bids == 5 */ }
TEST(Payload, UsdmDepth5IdIsU) { /* id == fixture "u", even though e == "depthUpdate" */ }
TEST(Payload, TradeMaker) { /* spot fixture m:true -> buyer_is_maker true */ }
TEST(Payload, FieldOrderIndependent) { /* reorder keys of the spot diff -> identical DepthDiff */ }
TEST(Payload, Errors) { /* missing "u" -> Malformed; duplicate "U" -> Malformed; a diff level ["1e5","1"] -> BadDecimal;
  "s":"ETHUSDT" for BTCUSDT -> SymbolMismatch; level ["1"] -> Malformed; trailing "}x" -> Malformed;
  depth5 with 6 bids -> Malformed */ }
TEST(Payload, OversizedDiffRejectedNotTruncated) { /* 20 001 levels -> Oversized; 20 000 -> Ok with 20 000 */ }
TEST(Payload, OversizedSnapshot) { /* 1 001 bids -> Oversized */ }
TEST(Payload, ScratchNeverGrows) { /* capacity of out.bids before and after a 20 000-level diff is unchanged
  when the vector was constructed through PayloadParser::make_diff() */ }
```
- [ ] **Step 3: Run** `--gtest_filter='Payload*'`. Expected: FAIL.
- [ ] **Step 4: Implement.** Iterate the object once, dispatching on `unescaped_key`. Read level strings with `get_string()` into `parse_e8`. It un-escapes into the parser's pre-allocated buffer and returns a `string_view` with a length; `raw_json_string::raw()` has no length, so it is not used. Count levels while decoding and return `Oversized` at the cap. Clear vectors without shrinking them.
- [ ] **Step 5: Run.** Expected: PASS.
- [ ] **Step 6: Commit** — `git commit -m "Add payload parser with decoded-size limits and fixtures"`

---

## Milestone M3 — Order book

### Task 10: `BookSide`

**Files:**
- Create: `src/blc/book/book_side.hpp`
- Test: `tests/book_side_test.cpp`

**Interfaces:**
- Consumes: `Level`.
- Produces:
  - `enum class Side : std::uint8_t { Bid, Ask };`
  - `template <Side S> class BookSide`:
    - `explicit BookSide(std::size_t reserve = 5001);`
    - `void set(std::int64_t price, std::int64_t qty) noexcept;` — `qty == 0` erases; erasing an absent level is a no-op.
    - `std::size_t size() const noexcept;`
    - `const Level& best(std::size_t i) const noexcept;` — i-th best, `i < size()`.
    - `std::optional<std::int64_t> qty_at(std::int64_t price) const noexcept;`
    - `void clear() noexcept;`
    - `void load_best_first(const std::vector<Level>&) noexcept;`
    - `std::size_t count_at_or_better(std::int64_t boundary) const noexcept;`
    - `std::int64_t worst_price() const noexcept;`
    - `bool trim_if_over(std::size_t cap = 5000, std::size_t to = 4000) noexcept;` — returns whether it trimmed.
    - `static constexpr bool better(std::int64_t a, std::int64_t b);`
  - Storage is a vector sorted with the best at the back (bids ascending, asks descending), searched by galloping from the back.

- [ ] **Step 1: Write failing tests** — insert, overwrite and erase at the top, middle and bottom on both sides; erasing an absent level; `best(0..4)` ordering; `count_at_or_better`; trimming 5 001 → 4 000 keeps the best 4 000. `capacity()` is unchanged across 10⁴ random `set` calls when the side stays ≤ 5 001 (zero reallocation).
- [ ] **Step 2: Run** `--gtest_filter='BookSide*'`. Expected: FAIL.
- [ ] **Step 3: Implement** as in §5.1: galloping search, then `std::lower_bound` within the bracket, then `vector::insert` / `vector::erase`.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add flat-vector book side"`

### Task 11: `OrderBook` with coverage

**Files:**
- Create: `src/blc/book/order_book.hpp`, `src/blc/book/order_book.cpp`
- Test: `tests/book_coverage_test.cpp`

**Interfaces:**
- Consumes: `BookSide`, `DepthSnapshot`, `DepthDiff`.
- Produces:
  - `struct Coverage { bool complete; std::int64_t boundary; };`
  - `struct TopOfBook { std::array<Level,5> bids{}, asks{}; };` — missing levels are `{0,0}`.
  - `class OrderBook`:
    - `void load(const DepthSnapshot&, std::size_t limit = 1000);` — a side with fewer than `limit` levels is complete; otherwise the boundary is the worst snapshot price.
    - `void apply(LevelSpan bids, LevelSpan asks) noexcept;` — trims after every insertion and applies the §5.3 trimming rules (a complete side becomes bounded). It takes spans so that both a freshly parsed `DepthDiff` (via `span_of`) and a window entry (`DiffView`) apply without copying.
    - `std::size_t margin(Side) const;` — `SIZE_MAX` when complete.
    - `bool authoritative() const;`
    - `void top5(TopOfBook&) const;`
    - `void clear();`
    - `const Coverage& coverage(Side) const;`
    - `std::uint64_t id{};`
    - accessors `bids()` / `asks()`.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Coverage, ShortSnapshotIsComplete) { /* 3 bids with limit 1000 -> complete; top5 pads {0,0}; authoritative */ }
TEST(Coverage, FullSnapshotBounded) { /* 1000 bids 1000..1 -> boundary 1; margin 1000 */ }
TEST(Coverage, ExhaustionAfterDeletes) { /* snapshot bids 2000..1001 (1000 levels, boundary 1001); delete the 996 best -> margin 4
  -> !authoritative, even though a diff then adds bid 1000, below the boundary (it does not count) */ }
TEST(Coverage, TrimCompleteSideRevokesCompleteness) { /* complete side grows to 5001 via diffs -> trimmed to 4000,
  complete == false, boundary == worst retained; then deleting down to 4 levels -> !authoritative (no zero padding) */ }
TEST(Coverage, TrimBeyondBoundaryKeepsBoundary) { /* trimmed levels all worse than boundary -> boundary unchanged */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Coverage*'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add order book with snapshot coverage tracking"`

### Task 12: Sequence classifier and recent-diff window

**Files:**
- Create: `src/blc/book/sequence.hpp`, `src/blc/book/sequence.cpp`, `src/blc/book/diff_window.hpp`, `src/blc/book/diff_window.cpp`
- Test: `tests/book_sequence_test.cpp`, `tests/book_diff_window_test.cpp`

**Interfaces:**
- Produces:
  - `sequence.hpp`:
    - `enum class SeqClass { Stale, Gap, Continues };`
    - `SeqClass classify(Venue, std::uint64_t U, std::uint64_t u, std::uint64_t pu, std::uint64_t ref) noexcept;`
    - `bool contained_in_snapshot(Venue, std::uint64_t u, std::uint64_t L) noexcept;` — spot `u ≤ L`, usdm `u < L`.
    - `bool bridges(Venue, std::uint64_t U, std::uint64_t u, std::uint64_t L) noexcept;` — spot `U ≤ L+1 ≤ u`, usdm `U ≤ L ≤ u`.
  - `diff_window.hpp`:
    - `struct DiffView { std::uint64_t U, u, pu; LevelSpan bids, asks; };` and `DiffView view_of(const DepthDiff&) noexcept;`, which give one representation for live and windowed diffs (the depth5 checks and `OrderBook::apply` take it).
    - `class DiffWindow`:
      - `DiffWindow(std::size_t max_levels = 20'000, std::size_t max_diffs = 2'000);` — reserves both arenas.
      - `enum class Append { Appended, Stale, Reset };`
      - `Append append(Venue, const DepthDiff&) noexcept;`
      - `void restart_with(const DepthDiff&) noexcept;`
      - `void clear() noexcept;`
      - `std::size_t size() const noexcept;`
      - `DiffView at(std::size_t i) const noexcept;` — 0 is the oldest.
      - `std::uint64_t trimmed() const noexcept;`
    - The arenas are circular. When a new diff does not fit, the oldest diffs are dropped.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Sequence, SpotOverlapContinues) { EXPECT_EQ(classify(Venue::Spot, 103, 108, 0, 105), SeqClass::Continues); }
TEST(Sequence, SpotBoundaries) { EXPECT_EQ(classify(Venue::Spot, 106, 110, 0, 105), SeqClass::Continues);
  EXPECT_EQ(classify(Venue::Spot, 107, 110, 0, 105), SeqClass::Gap);
  EXPECT_EQ(classify(Venue::Spot, 100, 105, 0, 105), SeqClass::Stale); }
TEST(Sequence, UsdmUsesPu) { EXPECT_EQ(classify(Venue::Usdm, 120, 130, 105, 105), SeqClass::Continues);
  EXPECT_EQ(classify(Venue::Usdm, 120, 130, 104, 105), SeqClass::Gap); }
TEST(Sequence, Bridges) { EXPECT_TRUE(bridges(Venue::Spot, 101, 105, 100)); EXPECT_FALSE(bridges(Venue::Spot, 102, 105, 100));
  EXPECT_TRUE(bridges(Venue::Usdm, 95, 105, 100)); EXPECT_FALSE(bridges(Venue::Usdm, 101, 105, 100));
  EXPECT_TRUE(contained_in_snapshot(Venue::Spot, 100, 100)); EXPECT_FALSE(contained_in_snapshot(Venue::Usdm, 100, 100)); }
TEST(DiffWindow, OverlapAppendedNotReset) { /* [100,105] then [103,108] -> Appended, size 2 */ }
TEST(DiffWindow, GapResets) { /* then [110,112] -> Reset, size 1 */ }
TEST(DiffWindow, TrimsOldestByLevels) { /* 3 diffs of 9 000 levels each -> first dropped, trimmed()==1 */ }
TEST(DiffWindow, TrimsOldestByCount) { /* 2 001 one-level diffs -> size 2000 */ }
TEST(DiffWindow, WrapAroundPreservesContent) { /* after wrapping, at(i) levels equal inputs */ }
TEST(DiffWindow, ArenaKeepsDiffContiguous) { /* a diff that would straddle the arena end is stored contiguously
  (the arena wraps before it), so at(i).bids/asks are single LevelSpans */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Sequence*:DiffWindow*'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Classification against the window's last `u` applies the same `classify`.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add sequence classifier and recent-diff window"`

### Task 13: depth5 full and partial checks

**Files:**
- Create: `src/blc/book/depth5_check.hpp`, `src/blc/book/depth5_check.cpp`
- Test: `tests/book_depth5_check_test.cpp`

**Interfaces:**
- Consumes: `OrderBook`, `Depth5`, `DiffView`.
- Produces:
  - `enum class CheckResult { Verified, PartiallyVerified, Mismatch };`
  - `CheckResult full_check(const OrderBook&, const Depth5&) noexcept;`
  - `CheckResult partial_check(const OrderBook&, const Depth5&, const DiffView& d, std::size_t& levels_unchecked) noexcept;` — compares only prices inside the depth5 range ∩ coverage and not touched by `d` (§5.6).

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Depth5Check, FullMatchAndMismatch) { /* equal top -> Verified; one qty differs -> Mismatch; count differs -> Mismatch */ }
TEST(Depth5Check, PartialIgnoresTouchedPrices) { /* d touches price 10 with a different qty -> PartiallyVerified */ }
TEST(Depth5Check, PartialBeyondCoverageNotCompared) {
  // spec §5.6 example: trusted bids 10,9,8,7,6 (boundary 6); d deletes 10 and adds 11;
  // depth5 bids 9,8,7,6,5 -> PartiallyVerified, levels_unchecked == 1 (price 5)
}
TEST(Depth5Check, PartialDetectsUntouchedDifference) { /* untouched price 8 qty differs -> Mismatch */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Depth5Check*'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add depth5 full and coverage-aware partial checks"`

### Task 14: `SymbolBook` — sync state machine, `enter_unsynced`, row outcomes

**Files:**
- Create: `src/blc/book/symbol_book.hpp`, `src/blc/book/symbol_book.cpp`
- Test: `tests/book_sync_test.cpp`

**Interfaces:**
- Consumes: Tasks 10–13.
- Produces:
  - `enum class SyncState { Unsynced, Synced };`
  - `enum class UnsyncCause { NewEpoch, Gap, DiffParseError, Oversized, CoverageExhausted, Depth5Mismatch };`
  - `struct Emit { bool row = false; char type = 0; char side = 'N'; };` — the processor maps it to a CSV row.
  - `struct SymbolCounters { std::uint64_t stale_diffs, gaps, resets, syncs, resyncs, snapshot_too_old, coverage_exhausted, window_resets, depth5_verified, depth5_partially_verified, depth5_mismatch, depth5_stale, depth5_unchecked, depth5_levels_unchecked, parse_errors, oversized_events, epoch_regressions; };`
  - `enum class Depth5Policy { Resync, CountOnly };` and `constexpr Depth5Policy depth5_policy(Venue) noexcept;` — both venues `Resync` (§5.6, §15).
  - `class SymbolBook`:
    - `SymbolBook(Venue, SymbolCounters&);` delegates to `SymbolBook(Venue, SymbolCounters&, Depth5Policy);`. A default argument cannot name another parameter; the second constructor lets tests exercise `CountOnly`.
    - `enum class EpochCheck { Same, Reset, Regression };`
    - `EpochCheck observe_epoch(std::uint32_t epoch) noexcept;` — the last-seen epoch starts as none, so the first call returns `Reset`. The caller then runs `enter_unsynced(NewEpoch)` and requests a snapshot.
    - `Emit on_diff(const DepthDiff&) noexcept;`
    - `Emit on_diff_error(UnsyncCause) noexcept;` — `DiffParseError` or `Oversized`.
    - `Emit on_depth5(const Depth5&) noexcept;`
    - `Emit on_trade(const Trade&, bool trade_rows) noexcept;`
    - `Emit on_snapshot(const DepthSnapshot&) noexcept;` — clears the outstanding mark first.
    - `Emit on_snapshot_error() noexcept;` — a snapshot record that failed to parse or was `Oversized`: counts it, clears the outstanding mark, and wants a new snapshot if still Unsynced.
    - `bool snapshot_wanted() const noexcept;` — true only when a snapshot is needed **and** none is outstanding (§5.4 step 5).
    - `void mark_requested() noexcept;` — sets the outstanding mark. It is cleared only by `on_snapshot`, `on_snapshot_error` or an epoch reset.
    - `void enter_unsynced(UnsyncCause) noexcept;` — per the §5.7 table.
    - `SyncState state() const noexcept;`
    - `const OrderBook& book() const noexcept;`
  - The pending snapshot and pending depth5 check are owned copies, reserved at construction.

- [ ] **Step 1: Write failing tests** — one per bullet, built from small hand-written diffs and snapshots:

```cpp
TEST(Sync, SpotHappyPathEmitsR) { /* diffs [101,103],[104,106]; snapshot L=103 -> on_snapshot returns {row,'R','N'}; book.id == 106 */ }
TEST(Sync, SyncCompletedByLaterDiffEmitsROnThatDiff) { /* snapshot L=110 first (window has [101,105]) -> no row;
  then diff [106,112] -> {row,'R'} returned from on_diff */ }
TEST(Sync, SnapshotTooOldRefetch) { /* window [101..], L=90 -> snapshot_wanted(); snapshot_too_old==1 */ }
TEST(Sync, UsdmBridgeBoundary) { /* U<=L<=u accepted; U==L+1 rejected */ }
TEST(Sync, GapEntersUnsyncedAndDropsPendingCheck) { /* synced id 1000; depth5 p=1001 pending; diff U=1005 -> no row,
  Unsynced; after resync to 1020 the next diff does not trigger a depth5 check (depth5_mismatch==0) */ }
TEST(Sync, StaleDiffWhileSyncedEmitsD) { /* u <= id -> {row,'D'}, book unchanged, stale_diffs==1 */ }
TEST(Sync, DiffSideLetter) { /* bids only 'B', asks only 'S', both 'N', neither 'N' */ }
TEST(Sync, TradeSideIsMakerSide) { /* buyer_is_maker -> 'B'; else 'S'; trade_rows=false -> no row */ }
TEST(Sync, CoverageExhaustionNoRow) { /* diff leaves margin 4 -> no row, Unsynced, coverage_exhausted==1 */ }
TEST(Sync, Depth5MismatchResyncs) { /* p==id with a different qty -> no row, Unsynced */ }
TEST(Sync, Depth5PendingResolutions) { /* p<U full-before; U<=p<u partial; p==u full-after; p>u keeps pending */ }
TEST(Sync, EpochFirstRecordResetsAndRequests) { /* observe_epoch(0) -> Reset; snapshot_wanted() */ }
TEST(Sync, EpochRegressionCounted) { /* observe 2 then 1 -> Regression, epoch_regressions==1 */ }
TEST(Sync, UnsyncedIgnoresDepth5AndTrades) { /* no rows */ }
TEST(Sync, DiffParseErrorIsGap) { /* synced; on_diff_error(DiffParseError) -> Unsynced, window cleared, parse_errors==1 */ }
TEST(Sync, CoverageExhaustionDropsPendingCheck) { /* p>id pending; the next diff exhausts coverage -> Unsynced, no depth5 check counted */ }
TEST(Sync, NoDuplicateRequestWhileOutstanding) { /* Unsynced, mark_requested(); a diff parse error -> snapshot_wanted()==false;
  on_snapshot(too old) -> snapshot_wanted()==true */ }
TEST(Sync, SnapshotErrorRerequests) { /* mark_requested(); on_snapshot_error() -> snapshot_wanted()==true */ }
TEST(Sync, CountOnlyPolicyDoesNotResync) { /* Depth5Policy::CountOnly; mismatch -> depth5_mismatch==1, still Synced, row emitted */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Sync*'`. Expected: FAIL.
- [ ] **Step 3: Implement** §5.4, §5.6 and §5.7 literally. `try_sync` searches the window, so it never consumes it. Every route into Unsynced goes through `enter_unsynced`.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add per-symbol sync state machine"`

### Task 15: Exchange-simulator property test

**Files:**
- Create: `tests/support/exchange_sim.hpp`, `tests/support/exchange_sim.cpp`
- Test: `tests/book_property_test.cpp`

**Interfaces:**
- Produces (test support only):
  - `class ExchangeSim`:
    - `ExchangeSim(Venue, std::uint32_t seed);`
    - `struct Step { enum Kind { Diff, Depth5, Trade, Snapshot, Reconnect } kind; DepthDiff diff; Depth5 d5; DepthSnapshot snap; };`
    - `Step next();` — random walk around a mid price, with deep resting levels that are never touched.
    - `TopOfBook true_top() const;`
    - `void inject_gap();` — the next diff skips ids. `void inject_reconnect();` — the next step is `Reconnect`.
    - `bool coverage_sufficient(const DepthSnapshot& served) const;` — an independent re-statement of §5.3 from the simulator's own data. Each side of the served snapshot is complete if it held fewer than 1 000 levels. Otherwise its boundary is the snapshot's worst price, and the side is sufficient only if the **true** book currently has ≥ 5 levels at or better than that boundary. Diff-created levels beyond the boundary never count.
    - The simulator keeps each true side ≤ 4 000 levels, so trimming never occurs here (it is unit-tested in Task 11).
  - Snapshots are truncated to 1 000 levels. usdm ids skip values, and `pu` chains. Gaps and reconnects are injected at random (about 1 per 2 000 steps).

- [ ] **Step 1: Write the test.** For seeds 1..20 and both venues, run 10⁴ steps each, feeding `SymbolBook` and requesting snapshots when `snapshot_wanted()`. The simulator answers snapshot requests two steps later. Assert after every step:
  - **No wrong output:** if a row was emitted, `book.top5()` equals `sim.true_top()`.
  - **Exhaustion matches the oracle:** the book enters Unsynced with cause `CoverageExhausted` at step *k* exactly when `coverage_sufficient(last served snapshot)` turns false at *k*.
  - **No needless suppression:** a market record may go without a row only while the book is in an *unsynced interval*. Such an interval opens at the start or at any `enter_unsynced` (any cause) and closes when the book is Synced again. Any row withheld outside an interval fails the test, with the seed and step number.
  - **Liveness:** within each interval, once a snapshot has been served, the book is Synced at the first diff that bridges it. The simulator knows which diff that is, regardless of how many depth5 or trade steps come in between.
  - **Consistency:** `depth5_mismatch == 0` across all seeds, and `syncs == 1 + (number of Synced → Unsynced transitions)` per run. Gaps or reconnects injected while the book is already Unsynced add no transition.
- [ ] **Step 2: Run** `--gtest_filter='BookProperty*'`. Expected: PASS. A failure is a real bug: fix it in Tasks 10–14 code with a regression test, never in the simulator.
- [ ] **Step 3: Commit** — `git commit -m "Add exchange-simulator property test for the book"`

---

## Milestone M4 — Pipeline and replay

### Task 16: SPSC byte ring (built and tested alone) and the TSan job

**Files:**
- Create: `src/blc/pipeline/record.hpp`, `src/blc/pipeline/spsc_ring.hpp`, `src/blc/pipeline/spsc_ring.cpp`
- Modify: `.github/workflows/ci.yml` (add the `tsan` job: `cmake --preset tsan`, run `unit_tests`)
- Test: `tests/pipeline_spsc_ring_test.cpp`

**Interfaces:**
- Consumes: `StreamKind`, `PaddedView`.
- Produces:
  - `enum class RecordKind : std::uint8_t { Market, Snapshot };`
  - `struct RecordHeader { RecordKind kind; StreamKind stream; std::uint16_t symbol; std::uint16_t shard; std::uint32_t epoch; std::uint64_t conn_seq; std::int64_t recv_ns; std::int64_t steady_ns; std::uint32_t payload_len; };`
  - `struct RecordView { RecordHeader header; PaddedView payload; };`
  - `class SpscRing`:
    - `explicit SpscRing(std::size_t capacity_pow2);`
    - `static std::size_t capacity_for_symbols(std::size_t n);` — 16 / 32 / 64 MiB.
    - `bool try_push(const RecordHeader&, std::string_view payload) noexcept;`
    - `bool try_peek(RecordView&) noexcept;`
    - `void pop() noexcept;`
    - `void close_producer() noexcept;`
    - `bool producer_closed() const noexcept;`
    - `void wait(std::chrono::milliseconds) noexcept;` — spins, then parks.
    - `std::size_t high_water() const noexcept;`
  - Records are 64-byte aligned and followed by `SIMDJSON_PADDING`; a wrap marker is used when needed (§6.4).

- [ ] **Step 1: Write failing tests**

```cpp
TEST(SpscRing, CapacityBands) { EXPECT_EQ(SpscRing::capacity_for_symbols(1), 16u<<20);
  EXPECT_EQ(SpscRing::capacity_for_symbols(65), 32u<<20); EXPECT_EQ(SpscRing::capacity_for_symbols(256), 64u<<20); }
TEST(SpscRing, WrapCases) { /* 4 KiB ring: records that end exactly at the end, need a wrap marker,
  and are larger than half the ring; payload bytes and padding readable after each */ }
TEST(SpscRing, FullRejectsWithoutCorruption) { /* fill; try_push false; pop one; push succeeds; FIFO intact */ }
TEST(SpscRing, CloseProtocol) { /* consumer drains everything pushed before close_producer(), then sees closed */ }
TEST(SpscRing, RandomLengthFuzz) { /* 10^5 random lengths 0..3000, single thread, round-trip equal */ }
TEST(SpscRing, TwoThreadStress) { /* producer pushes 10^6 sequence-numbered records with random lengths;
  consumer verifies order and content; run under TSan in CI */ }
```

- [ ] **Step 2: Run** `--gtest_filter='SpscRing*'`. Expected: FAIL.
- [ ] **Step 3: Implement** as in §6.4: monotonically increasing 64-bit positions, cache-line-separated atomics, cached opposite positions, and a parked flag plus a condition variable. The producer notifies only when the consumer is parked.
- [ ] **Step 4: Run** locally (MSYS2 has no TSan), then push. The CI `tsan` job is the gate until WSL exists; once it does, also run `cmake --preset tsan && cmake --build build/tsan --target unit_tests && ./build/tsan/tests/unit_tests --gtest_filter='SpscRing*'`. Expected: PASS, no TSan reports.
- [ ] **Step 5: Commit** — `git commit -m "Add SPSC byte ring; CI: TSan job"`

### Task 17: `FrameRouter` with the loss transition

**Files:**
- Create: `src/blc/pipeline/frame_router.hpp`, `src/blc/pipeline/frame_router.cpp`, `src/blc/pipeline/metrics.hpp`
- Test: `tests/pipeline_frame_router_test.cpp`

**Interfaces:**
- Consumes: `StreamTable`, `EnvelopeSplitter`, `SpscRing`, `RecordHeader`.
- Produces:
  - `metrics.hpp`:
    - `struct IoMetrics` — relaxed atomics: `frames_received`, `bytes_received`, `malformed_frames`, `ring_drops`, `shard_losses`, `snapshots_discarded`, `connects`, `reconnects`, `connect_failures`, `connect_throttled`, `rest_requests`, `rest_failures`, `rest_cooldowns`, `rest_weight_used`.
    - `struct EpochStats { std::uint64_t frames, malformed, drops; };`
  - `class FrameRouter`:
    - `FrameRouter(std::uint16_t shard, const StreamTable&, EnvelopeSplitter& shared, SpscRing&, IoMetrics&);` — the splitter is the process-wide one (Task 8).
    - `void on_connected(std::uint32_t epoch) noexcept;`
    - `enum class FrameResult { Pushed, Unsubscribed, LostNow, Withheld };`
    - `FrameResult on_frame(std::string_view frame, std::int64_t recv_ns, std::int64_t steady_ns) noexcept;`
    - `bool lost() const noexcept;`
    - `void on_disconnected() noexcept;` — called by `WsShard` on any read error, close or loss. It clears the `established` flag.
    - `bool can_deliver_snapshot(std::uint32_t epoch) const noexcept;` — `epoch` is current, the connection is established, and there has been no loss (§6.3).
    - `bool push_snapshot(std::uint16_t symbol, std::uint32_t epoch, std::string_view body, std::int64_t recv_ns) noexcept;` — false if the ring is full.
    - `EpochStats close_epoch() noexcept;` — the caller logs the `epoch_closed` line.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(FrameRouter, AssignsConnSeqAndPushes) { /* 3 frames -> conn_seq 0,1,2 in ring records */ }
TEST(FrameRouter, UnsubscribedCountedNotLost) { /* valid envelope, unknown stream -> Unsubscribed, conn_seq consumed */ }
TEST(FrameRouter, InvalidEnvelopeIsLoss) { /* -> LostNow; next frame -> Withheld; ring unchanged; drops counted */ }
TEST(FrameRouter, RingFullIsLoss) { /* tiny ring; push fails -> LostNow; can_deliver_snapshot(epoch) == false */ }
TEST(FrameRouter, NoDeliveryAfterDisconnect) { /* on_connected(0); on_disconnected(); can_deliver_snapshot(0) == false;
  on_connected(1) -> can_deliver_snapshot(1) == true, can_deliver_snapshot(0) == false */ }
TEST(FrameRouter, NewConnectionClearsLoss) { /* on_connected(1) -> not lost; conn_seq restarts at 0 */ }
TEST(FrameRouter, AccountingInvariant) { /* frames == pushed + malformed + drops for the closed epoch */ }
TEST(FrameRouter, ReversedEnvelopeSameRecord) { /* {"data":…,"stream":…} and the forward form of the same frame push
  records with identical header fields and payload bytes */ }
```

- [ ] **Step 2: Run** `--gtest_filter='FrameRouter*'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add frame router with shard loss transition"`

### Task 18: Output files — naming, archive rule, per-symbol output set

**Files:**
- Create: `src/blc/pipeline/output_files.hpp`, `src/blc/pipeline/output_files.cpp`
- Test: `tests/pipeline_output_files_test.cpp`

**Interfaces:**
- Consumes: `OutputFile`, `CsvRow`, schema constants, `utc_date`, `utc_compact_stamp`.
- Produces:
  - `struct FileNames { std::string market_data, orderbook, snapshots; };`
  - `FileNames file_names(Venue, std::string_view upper_symbol, std::string_view date);`
    - → `market_data_spot_BTCUSDT_2026-10-11.csv`
    - → `market_data_spot_BTCUSDT_2026-10-11_orderbook.csv`
    - → `depth_snapshots_spot_BTCUSDT_2026-10-11.csv`
  - `std::filesystem::path prepare_output_dir(const std::filesystem::path&);` — creates the directory, checks it is writable, throws `IoError` naming the path.
  - `std::optional<std::filesystem::path> archive_existing(const std::filesystem::path& dir, const std::vector<std::string>& names, std::string_view run_stamp);` — moves every existing name into one `replaced_<stamp>[-k]/` directory. It is called **once per run** with the names of all symbols, so a run never creates more than one archive directory.
  - `class SymbolFiles` — owns three `OutputFile`s and their headers. The market-data and order-book files each get a 64 KiB buffer.
    - The sidecar `OutputFile` is built over the shared 64 KiB scratch passed in. It is flushed right after its header is written, and `SymbolFiles::end_snapshot_row()` flushes it as soon as each row ends. A chunked row longer than 64 KiB is written out chunk by chunk by `OutputFile::write`.
    - The processor writes one sidecar row at a time, on one thread. The shared scratch is therefore always empty when another symbol's row starts, so rows of different symbols can never mix (§6.6).

- [ ] **Step 1: Write failing tests**

```cpp
TEST(OutputFiles, Names) { /* exactly the three names above */ }
TEST(OutputFiles, ArchiveWholeSetKeepsNames) { /* create the three files; archive -> replaced_20261011T120000Z/ holds all three
  under original names; dir no longer has them */ }
TEST(OutputFiles, ArchivePartialSetAndCollision) { /* only the orderbook exists -> moved; second archive same stamp -> "-1" suffix */ }
TEST(OutputFiles, OutputDirIsAFile) { /* prepare_output_dir(existing regular file) throws IoError containing the path */ }
TEST(OutputFiles, OutputDirNotWritable) { /* chmod 0555 dir (POSIX only; GTEST_SKIP when euid==0 — the non-root CI job runs it) -> IoError */ }
TEST(OutputFiles, SidecarHeadersSurviveSharedScratch) { /* two symbols with different venue/symbol in their paths: create both, then write a
  70 KiB row to the second -> both files start with their exact header */ }
TEST(OutputFiles, HeadersWritten) { /* new SymbolFiles -> each file's first line is its schema header + "\n" */ }
TEST(OutputFiles, ArchiveOncePerRunForAllSymbols) { /* BTC and ETH sets exist; one archive_existing call with all six names
  -> exactly one replaced_<stamp>/ holding all six */ }
TEST(OutputFiles, SharedSidecarScratchKeepsRowsSeparate) { /* two SymbolFiles over one 64 KiB scratch; write a ~60 KB row
  for BTC then for ETH without an intervening timer flush -> each sidecar file holds exactly its own row */ }
```

- [ ] **Step 2: Run** `--gtest_filter='OutputFiles*'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Moves use `std::filesystem::rename`; the directory is in the same filesystem.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add output file naming, archiving and per-symbol files"`

### Task 19: `Processor`, request slots, metrics output, loss integration and allocation tests

**Files:**
- Create: `src/blc/pipeline/snapshot_requester.hpp`, `src/blc/pipeline/snapshot_requester.cpp`, `src/blc/pipeline/processor.hpp`, `src/blc/pipeline/processor.cpp`, `src/blc/pipeline/row_format.hpp`, `src/blc/pipeline/row_format.cpp`
- Test: `tests/pipeline_processor_test.cpp`, `tests/pipeline_loss_integration_test.cpp`, `tests/pipeline_allocation_test.cpp`, `tests/support/counting_allocator.cpp`, `tests/support/loss_scenario.hpp`, `tests/support/loss_scenario.cpp` (`void write_loss_scenario(const std::filesystem::path& dir);`, shared with Task 21)

**Interfaces:**
- Consumes: everything in M1–M4 so far.
- Produces:
  - `snapshot_requester.hpp`:
    - `class SnapshotRequester { public: virtual ~SnapshotRequester() = default; virtual void request(std::uint16_t symbol, std::uint32_t epoch) noexcept = 0; };`
    - `class RequestSlots final : public SnapshotRequester` — one `std::atomic<std::uint64_t>` per symbol holding `(epoch << 32) | request_number`; request numbers start at 1. `template <class F> void scan(F&& on_changed);` is for the io thread.
    - `class NullRequester final : public SnapshotRequester`.
  - `class SymbolOutput` (interface, `processor.hpp`):
    - `virtual void market_row(const RecordView&, std::string_view payload_minified) = 0;`
    - `virtual void snapshot_row(const RecordView&, std::size_t apply_before_row) = 0;`
    - `virtual void book_row(std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, Emit, const TopOfBook&) = 0;`
    - `virtual std::size_t market_rows() const = 0;`
    - `virtual bool failed() const = 0;`
    - Implementations: `FileOutput` (capture: all three files) and `BookOnlyOutput` (replay).
  - `struct ProcessorConfig { Venue venue; std::vector<SymbolSpec> symbols; bool trade_rows = true; };`
  - `class Processor`:
    - `Processor(ProcessorConfig, std::vector<std::unique_ptr<SymbolOutput>>, SnapshotRequester&);`
    - `void process(const RecordView&) noexcept;`
    - `bool fatal() const noexcept;` — read only on the processor thread. The consumer loop (Task 20) publishes it to the io thread through an atomic.
    - `void flush() noexcept;`
    - `bool finish() noexcept;` — flush and close, checked.
    - `const SymbolCounters& counters(std::size_t symbol) const;`
    - `ProcessorStats stats() const;` — processor counters summed over symbols, `retained_bytes()` (the capacity sum of every processor and per-symbol container), and the latency histogram.
  - `class LatencyHistogram` — fixed log2 buckets (64), `void add(std::int64_t ns) noexcept`, `std::int64_t percentile(double p) const noexcept` (bucket upper bound). The processor adds `wall steady now − header.steady_ns` per record after its row is written; replay records carry no steady time and add nothing.
  - `std::string format_stats(const ProcessorStats&, const IoMetrics* io, std::size_t ring_hw_bytes, std::int64_t uptime_s);` — the §8 `key=value` line. `io == nullptr` (replay) omits io keys.
  - `row_format.hpp`: `void write_book_row(CsvRow&, std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, char type, char side, const TopOfBook&);` — 26 columns.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Processor, MarketRowBeforeParseEvenOnParseError) { /* malformed trade payload -> market row written, no book row */ }
TEST(Processor, BookRowFormat) { /* synced book: the R row has seqNo 0; the following P record -> row "tsec,tnsec,1,2014473836,P,N,<20 ints>" with exactly 25 commas */ }
TEST(Processor, SeqNoPerSymbol) { /* two symbols interleaved -> each file's seqNo 0,1,2 */ }
TEST(Processor, SidecarApplyBeforeRow) { /* 3 market rows then a snapshot -> sidecar apply_before_row == 3 */ }
TEST(Processor, RequestsThroughSlots) { /* first record -> RequestSlots holds (0,1); after a snapshot record and a gap -> (0,2) */ }
TEST(Processor, PerSymbolEpochIsolation) { /* BTC epoch-1 record does not reset ETH; ETH resets at its own epoch-1 record */ }
TEST(Processor, WriterFailureSetsFatal) { /* SymbolOutput reporting failed() -> fatal(), later records ignored */ }
TEST(Processor, PrettyPayloadMinified) { /* payload with spaces and newlines -> the market row holds the one-line minified
  form; payloads_minified==1; parsing still succeeds */ }
TEST(Processor, StatsLineKeys) { /* format_stats with an IoMetrics contains every §8 key: frames md_rows ob_rows parse_errors gaps resyncs
  reconnects coverage_exhausted depth5_verified depth5_mismatch ring_drops ring_hw_kib retained_kib lat_p50_us lat_p99_us */ }
TEST(LatencyHistogram, Percentiles) { /* 1..1000 us uniform -> p50 within its bucket bounds of 500 us; p99 >= p50; empty -> 0 */ }
TEST(Processor, RetainedBytesStable) { /* retained_bytes() equals its value at construction after 10^4 records (nothing grows) */ }
TEST(LossIntegration, DroppedDiffThenTradeEmitsNoStaleRow) {
  // spec §13 'loss' scenario via write_loss_scenario(tmp): FrameRouter -> tiny SpscRing -> Processor; ring full when a diff
  // deleting the best bid arrives; then a trade frame; expect the trade Withheld, no book row containing the old
  // best bid; after on_connected(1) + new frames the symbol resyncs. The replay half of this scenario is in Task 21.
}
TEST(Allocations, ProcessorPathAllocatesNothing) {
  // counting operator new armed right after Processor construction (no warm-up);
  // feed scenario-style records covering sync, gap, resync, coverage exhaustion, trimming, depth5 checks;
  // EXPECT_EQ(allocations, 0)
}
```

- [ ] **Step 2: Run** `--gtest_filter='Processor*:LossIntegration*:Allocations*'`. Expected: FAIL.
- [ ] **Step 3: Implement** the §6.5 steps. The minify scratch is 1 MiB. `tests/support/counting_allocator.cpp` replaces the global `operator new`/`delete` and installs `set_log_allocation_hook` (Task 5), so log formatting is not counted.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add processor, request slots, loss and allocation tests"`

### Task 20: Consumer loop and the shutdown suite

**Files:**
- Create: `src/blc/pipeline/consumer.hpp`, `src/blc/pipeline/consumer.cpp`
- Test: `tests/pipeline_consumer_test.cpp`

**Interfaces:**
- Consumes: `SpscRing` (Task 16), `Processor` (Task 19).
- Produces:
  - `struct ConsumerConfig { std::function<std::int64_t()> steady_now_ns; std::chrono::milliseconds flush_every{1000}; std::chrono::seconds stats_every{10}; std::function<void(const std::string&)> on_stats; };` — the clock is injectable, so tests don't sleep. `stats_every == 0s` means no periodic line, only the final summary (`--stats-interval 0`).
  - `int run_consumer(SpscRing&, Processor&, std::atomic<int>& fatal_code, const ConsumerConfig&) noexcept;` — the processor thread's body, implementing §6.7 steps 4–5:
    - peek → `process` → `pop`;
    - when the ring is empty, `ring.wait(50 ms)`;
    - flush and stats on schedule;
    - on `processor.fatal()`, store `1` into `fatal_code` (release) exactly once, then drain and discard;
    - when the ring is empty and `producer_closed()` is seen, drain once more, then `finish()`; a failed finish stores `1`.
  - It returns the value of `fatal_code` at exit. `fatal_code` is the only fatal state the io thread reads (§6.1).

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Consumer, CloseWithEmptyRingTerminates) { /* close_producer() before any push -> returns 0 promptly; finish() called once */ }
TEST(Consumer, CloseWithFullRingDrainsEverything) { /* fill a small ring, close_producer(), start consumer
  -> every record processed (count matches), returns 0 */ }
TEST(Consumer, WriterFailurePublishesFatalAndDrains) { /* failing SymbolOutput after record 3 -> fatal_code==1,
  records 4..N popped but not written, consumer returns 1 after close */ }
TEST(Consumer, FinishFailureReturnsOne) { /* close() fails at finish -> 1 */ }
TEST(Consumer, PeriodicFlushAndStatsFollowInjectedClock) { /* advance the fake clock 1 s -> flush; 10 s -> one stats line */ }
TEST(Consumer, ConcurrentProducerUnderTsan) { /* producer thread pushes 10^5 records then closes; consumer processes all; TSan clean */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Consumer*'`. Expected: FAIL.
- [ ] **Step 3: Implement** `run_consumer`.
- [ ] **Step 4: Run.** Expected: PASS (the TSan job runs the concurrent test in CI).
- [ ] **Step 5: Commit** — `git commit -m "Add consumer loop with shutdown and fatal-error handling"`

### Task 21: Replay mode and `blc_verify`

**Files:**
- Create: `src/blc/replay/replay.hpp`, `src/blc/replay/replay.cpp`, `tools/blc_verify.cpp`, `tools/CMakeLists.txt`
- Modify: `src/main.cpp` (dispatch `Mode::Replay` to `run_replay`), root `CMakeLists.txt` (`add_subdirectory(tools)`)
- Test: `tests/replay_test.cpp`

**Interfaces:**
- Consumes: `CsvReader`, `Processor`, `BookOnlyOutput`, `NullRequester`, `ReplayOptions`.
- Produces:
  - `int run_replay(const ReplayOptions&);` — returns 0, 1 or 2 per §7.
    - Venue and symbol come from the file name `market_data_<venue>_<SYMBOL>_<date>.csv`; every row must agree, or that file fails.
    - A header-only input replays to a header-only order book.
    - Stats lines print every `stats_interval_s` during long replays, and a per-file final summary (`format_stats` with `io == nullptr`) prints at the end.
  - `blc_verify <file>...` detects the file type from its header and exits 0 or 1. It streams through `CsvReader`, so memory stays bounded. Checks:
    - **All files:** exact header, column count on every row, full RFC 4180 parse, `tnsec`/`recv_tnsec` in [0, 999 999 999].
    - **Market data:** `venue` ∈ {spot, usdm}; `stream_kind` ∈ {depth_diff, depth5, trade}; `symbol` uppercase `[A-Z0-9]{2,20}` and constant per file; `payload_json` one line (no CR/LF) and valid JSON; `conn_seq` strictly increasing within each (shard, epoch); epochs non-decreasing.
    - **Order book:** `seqNo` contiguous from 0; `id` constant; `type` ∈ {R,D,P,T}; `side` ∈ {B,S,N}; the first row is `R`; non-zero bids strictly descending and asks strictly ascending; zero padding only after the last non-zero level, with price and size both 0.
    - **Sidecar:** `apply_before_row` non-decreasing and ≤ the paired market-data file's row count (when that file is given too).
    - **With `--epoch-log <stderr-log>`:** the frame-accounting invariant per (shard, epoch) against the `epoch_closed` lines.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Replay, GlobSkipsOrderbookFiles) { /* inputs {market, orderbook} -> orderbook skipped (info), exit 0, one output */ }
TEST(Replay, OnlyOrderbookFilesIsUsageError) { /* -> 2 */ }
TEST(Replay, FailingFileDoesNotStopOthers) { /* bad header + good file -> good output written, exit 1 */ }
TEST(Replay, MissingSidecarFails) { /* -> exit 1, message names the sidecar */ }
TEST(Replay, NonMonotonicApplyBeforeRowRejected) { /* -> exit 1 */ }
TEST(Replay, CrlfInputSameOutput) { /* converting a fixture to CRLF and dropping the final newline -> byte-identical output */ }
TEST(Replay, ExistingOutputArchived) { /* second replay into the same dir -> replaced_<stamp>/ created */ }
TEST(Replay, LossIntegrationReplaysIdentically) { /* tests/support/loss_scenario.{hpp,cpp}: write_loss_scenario(dir) runs the Task 19 loss
  pipeline into dir (shared with LossIntegration, so neither test depends on the other's files); replaying dir -> byte-identical order book */ }
TEST(Replay, SkipsByOrderbookHeader) { /* a file named market_data_x.csv whose header is the order-book header -> skipped with an info line */ }
TEST(Replay, HeaderOnlyInput) { /* -> header-only output, exit 0 */ }
TEST(Replay, ArchivedRunReplaysFromArchiveDir) { /* built at test time, not as a fixture (git ignores replaced_*/):
  write run 1's three files, archive_existing them as a second run would, then replay
  replaced_<stamp>/market_data_*.csv -> equals run 1's order book */ }
TEST(Verify, RejectsEachViolation) { /* blc_verify returns 1 for: wrong header, 25 columns, seqNo gap, type 'X', side 'Q',
  bids not descending, payload with a newline, lower-case symbol, conn_seq not increasing; 0 for a valid fixture */ }
```

- [ ] **Step 2: Run** `--gtest_filter='Replay*'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Payloads are copied into a reusable padded buffer before parsing.
- [ ] **Step 4: Run.** Expected: PASS. Also run `./build/debug/binance_capture --replay nothing.csv`: expect exit 1 and a message naming the file.
- [ ] **Step 5: Commit** — `git commit -m "Add replay mode and blc_verify tool"`

### Task 22: Scenario fixtures, output-contract and Valgrind CI jobs

**Files:**
- Create, for each scenario below: `tests/scenarios/<name>/input/` (one or more `market_data_*.csv` with their `depth_snapshots_*.csv`); `tests/scenarios/<name>/expected/<input stem>_orderbook.csv`, one per input file; `tests/scenarios/<name>/README.md`
- Create: `tests/scenarios_test.cpp`, `scripts/verify_outputs.sh`
- Modify: `.github/workflows/ci.yml` (jobs `output-contract`, `valgrind`)

**Interfaces:**
- Consumes: `run_replay`, `blc_verify`.

Scenarios (each `README.md` explains how its expected rows were derived by hand):

| Scenario | What it pins |
|---|---|
| `spot_sync_overlap` | Bridge `U ≤ L+1 ≤ u`, an overlapping buffered diff, the first row `R` |
| `usdm_bridge_boundary` | `U ≤ L ≤ u` accepted at both edges |
| `gap_resync` | Gap → no rows → new `R` |
| `two_symbols_one_shard_reconnect` | Each file replayed alone gives its expected output |
| `coverage_exhaustion` | Rows stop when the margin drops below 5 |
| `depth5_mismatch` | Mismatch → resync |
| `depth5_beyond_coverage` | The §5.6 example: no false mismatch |
| `oversized_diff` | 20 001-level diff → gap, never truncated |
| `sync_by_later_diff` | `R` row timestamped with the diff |
| `trade_side` | `m` → `B`/`S` |

Two scenarios are deliberately not file fixtures. The reversed envelope never reaches a CSV, so it is covered by `FrameRouter.ReversedEnvelopeSameRecord` (Task 17) and by the loopback server (Task 27). Archives are covered by `Replay.ArchivedRunReplaysFromArchiveDir` (Task 21), because git ignores `replaced_*/`.

- [ ] **Step 1: Write `tests/scenarios_test.cpp`.** For each scenario directory, replay every file in `input/` into a temp dir. Each produced `<stem>_orderbook.csv` must equal, byte for byte, the file of the same name in `expected/`, and the two directories must hold the same set of files.
- [ ] **Step 2: Author the scenarios one by one.** Per scenario: write the inputs, write the expected rows by hand, run `--gtest_filter='Scenarios*'`, and fix code (never expectations) until it passes.
- [ ] **Step 3: Add CI.**
  - `output-contract` (installs `strace`; `options: --cap-add=SYS_PTRACE`; `CXX=g++-12`):
    - build Release;
    - run scenario replays through the `market_data_*.csv` glob;
    - run `blc_verify` on all outputs;
    - run `strace -f -e trace=socket -o strace.log ./build/binance_capture --replay ...`, then `! grep -q 'socket(' strace.log`;
    - run the secrets grep with the pattern assembled in the script from fragments. Every step uses `set -euo pipefail` and `grep -q`, never `grep -c`, whose exit status 1 on zero matches breaks `-e`.
    - run `scripts/verify_outputs.sh` on every replay output (exact headers, `awk -F',' 'NR==2{print NF}'` = 26, `blc_verify`);
    - check that the `-E` reading of the secrets pattern (assembled at runtime) matches nothing outside `docs/`, `README.md` and `.gitignore`, so the README's claim stays true.
  - `valgrind` (installs `valgrind`): `valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect` over the scenario replays.
- [ ] **Step 4: Push** and run `gh run watch`. Expected: green.
- [ ] **Step 5: Update requirements.md** rows whose evidence now exists (F5, F8–F16 consistency/correctness parts, S3 partial, Q4). Commit — `git commit -m "Add hand-derived scenario fixtures; CI: output contract and Valgrind"`

---

## Milestone M5 — Networking (WSL2 Ubuntu 22.04 required from here)

### Task 23: Endpoints, sharding, URL limits, attempt limiter

**Files:**
- Create: `src/blc/net/endpoints.hpp`, `src/blc/net/endpoints.cpp`, `src/blc/net/attempt_limiter.hpp`, `src/blc/net/attempt_limiter.cpp`; `src/CMakeLists.txt` adds `blc_net` (static; `find_package(Boost 1.74 REQUIRED)`, `find_package(OpenSSL REQUIRED)`, `Threads`)
- Test: `tests/net_endpoints_test.cpp`, `tests/net_attempt_limiter_test.cpp`

**Interfaces:**
- Produces:
  - `struct Endpoint { std::string host; std::string port; std::string target_prefix; };`
  - `struct VenueEndpoints { Endpoint ws; Endpoint rest; std::string depth_path; std::string ticker_path; };`
  - `VenueEndpoints default_endpoints(Venue);` — values from §6.2/§6.3.
  - `std::vector<std::vector<std::uint16_t>> make_shards(std::size_t n_symbols, int per_connection);`
  - `std::string ws_target(const VenueEndpoints&, const StreamTable&);` — throws `UsageError` above 8 192 bytes.
  - `class AttemptLimiter`:
    - `AttemptLimiter(std::size_t max_attempts = 200, std::chrono::seconds window = std::chrono::minutes(5));`
    - `bool try_take(std::chrono::steady_clock::time_point now);`
    - `std::chrono::steady_clock::time_point next_available(std::chrono::steady_clock::time_point now) const;`
  - `class Backoff`:
    - `Backoff(std::chrono::milliseconds initial, std::chrono::milliseconds cap, double jitter, std::uint32_t seed);`
    - `std::chrono::milliseconds next() noexcept;` — doubles up to the cap, ±jitter.
    - `void reset() noexcept;`
  - `struct NetTuning` — every network timing, so tests can shrink them. Defaults are the spec values:
    - `limiter_attempts = 200`, `limiter_window = 5min`;
    - `backoff_initial = 500ms`, `backoff_cap = 30s`, `backoff_jitter = 0.2`, `backoff_reset_after = 60s`;
    - `rest_timeout = 10s`, `validation_give_up = 30s`, `default_retry_after = 60s`;
    - `ws_handshake_timeout = 10s`, `ws_idle_timeout = 30s`, `close_timeout = 2s`, `shutdown_deadline = 3s`;
    - `poll_interval = 10ms`, `ring_retry = 10ms`.

- [ ] **Step 1: Write failing tests:**
  - `Backoff` with seed 1 and jitter 0: 500, 1000, 2000, … ms capped at 30 000; `reset()` restarts at 500. With jitter 0.2 every value lies within ±20 % of the un-jittered one;
  - spot target equals `/stream?streams=btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade`;
  - usdm target prefix is `/public/stream?streams=`;
  - 120 symbols of 20 characters in one shard → `UsageError`;
  - `make_shards(256, 50)` gives 6 shards (the last one has 6 symbols);
  - the limiter allows 200 attempts in a window, denies the 201st, and allows again after the oldest expires.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add endpoints, sharding and connection-attempt limiter"`

### Task 24: TLS context, REST client and scheduler (with symbol validation)

**Files:**
- Create: `src/blc/net/tls.hpp`, `src/blc/net/tls.cpp`, `src/blc/net/dispatch_policy.hpp`, `src/blc/net/dispatch_policy.cpp` (pure logic: no Asio, every call takes `now`), `src/blc/net/rest_scheduler.hpp`, `src/blc/net/rest_scheduler.cpp` (Asio glue: timers plus transport around the policy)
- Test: `tests/net_dispatch_policy_test.cpp` (deterministic, explicit clock, no sleeping)

**Interfaces:**
- Produces:
  - `struct TlsConfig { std::string extra_ca_pem; };` — empty means the system store only.
  - `boost::asio::ssl::context make_tls_client_context(const TlsConfig&);` — TLS ≥ 1.2, `verify_peer`; on CA load failure it throws `IoError` mentioning `ca-certificates`.
  - `class RestTransport` (interface): `virtual void get(const Endpoint&, std::string target, std::function<void(HttpResult)>) = 0;`, with `struct HttpResult { int status; std::string body; int retry_after_s; int used_weight_1m; std::int64_t recv_ns; bool transport_error; };` Implementation `BeastRestTransport`: Beast HTTP over TLS, SNI, host verification, 10 s timeout, body ≤ 1 MiB.
  - `class DispatchPolicy` (pure):
    - `DispatchPolicy(Venue, const NetTuning&);`
    - `void want(std::uint16_t symbol, std::uint64_t slot_value, Clock::time_point now);`
    - `std::optional<Dispatch> next_dispatch(Clock::time_point now, std::int64_t wall_now_s);` — `Dispatch { std::uint16_t symbol; std::uint64_t slot_value; int weight; }`. `wall_now_s` places the usage header's minute boundary.
    - `Clock::time_point next_wakeup(Clock::time_point now) const;`
    - `Outcome on_response(std::uint16_t symbol, const HttpResult&, Clock::time_point now, std::int64_t wall_now_s);` — `Outcome` ∈ {`Deliver`, `Retry`, `Fatal`}.
    - `enum class DeliveryResult { Delivered, Discard, RingFull };` and `struct ValidationResult { enum class Kind { Ok, InvalidSymbol, Unreachable } kind; std::string symbol; std::string message; };`
    - `void on_delivery(std::uint16_t symbol, DeliveryResult, Clock::time_point now);`
    - `Clock` is `std::chrono::steady_clock`. Minute rollover for the usage header uses an injected `int64 wall_now_s` argument.
  - `class RestScheduler` (Asio glue around `DispatchPolicy`):
    - `RestScheduler(boost::asio::io_context&, Venue, const VenueEndpoints&, const NetTuning&, RestTransport&, IoMetrics&);`
    - `void validate_symbols(const std::vector<SymbolSpec>&, std::function<void(ValidationResult)>);`
    - `void want(std::uint16_t symbol, std::uint64_t slot_value);`
    - `void set_delivery(std::function<DeliveryResult(std::uint16_t symbol, std::uint32_t epoch, const HttpResult&)>);` — the delivery callback returns `Delivered`, `Discard` or `RingFull`.
    - `void stop();`
  - Constants: budget 3 000 (spot) / 1 200 (usdm) per minute; weights 50/20 (depth) and 2/1 (ticker); 4 dispatch slots.

- [ ] **Step 1: Write failing `DispatchPolicy` tests.** Each passes explicit `now` values; none sleeps.
  - token pacing: 61 spot requests leave the 61st waiting;
  - `X-MBX-USED-WEIGHT-1M` reconciliation pauses until the next minute;
  - a 429 with `Retry-After: 5` pauses all dispatch for 5 s, and the request is requeued at the head;
  - a 418 with `Retry-After: 120` pauses all dispatch for 120 s; a 418 without the header pauses `default_retry_after`;
  - a 5xx is retried with backoff;
  - a 400 is fatal;
  - a newer slot value replaces a queued one in place;
  - a newer value during an in-flight request is served after it completes;
  - a failed request with a newer value waiting requeues only the newer one;
  - a `RingFull` delivery keeps the dispatch slot and retries in 10 ms;
  - a `Discard` delivery releases the slot;
  - validation: 400 for `BTCUSDX` → result names the symbol; transport failure for 30 s → result is unreachable.
- [ ] **Step 2: Run** `--gtest_filter='DispatchPolicy*'`. Expected: FAIL. **Step 3: Implement** the policy, then the scheduler glue: one steady timer, armed for `next_wakeup`. The glue's behaviour against a real transport is covered by Task 27. **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add TLS context, REST transport and weighted scheduler"`

### Task 25: `WsShard` — connection state machine, reconnect, state-aware shutdown

**Files:**
- Create: `src/blc/net/ws_shard.hpp`, `src/blc/net/ws_shard.cpp`

**Interfaces:**
- Consumes: `FrameRouter`, `AttemptLimiter`, `make_tls_client_context`, `VenueEndpoints`.
- Produces:
  - `class WsShard : public std::enable_shared_from_this<WsShard>`:
    - `enum class State { Waiting, Resolving, Connecting, TlsHandshake, WsHandshake, Open, Closing, Closed };`
    - `WsShard(boost::asio::io_context&, boost::asio::ssl::context&, Endpoint, std::string target, FrameRouter&, AttemptLimiter&, const NetTuning&, IoMetrics&, int fault_reconnect_every_s);` — reconnect delays come from a `Backoff` built from `NetTuning`.
    - `void start();`
    - `void shutdown();` — per state, as in §6.7.
    - `State state() const;`
    - `std::uint32_t epoch() const;`
  - Read buffer: `flat_buffer`, shrunk after messages > 256 KiB; `read_message_max(1 << 20)`; idle timeout 30 s with keep-alive pings.

- [ ] **Step 1: Implement.** Every handler checks `stopping_` first. Any read error, close or loss calls `FrameRouter::on_disconnected()` before anything else. Reconnects take a limiter token. A loss reported by `FrameRouter` closes the connection and reconnects at once, skipping backoff but not the limiter. Logic is tested by Task 27; there is no separate unit test here, because the class is I/O glue over pieces already tested.
- [ ] **Step 2: Build** with `-Werror` in WSL (`cmake --preset release && cmake --build build/release`). Expected: no warnings.
- [ ] **Step 3: Commit** — `git commit -m "Add WebSocket shard with reconnect and state-aware shutdown"`

### Task 26: `CaptureApp` — startup order, signals, duration, shutdown protocol, stats, memory budget

**Files:**
- Create: `src/blc/net/capture_app.hpp`, `src/blc/net/capture_app.cpp`
- Modify: `src/main.cpp` (dispatch `Mode::Capture` to `run_capture`)

**Interfaces:**
- Produces:
  - `struct CaptureConfig`:
    - `CaptureOptions options;`
    - `VenueEndpoints endpoints;`
    - `TlsConfig tls;`
    - `NetTuning tuning;`
    - `std::function<std::unique_ptr<SymbolOutput>(const SymbolSpec&)> output_factory;` — empty means `FileOutput`. Tests use it to inject a failing writer.
    - Tests override everything except `options`' semantics.
  - `int run_capture(const CaptureConfig&);` — exit codes per §2.
  - `std::size_t budget_mib(std::size_t symbols, std::size_t connections);` — the §9 formula in bytes, rounded up to whole MiB:
    - ring band (16 / 32 / 64 MiB);
    - 5 MiB parser;
    - 672 000 B decode scratch;
    - 64 KiB sidecar scratch;
    - 1 MiB minify scratch;
    - 8 B × symbols;
    - 883 136 B × symbols;
    - 327 680 B × connections retained;
    - 1 MiB × connections + 4 × 1 114 112 B transient;
    - 48 MiB allowance.
  - Startup order (§3.1):
    1. validate options — including `ws_target` for every shard, so an over-long URL exits 2 before any network use;
    2. validate symbols (REST);
    3. `prepare_output_dir`;
    4. **one** `archive_existing` call with every symbol's three names;
    5. create files;
    6. start the processor thread (`run_consumer`);
    7. start shards.

    The `--duration` deadline is measured from process start (`main` records `steady_clock::now()` first), not from step 7.
  - A `poll_interval` io timer scans `RequestSlots` and the consumer's `fatal_code`. Stats print every `stats_interval_s`.
  - The final summary prints `VmHWM`, `budget_mib`, the `epoch_closed` lines and a totals line. It checks `frames_received == md_rows + malformed_frames + ring_drops`; on a violation it logs an error, and the exit code becomes 1.

- [ ] **Step 1: Write a failing test** `tests/net_budget_test.cpp`: `budget_mib(1, 1) == 84` (83.1 exactly), `budget_mib(256, 64) == 429` (428.6), and `budget_mib(256, 6) == 353` (the formula includes the io thread's 6 MiB envelope-splitter fallback).
- [ ] **Step 2: Implement** `budget_mib` and `run_capture` as in §6.7: `close_producer` runs after `run()` returns, and the processor drains. **Step 3:** Run the budget test. Expected: PASS.
- [ ] **Step 4: Manual live run in WSL:** `./build/release/binance_capture --symbols BTCUSDT --duration 20 --output-dir /tmp/c1`. Expected: exit 0; three files; the order book starts with an `R` row; `blc_verify` passes on all files.
- [ ] **Step 5: Commit** — `git commit -m "Add capture application and wire main"`

### Task 27: `capture_loopback` integration test

**Files:**
- Create: `tests/support/loopback_server.hpp`, `tests/support/loopback_server.cpp`, `tests/support/test_ca.hpp`, `tests/support/test_ca.cpp`, `tests/net_capture_loopback_test.cpp`
- Modify: `.github/workflows/ci.yml` (the ASan and TSan jobs already run all of `unit_tests`; check that the loopback test runs there)

**Interfaces:**
- Produces (test support):
  - `TestCa make_test_ca();` — an in-memory CA plus a server certificate for `127.0.0.1`, generated with the OpenSSL EVP/X509 APIs; nothing is written to disk.
  - `class LoopbackServer` — Beast TLS WebSocket server and an HTTPS server on ephemeral ports. Scriptable: frames to send; delays before the TLS and WebSocket handshake responses; 429 responses; forced close; `ticker/price` returning 200 for known symbols and 400 `-1121` otherwise.

- [ ] **Step 1: Write the tests**, each running `run_capture` in-process with `CaptureConfig` pointed at the server:
  - `ConnectSyncAndRows`: the order book starts with `R`; the rows match the scripted book.
  - `ForcedReconnectStartsNewEpoch`: `conn_epoch` 1 appears; the symbol resyncs.
  - `Rest429Cooldown`: no request is sent during the cooldown window (server-side timestamps).
  - `LossTransition`: a server-sent frame that is not JSON → reconnect, and no rows from the old epoch after it.
  - `SigintClean`, `SecondSigint`, `DurationExpiry`: exit 0; files flushed; no leaks under ASan.
  - `ShutdownDuringTlsHandshake`, `ShutdownDuringWsHandshake`: the server delays the handshake 2 s; SIGINT arrives at 0.5 s; exit 0 within 3 s; no WebSocket close is attempted.
  - `AttemptLimiterUnderStorm`: the server closes every connection immediately; with a 5-attempt / 2 s test window, attempts never exceed the limit.
  - `InvalidSymbolExit2`: `--symbols BTCUSDX` → exit 2; no files created.
  - `ReversedEnvelopeFrames`: the server sends some frames with `data` before `stream` → the same rows as the forward form, and no loss transition.
  - `UntrustedCertificateRejected`: the client is given a different test CA → the TLS handshake fails, no rows are written, and the error is logged; verification is never bypassed (Q16).
  - `WriterFailureExitsOne`: `output_factory` returns a writer that fails after 5 rows → shutdown within `poll_interval` + `shutdown_deadline`, exit 1.
  - All timings come from a test `NetTuning` (limiter 5 attempts / 2 s, backoff 10–100 ms, shutdown deadline 1 s), multiplied by `BLC_TEST_TIME_SCALE`. Handshake-delay tests trigger SIGINT on a server event (ClientHello received / HTTP Upgrade received), not after a sleep. Assertions are orderings and upper bounds (for example "exit within 5 s × scale"), never exact counts.
  - `SigtermClean`: like `SigintClean`, with SIGTERM.
  - `DurationExpiryDuringHandshakes`: `--duration` expires while the server holds the TLS and then the WebSocket handshake → exit 0, no WebSocket close attempted.
  - `ShutdownWhileWaiting`: the server refuses connections, so the shard sits in backoff (`Waiting`); SIGINT → exit 0 promptly, no further attempts. (`Resolving`/`Connecting` complete instantly on loopback and cannot be held open reliably; their cancel path is the same `close lowest layer` call exercised by the handshake tests.)
  - `OversizedFrameRejected`: the server sends a 1.5 MiB frame → read error, reconnect, counted; no record pushed.
  - `OversizedRestBodyRetried`: a 1.5 MiB depth body → request fails, retried, counted; the symbol syncs once a normal body is served.
  - The test CA issues the server certificate with `subjectAltName = IP:127.0.0.1`, so `host_name_verification("127.0.0.1")` passes.
  - `DurationBeforeSyncExitsZero` (Review Focus 5): snapshots are delayed past `--duration 1` → exit 0; header-only order-book file; summary has `syncs=0`.
- [ ] **Step 2: Run** in WSL: `./build/debug/tests/unit_tests --gtest_filter='CaptureLoopback*'`. Then run the same under `build/asan` and `build/tsan`. Expected: PASS, no sanitizer reports.
- [ ] **Step 3: Commit and push**; check CI is green — `git commit -m "Add loopback capture integration test"`

### Task 28: Live smoke script

**Files:**
- Create: `scripts/live_smoke.sh`

- [ ] **Step 1: Write the script**. For each venue:
  - (a) ASan build, 60 s capture of BTCUSDT ended with `kill -INT`; zero `LeakSanitizer` lines in stderr;
  - (b) Release build, 60 s capture; `VmHWM ≤ budget_mib` from the summary;
  - `verify_outputs.sh` on both (headers, `awk -F',' 'NR==2{print NF}'` = 26, `blc_verify --epoch-log`);
  - replay of (b) → `cmp` with its order book;
  - `depth5_mismatch=0` for venues on the `resync` policy.

  Then, once on spot, a **multi-symbol** run: `--symbols BTCUSDT,ETHUSDT --symbols-per-connection 1` (two shards) for 60 s with the Release build, then `blc_verify`, replay of every `market_data_*.csv` and `cmp` per symbol. This is the X2 evidence, and it exercises shared sidecar scratch with interleaved snapshots.
- [ ] **Step 2: Run** `scripts/live_smoke.sh` in WSL. Expected: prints `LIVE SMOKE PASSED` for spot and usdm. Also confirm whether GitHub runners get HTTP 451 (§15) with a one-off `workflow_dispatch` curl job, and record the result in design.md §15.
- [ ] **Step 3: Update requirements.md** (F1–F3, F6, F7, Q5, Q6, Q13, Q14, Q16, X1, X2 as their evidence now exists). Commit — `git commit -m "Add live smoke script; record live verification"`

---

## Milestone M6 — Hardening

### Task 29: clang-tidy CI job

**Files:** Modify `.github/workflows/ci.yml`; source fixes.
- [ ] **Step 1:** In WSL, configure a Clang build so the compile database holds Clang flags (GCC-only warnings such as `-Wlogical-op` would make clang-tidy error): `cmake -B build/tidy -G Ninja -DCMAKE_CXX_COMPILER=clang++-15 -DCMAKE_BUILD_TYPE=Debug`. Then run `run-clang-tidy-15 -p build/tidy -header-filter='src/.*' 'src/.*'` (`third_party/` excluded) and fix the findings. Keep each suppression narrow, with a `// NOLINT(<check>): <reason>` comment.
- [ ] **Step 2:** Add the `clang-tidy` job, run with `-warnings-as-errors='*'` (the repo's `.clang-tidy` keeps `WarningsAsErrors: ''` for local use), so any warning fails it. Push; expect green.
- [ ] **Step 3:** Commit — `git commit -m "CI: clang-tidy job and fixes"`

### Task 30: `--fault-reconnect-every`, limit validation, remaining shutdown tests

**Files:** Modify `src/blc/net/ws_shard.cpp`, `src/blc/cli/options.cpp`; Test: `tests/cli_options_test.cpp`, `tests/net_capture_loopback_test.cpp`
- [ ] **Step 1: Write failing tests:**
  - `FaultReconnectEvery`: with the loopback server and `--fault-reconnect-every 1`, the capture runs until at least epoch 2 has synced (event-gated, upper bound 10 s × scale). Assert epochs are strictly increasing, each epoch's first row is `R`, and no row precedes its epoch's `R`;
  - option limits: `--fault-reconnect-every -1` and `--stats-interval -1` are usage errors.
- [ ] **Step 2: Run.** FAIL. **Step 3: Implement.** **Step 4: Run.** PASS.
- [ ] **Step 5: Commit** — `git commit -m "Add fault-injection reconnects and limit validation"`

### Task 31: Book and payload-parse benchmarks (mandatory: back Q11 and Q9)

**Files:** Create `bench/CMakeLists.txt`, `bench/book_bench.cpp`, `bench/payload_bench.cpp`; root `CMakeLists.txt` adds `option(BLC_BUILD_BENCH OFF)`.
- [ ] **Step 1: Implement** `blc_bench book` comparing `BookSide` with a `std::map` reference over four workloads (near-touch updates, deep updates, near-touch deletes, deep deletes; 10⁶ operations each; book of 1 000–5 000 levels). Print median ns/op and p99 over 10 repetitions.
- [ ] **Step 1b: Implement** `blc_bench payload`: `PayloadParser` on the Task 9 fixtures (spot/usdm diff, depth5, trade), 10⁶ parses each. Print ns per message and MB/s.
- [ ] **Step 2: Run** in WSL on Release. Paste the table and the machine description into README draft notes (`docs/benchmarks.md`).
- [ ] **Step 3: Commit** — `git commit -m "Add book benchmark (flat vector vs std::map)"`

### Task 32: Long live runs

- [ ] **Step 1:** In WSL (Release), run a 10-minute capture per venue and check `depth5_mismatch=0`, or apply the §5.6 `count-only` fallback with documented evidence.
- [ ] **Step 2:** Run a 30-minute spot capture and record `coverage_exhausted`, `resyncs` and `VmHWM` against `budget_mib`.
- [ ] **Step 3:** Record the results in design.md §15 and `docs/benchmarks.md`; update requirements.md. Commit — `git commit -m "Record long-run live verification"`

---

## Milestone M7 — Optional: coverage refresh and performance notes

> Never blocks submission (§16). If it is skipped, X3 is marked **Deferred** with a reason.

### Task 33: Background coverage refresh

**Files:** Modify `src/blc/book/symbol_book.{hpp,cpp}`; Test: `tests/book_refresh_test.cpp`; extend `tests/book_property_test.cpp`.
- [ ] **Step 1: Write failing tests:**
  - margin below 200 → `snapshot_wanted()`;
  - shadow sync from the window, swapping at the snapshot record (`R` row) or at a later diff (`D` row);
  - a too-old snapshot is refetched;
  - an equal top counts `refresh_ok`; a different top swaps and counts `refresh_mismatch`;
  - exhaustion during the refresh hands the snapshot over to the resync;
  - the property test with refresh on: no suppression from coverage exhaustion while snapshots arrive within 2 steps.
- [ ] **Step 2: Run.** FAIL. **Step 3: Implement** §5.5. The shadow book is pre-reserved per symbol. **Step 4: Run.** PASS. **Step 5: Commit** — `git commit -m "Add background coverage refresh"`

### Task 34: Remaining benchmarks and performance notes

- [ ] **Step 1:** Add `blc_bench decimal|ring|replay` (`replay`: end-to-end records/s and p50/p99/p99.9 per record). `payload` already exists from Task 31.
- [ ] **Step 2:** Run them and update `docs/benchmarks.md`. Mark X3 Done. Commit — `git commit -m "Add parsing, ring and end-to-end benchmarks"`

---

## Milestone M8 — Submission

### Task 35: Sample run and live = replay check in CI

**Files:** Create `samples/` (three files from one run); modify `.github/workflows/ci.yml`.
- [ ] **Step 1:** In WSL (Release), run `./build/release/binance_capture --venue spot --symbols BTCUSDT --duration 120 --output-dir samples`, then confirm `blc_verify samples/*.csv` exits 0.
- [ ] **Step 2:** Extend `output-contract`: replay `samples/market_data_*.csv` and `cmp` against `samples/*_orderbook.csv`. Extend `asan-ubsan`: replay the samples.
- [ ] **Step 3:** Push; expect green. Commit — `git commit -m "Add 2-minute BTCUSDT sample run and live=replay CI check"`

### Task 36: README, `check_readme.sh`, `reviewer_checklist.sh`

**Files:** Modify `README.md`; create `scripts/check_readme.sh`, `scripts/reviewer_checklist.sh`.
- [ ] **Step 1: Write the README.** Sections:
  - overview;
  - toolchain (GCC 12, C++17, Clang 15; dependencies with versions);
  - exact CLI for both CSVs and the symbol-list format;
  - file naming;
  - scaling and timestamp policies;
  - reconnect and gap handling;
  - threading and I/O design note;
  - replay ("no network calls");
  - metrics;
  - limits;
  - the two readings of the secrets check;
  - sample excerpt (first 3 rows of each file);
  - benchmark table (Task 31);
  - **then, at the very end, the brief's section verbatim** with the adaptations listed in requirements.md S6.
- [ ] **Step 2: Write `check_readme.sh`.** It extracts the README's text after the `How to Submit on GitHub` heading, then checks that every heading and command from S6 appears in order, allowing only the listed adaptations. Add it to CI.
- [ ] **Step 3: Write `reviewer_checklist.sh`.** It runs the brief's Reviewer Checklist verbatim in a scratch clone: clean build, a 300 s capture without `--output-dir` (the duration can be overridden for a quick run), the `awk` check (expect `26`), and the secrets grep (pattern assembled at runtime; expect empty).
- [ ] **Step 4:** Run both in WSL. Expected: `README OK` and `CHECKLIST PASSED`. Commit — `git commit -m "Complete README and add reviewer checklist scripts"`

### Task 37: Release

- [ ] **Step 1:** Walk requirements.md: every Functional, Submission and Review-focus row is **Done** with its evidence linked. Each stretch row is Done or Deferred with a reason.
- [ ] **Step 2:** In a fresh `ubuntu:22.04` container (`docker run` or a clean WSL distro), follow the README from `git clone` to the checklist. Expected: everything passes as written.
- [ ] **Step 3:** `git tag -a v1.0.0 -m "Submission v1.0.0" && git push origin v1.0.0`. Confirm CI is green on the tag.
- [ ] **Step 4:** **Ask the user for confirmation**, then make the repository public (`gh repo edit GruffElixir/binance-lob-capture --visibility public --accept-visibility-change-consequences`).
