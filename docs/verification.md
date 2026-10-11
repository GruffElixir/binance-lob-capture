# Implementation verification

The remaining implementation is on `codex/complete-binance-capture`, starting from `5fe6bb0`.
The assignment contracts are tracked in [requirements.md](requirements.md); the implementation
and its rationale are in [design.md](design.md). Real traffic and performance measurements are
recorded in [benchmarks.md](benchmarks.md).

## Completed local checks

- Ubuntu 22.04, GCC 12, Boost 1.74, OpenSSL 3.0.2: Debug suite **302/302 passed**, including
  POSIX permission and `/dev/full` failures. Release build passed with warnings as errors.
- Windows, MinGW GCC 16: **299 passed**, three tests skipped: symlink creation requires extra
  Windows privileges; directory permission and `/dev/full` cases require Linux.
- The 29 real TLS loopback cases exercise signals, connection-state cancellation, second
  signal, bounded fragmented messages, reconnect, fault timers, invalid JSON, rate cooldown,
  writer failure, certificate rejection, duration expiry and previous-run archive reporting.
  Separate REST transport tests cover pending DNS deadlines, cancellation and late callbacks.
- Coverage refresh: seven tests failed before implementation and passed afterwards. Both
  venues have an independent full-depth property oracle; finite-coverage exhaustion remains
  tested separately. Real-file allocation tests cover equal/different refresh swaps and
  exhaustion, with zero measured processing allocations.
- Replay timing and run-start filename tests failed on their missing interfaces, then passed
  in the full suite. Timing observations cover every market row and preserve the exact output.
- The committed 120-second spot BTCUSDT sample contains **7,107 market rows**, **7,098 book
  rows**, one snapshot row, **1,185 full depth5 checks**, zero mismatches/drops/parse errors,
  and **30,684 KiB peak RSS** against an 84 MiB budget. All three CSVs verified, with exact
  replay equality.
- Final full sanitizer suites: ASan/UBSan/LSan **302/302 passed**, TSan **302/302 passed**.
  The committed sample also verified and replayed identically under ASan/UBSan/LSan.
- A clean official Ubuntu 22.04 filesystem, bootstrapped independently inside WSL, used only
  the README packages plus Git. The README Release build and all **302 tests** passed as a
  regular user. The checklist then cloned the final tree again, ran the exact default build,
  completed a real **300-second** BTCUSDT capture without `--output-dir`, checked 26 columns,
  the literal grep, frame accounting and positive live gates, and printed **CHECKLIST PASSED**.
- [CI run 38164859492](https://github.com/GruffElixir/binance-lob-capture/actions/runs/38164859492)
  passed every mandatory job on `2f2bece`: GCC 12 and Clang 15 warning-free Release builds,
  formatting, clang-tidy, ASan/UBSan, TSan, Valgrind, clean default build, scenarios, schemas,
  sample equality and the no-socket replay assertion. The optional availability probe was
  separately verified in the run linked from [benchmarks.md](benchmarks.md).

## Reproduction

Use the README commands from a fresh checkout. Linux build folders used during development
were named `build/linux-*` to avoid existing native Windows CMake caches; that is a local
workspace choice. A local `-fuse-ld=lld` speeds debug links on the mounted Windows drive and
is not required by the project. Source fixtures are located through configured constants,
not the current working directory.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-12 -DBLC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
./build/tests/unit_tests
./build/binance_capture --replay samples/market_data_*.csv --output-dir replay_output
./build/blc_verify samples/*.csv replay_output/*.csv
cmp samples/*_orderbook.csv replay_output/*_orderbook.csv
scripts/check_readme.sh
scripts/reviewer_checklist.sh
```

The CI workflow repeats GCC/Clang builds, sanitizer suites, formatting, clang-tidy, scenario
replays, Valgrind, schema checks, sample equality, and a `strace` assertion that replay opens
no sockets. Live public endpoint availability is probed separately from deterministic tests.

## Rulings

- The Ubuntu 22.04 `run-clang-tidy` wrapper cannot pass `-warnings-as-errors`; the checked-in
  script invokes `clang-tidy-15` directly on tracked application translation units, with the
  Clang compile database and a source-only header filter. Suppressions name a specific check
  and explain the reserved storage or simdjson rvalue API requirement.
- Public repository visibility was explicitly authorized by the user on 2026-10-11 and
  applied immediately. The annotated submission tag remains a final release step.
- Docker was unavailable, so release validation used a fresh official Jammy filesystem with
  independent packages and checkout in a chroot under WSL. The host kernel is shared, as it
  would be for a container; existing development headers/build caches were not exposed.
- A local TSan object compiled before the newest archive test initially yielded 301 tests.
  Test-list comparison exposed the omission; forced recompilation and the complete 302-test
  run passed. Fresh CI independently compiled every test from its checkout.

## Plan completion

| Tasks | Result | Implementation/evidence |
|---|---|---|
| 1–24 | Complete before continuation | `5fe6bb0`; primitives, parsing, book, bounded pipeline, replay, scheduler and transport |
| 25–28, 30 | Complete | `3be903b`, `2f2bece`; live capture, TLS loopback, shutdown, live smoke and fault reconnect |
| 29 | Complete | `ef3b91e`, `2f2bece`; clang-tidy, warning-free compilers and CI |
| 31, 34 | Complete | `ef3b91e`; [reproducible benchmark results](benchmarks.md) |
| 32 | Complete | `2f2bece`; both-venue 10-minute and 30-minute Spot [live results](benchmarks.md) |
| 33 | Complete | `b29ad26`; coverage refresh, independent property oracle and allocation tests |
| 35–36 | Complete | `5f8a312`, `2f2bece`; real sample, README, exact command manifest and clean checklist |
| 37 | Release integration remains | Public visibility done; annotated `v1.0.0` and final S5 bookkeeping follow integration |

## Final independent review

One fresh supervisor reviewed the implementation from the task-24 baseline through `5f8a312`.
It found no critical issues and two important issues: the fixed-name writable-directory probe
could overwrite an existing file or symlink target, and the REST deadline excluded DNS resolution.
Both were reproduced before correction. Exclusive probe creation now preserves existing paths;
a timer starts before resolution and completes the logical request once, including cancellation
and late resolver callbacks. Boost's OS resolver worker may still wait for a system DNS call
on destruction; the deadline promises request-slot release rather than a hard OS shutdown bound.

The review also caught an unsupported README trade flag, a missing previous-run archive warning,
and a header-only shutdown test that did not assert file existence. The flag is corrected; a
failing regression now checks the archive warning; the shutdown test checks real files and its
zero-sync summary. The archive warning is treated as a required fix because the design promises
it and users need to locate previous output. Signal/reconnect tests now wait for an actual
book-sync row before interruption, preserving real-file output under slower sanitizer runs.

Startup validation is a separate lifecycle stage: expiry there intentionally creates no files,
as task 26 specifies. After validation, expiry before book sync leaves header-only files and a
summary. Malformed replay CSV fails the file; semantic payload failures follow the shared
processor's documented count/resync policy. These are explicit contract choices, not omitted
error handling. Final fix verification uses the full test and CI gates, without another review cycle.
