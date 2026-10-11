# Design: binance-lob-capture

Approved 2026-10-11; revised the same day after design review. This document is the
specification the implementation follows; the README's design notes summarise it. Requirement
IDs (F*, S*, Q*, X*) refer to [requirements.md](requirements.md). Features marked
**(enhancement)** go beyond the brief and are built only after the mandatory behaviour they
extend is complete.

## 1. Scope

`binance_capture` is a single C++17 executable with two modes:

- **Capture** — connects to Binance public combined WebSocket streams for one venue (`spot` or
  `usdm`) and a list of symbols, subscribes to `<symbol>@depth@100ms`, `<symbol>@depth5@100ms` and
  `<symbol>@trade`, and writes:
  - **Deliverable A** — a market-data audit CSV: one row per inbound data message, holding the
    inner Binance `data` payload.
  - **Deliverable B** — an order-book CSV: one top-5 snapshot row after each processed event of a
    synchronised book.
  - a depth-snapshot sidecar CSV recording the REST snapshots used to synchronise the book, so a
    capture can be replayed exactly.
- **Replay** — reads saved market-data CSVs (plus their sidecars) and regenerates the order-book
  CSVs byte-for-byte, without any network access.

Out of scope: trading, authenticated endpoints, both venues in one process (run two processes),
persistence formats other than CSV.

## 2. Command-line interface

```
binance_capture [--venue spot|usdm] --symbols LIST [--output-dir DIR] [--duration SECONDS]
                [--no-trade-rows] [--symbols-per-connection N] [--stats-interval SECONDS]
                [--fault-reconnect-every SECONDS]
binance_capture --replay FILE [FILE...] [--output-dir DIR] [--no-trade-rows]
                [--stats-interval SECONDS]
binance_capture --help | --version
```

| Option | Mode | Default | Rules |
|---|---|---|---|
| `--venue` | capture | `spot` | `spot` or `usdm` |
| `--symbols` | capture | required | Comma-separated, e.g. `BTCUSDT,ETHUSDT`. Case-insensitive (normalised to uppercase); whitespace around items trimmed; each item 2–20 characters of `A–Z0–9`; no duplicates; 1–256 symbols |
| `--output-dir` | both | capture: `.`; replay: `./replay_output` | Created if missing |
| `--duration` | capture | `0` = run until SIGINT/SIGTERM | Integer seconds, 0–2 678 400 (31 days), counted from process start |
| `--no-trade-rows` | both | off | Trades still go to the market-data CSV but produce no order-book rows (§5.8) |
| `--symbols-per-connection` | capture | `50` | 1 – venue cap (§9); the resulting stream URL must also fit the 8 192-byte limit |
| `--stats-interval` | both | `10` | Seconds between stats lines on stderr; `0` = final summary only |
| `--fault-reconnect-every` | capture | `0` = off | **(enhancement)** Testing aid: force-close every connection each N seconds to exercise reconnect + resync live |
| `--replay` | replay | — | One or more market-data CSVs; each is replayed independently (§7) |

Both `--opt value` and `--opt=value` are accepted. Unknown options, missing values and invalid
values are usage errors.

Exit codes: `0` normal completion (including shutdown by signal or `--duration`), `1` runtime
failure (output I/O error including a failed final flush or close, malformed replay input,
unrecoverable configuration error), `2` usage error.

The default capture `--output-dir` is `.` because the reviewer checklist runs a capture without
`--output-dir` and then checks `*_orderbook.csv` in the working directory.

## 3. Output files

### 3.1 Naming and lifecycle

Capture writes three files per symbol:

| File | Content |
|---|---|
| `market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv` | Deliverable A (§3.3) |
| `market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>_orderbook.csv` | Deliverable B (§3.4) |
| `depth_snapshots_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv` | REST snapshots used for sync (§3.5) |

- **One file per symbol** (the brief's per-symbol naming option). The `symbol` column is still
  written. Per-symbol files keep each book independently replayable and its `seqNo` contiguous.
- **One set of files per run.** `<YYYY-MM-DD>` is the UTC date at process start. Files are never
  rotated: a capture that crosses UTC midnight stays in the files named after its start date. A
  run's files are therefore self-contained: each can be replayed on its own, and each order-book
  file starts with an `R` row.
- All files for all configured symbols are created at startup, with their header rows.
- The sidecar deliberately does not match the `market_data_*.csv` or `*_orderbook.csv` globs.
- **Existing files are never overwritten and never cause an error.** If any of a symbol's three
  files already exists, every existing file of that symbol is moved, keeping its name, into one
  archive directory for the run: `<output-dir>/replaced_<run start, YYYYMMDDTHHMMSSZ>/` (with
  `-1`, `-2`, … appended if that directory exists). A warning names the directory.
  - A symbol's three files are always created together by one run, so an archived set keeps
    its pairing: the archived market-data file finds its sidecar under the original name and
    replays exactly like the original (§7).
  - The brief's own README sequence writes several runs into `./output` on the same day, so
    refusing would break it, and truncating would silently destroy an earlier capture.
  - Archive directories are not matched by the output globs, which do not descend into
    directories.
- **Replay** writes `<output-dir>/<input file stem>_orderbook.csv` for each input file. An
  existing file of that name is moved into a `replaced_<…>/` directory in the same way.

### 3.2 CSV encoding (all files)

- UTF-8 without BOM (content is ASCII in practice).
- Field separator `,`. Every record, including the header and the last record, ends with `\n`.
  RFC 4180 specifies CRLF; LF is chosen for Unix tooling (`awk`, `head`, `cut`). Quoting and
  escaping follow RFC 4180 exactly, and the reader accepts LF and CRLF.
- A field is enclosed in double quotes if and only if it contains `,`, `"`, CR or LF; inside a
  quoted field each `"` is written as `""`. In practice only `payload_json` is quoted, and always.
- Integers are base 10 with no sign, padding or separators.
- No field is ever truncated. A record that cannot be written completely is an I/O error and the
  process exits with code 1.

### 3.3 Market-data CSV (Deliverable A)

Header (exact):

```
recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json
```

| Column | Type | Value |
|---|---|---|
| `recv_tsec` | int64 | Seconds part of the receive time (§4.1) |
| `recv_tnsec` | int32 | Nanosecond remainder, 0 – 999 999 999 |
| `venue` | text | `spot` or `usdm` |
| `stream_kind` | text | `depth_diff`, `depth5` or `trade`, derived from the stream name |
| `shard_id` | int | 0-based connection index (§4.4) |
| `conn_epoch` | int | 0 for a shard's first connection, +1 on each reconnect (§4.4) |
| `conn_seq` | int | 0-based data-frame index within (`shard_id`, `conn_epoch`) (§4.4) |
| `symbol` | text | Uppercase symbol from the stream name, e.g. `BTCUSDT` |
| `payload_json` | text | The envelope's inner `data` object, minified, one line, CSV-escaped |

- One row per WebSocket data frame with a valid envelope for a subscribed stream, in processing
  order: the processor writes the row before parsing the payload or touching the book.
- `payload_json` is the received bytes of the `data` value. Binance sends compact JSON (no frame
  in the probe contained whitespace); if a payload ever contains whitespace bytes it is minified
  with `simdjson::minify` before writing, and counted.

Example (spot trade from the live probe; `recv_tnsec` and `conn_seq` illustrative):

```
1791669451,812345678,spot,trade,0,0,57,BTCUSDT,"{""e"":""trade"",""E"":1791669451798,""s"":""BTCUSDT"",""t"":6752440754,""p"":""83044.00000000"",""q"":""0.00009000"",""T"":1791669451798,""m"":true,""M"":true}"
```

### 3.4 Order-book CSV (Deliverable B)

Header (exact, 26 columns):

```
tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,ask_size0,ask_size1,ask_size2,ask_size3,ask_size4
```

The brief's PDF renders this line clipped at `ask_siz`; the remainder is fixed by the brief's
"26 data columns" rule and the `ask_size0 … ask_size4` column table. A unit test pins the exact
string and a `static_assert` pins the count.

| Column | Type | Value |
|---|---|---|
| `tsec`, `tnsec` | int64, int32 | Receive time of the record that produced the row (§4.1, §5.8); `tsec * 1e9 + tnsec` = ns since the Unix epoch |
| `seqNo` | uint64 | 0-based, +1 per row of this order-book file (§4.4) |
| `id` | int32 | Instrument id (§4.3) |
| `type` | char | Class of the event that produced the row (table below) |
| `side` | char | `B`, `S` or `N` (table below) |
| `bid0`…`bid4` | int64 | Best five bid prices, best (highest) first, scaled (§4.2) |
| `bid_size0`…`bid_size4` | int64 | Quantities at those bids, scaled |
| `ask0`…`ask4` | int64 | Best five ask prices, best (lowest) first, scaled |
| `ask_size0`…`ask_size4` | int64 | Quantities at those asks, scaled |

Missing levels are written as price `0` and size `0`. This only happens when the exchange side
itself has fewer than five levels: a row is never emitted while a visible level could be unknown
(§5.3).

| `type` | Event | `side` |
|---|---|---|
| `R` | REST snapshot applied: a synchronisation completed (§5.4) or a coverage refresh was swapped in (§5.5) | `N` |
| `D` | Differential depth update (`depthUpdate` on `@depth@100ms`) | `B` if the update has only bid levels, `S` if only ask levels, `N` if both or neither |
| `P` | Partial depth snapshot (`@depth5@100ms`) | `N` |
| `T` | Trade (annotation; the book is unchanged) | Book side of the resting (maker) order: `m=true` (buyer is maker) → `B`; `m=false` → `S` |

Example (`P` row built from the spot depth5 payload in the live probe; `tnsec` and `seqNo`
illustrative):

```
1791669450,431220517,1834,2014473836,P,N,8304400000000,8304399000000,8304398000000,8304397000000,8304396000000,325216000,55000,227000,21428000,6034000,8304401000000,8304402000000,8304403000000,8304410000000,8304423000000,57036000,76000,13000,8000,14000
```

### 3.5 Depth-snapshot sidecar

Header (exact):

```
recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_json
```

- One row per REST depth snapshot received for the symbol, whether or not it was used.
- `shard_id` and `conn_epoch` identify the symbol's connection when the response was received
  (its latest established epoch). Processing uses them exactly like a market-data row's (§5.7).
- `apply_before_row` is the number of data rows already written to the paired market-data file
  when the processor consumed the snapshot, i.e. the 0-based index of the market-data row it
  precedes. A value equal to the file's row count means after the last row. Rows are written in
  processing order, so the column is non-decreasing; replay rejects a sidecar where it is not.
- `payload_json` is the minified REST response body (`{"lastUpdateId":…,"bids":[…],"asks":[…]}`,
  plus `E`/`T` on usdm).

## 4. Policies

### 4.1 Timestamps

- **Receive time** = `std::chrono::system_clock::now()` taken in the WebSocket (or REST) read-
  completion handler, after the whole message has been read and before any parsing. Nanoseconds
  since the Unix epoch, UTC (CLOCK_REALTIME on Linux).
- `recv_tsec = recv_ns / 1 000 000 000`, `recv_tnsec = recv_ns % 1 000 000 000`.
- An order-book row uses the receive time of the record whose processing produced it (§5.8).
- Binance's own times (`E`, `T`) remain available inside `payload_json`. Receive time was chosen
  because spot `depth5` payloads carry no event time, it is uniform across the three streams, and
  replay reproduces it exactly.
- Wall-clock time can step (NTP), so timestamps are not guaranteed monotonic. Ordering is defined
  by row order (`conn_seq`, `seqNo`), never by timestamps.

### 4.2 Integer scaling

- Prices **and** quantities are scaled by 10^8: `kPriceScale = kQtyScale = 100'000'000`
  (`constexpr`). Spot uses 8 decimals; usdm uses fewer (BTCUSDT: 2 price, 3 quantity).
- Binance decimal strings are converted to `int64` by an exact parser. No binary floating point
  is used anywhere on the path to a CSV integer.
- Grammar: `[0-9]+(\.[0-9]+)?`. Fraction digits beyond the 8th must be `0`. The result must not
  exceed `INT64_MAX` (integer part ≤ 92 233 720 368).
- Rejected inputs — empty string, sign, exponent, any other character, a non-zero 9th+ decimal,
  overflow — are parse errors: counted, and handled per §5.7. Values are never rounded.

| Input | Scaled int64 |
|---|---|
| `"83044.00000000"` | `8304400000000` |
| `"3.25216000"` | `325216000` |
| `"0.00009000"` | `9000` |
| `"83000.10"` | `8300010000000` |
| `"0.022"` | `2200000` |

### 4.3 Instrument id

`id = FNV-1a-32("<venue>:<SYMBOL>") & 0x7FFFFFFF`, computed over the ASCII bytes. Examples:
`spot:BTCUSDT` → `2014473836`, `usdm:BTCUSDT` → `1567771867`, `spot:ETHUSDT` → `1884648048`.
The id depends only on venue and symbol (not on argument order or process), so capture and replay
agree. Startup rejects a symbol list containing two symbols with the same id.

### 4.4 Sequence counters

| Counter | Scope | First value | Increment |
|---|---|---|---|
| `shard_id` | process | 0 | Symbols are split into shards in `--symbols` order, `--symbols-per-connection` per shard: shard *k* holds symbols *kN* … *kN+N−1* |
| `conn_epoch` | per shard | 0 | Each successful (re)connection (WebSocket handshake complete) |
| `conn_seq` | per (shard, epoch) | 0 | Each WebSocket data frame received. A frame rejected as malformed, or dropped because the queue was full, consumes its number, so losses are visible as gaps |
| `seqNo` | per order-book file | 0 | Each order-book row |

## 5. Order book

### 5.1 Data structure

Each side is a `std::vector<Level{int64 price; int64 qty;}>` kept sorted so that the **best price
is at the back**: bids ascending, asks descending.

- **Search:** galloping (exponential) search from the back, then binary search inside the
  bracket: O(log k), where *k* is the number of levels better than the price.
- **Insert, overwrite, erase:** only the *k* elements between the position and the back move, so
  one level update costs O(k) and a message costs the sum over its levels. Updates cluster at the
  touch, where *k* is small; a deep insert or delete costs up to O(n) contiguous moves
  (n ≤ 5 000 levels of 16 bytes). This is not O(1) per update in the worst case, and the
  benchmark (§10) measures deep updates and deletions as well as near-touch traffic.
- **Top-5 read:** O(1) per level, from the back.
- **Cap:** 5 000 levels per side. The cap is checked after every insertion; an insertion that
  takes a side to 5 001 levels trims the worst levels in one batch down to 4 000 and adjusts
  coverage (§5.3).
- **Storage:** each side reserves 5 001 levels (the cap plus one insertion) once, when the symbol
  is set up at startup. Because the trim check runs after every insertion, a side never needs
  more, so loading snapshots and applying diffs never reallocate.

| | Flat vector, best at back (chosen) | `std::map<int64,int64>` |
|---|---|---|
| Update near touch | O(log k) search + k contiguous moves; no allocation | O(log n) pointer chasing + node allocation on insert |
| Deep update or delete | Up to O(n) contiguous moves (16-byte elements) | O(log n) |
| Top-5 read | 5 contiguous loads | 5 node hops |
| Allocation per message | None (storage reserved at startup) | One per inserted level |

### 5.2 Event semantics

- **Diff** (`depthUpdate` on `@depth@100ms`): each `[price, qty]` is the **absolute** quantity at
  that price; `qty == 0` erases the level. Erasing a level that is not in the book is a no-op
  (Binance documents this as normal). Levels are applied in payload order.
- **REST snapshot**: replaces the whole book and resets coverage (§5.3).
- **depth5** (`@depth5@100ms`): the exact top 5 levels per side at update id *p* (spot:
  `lastUpdateId`; usdm: `u`). Its semantics are **replace**, not merge: a level that left the top
  5 is simply absent — it is never sent with `qty 0` — so depth5 must never be applied like a
  diff. Because the REST + diff book holds strictly more information, this design never writes
  depth5 into the book; it uses depth5 only as an exact cross-check (§5.6). Patching the top of
  the book from depth5 would require trusting the levels below the replaced range, which a
  disagreement has just shown cannot be trusted. A depth5 payload with more than 5 levels on a
  side is a parse error.
- **Trade**: never changes the book. Fills already reach the book through the diff stream;
  applying trades as well would double-count. Trades produce annotation rows only (§5.8).
- usdm `depth5` payloads also carry `"e":"depthUpdate"`; the event class is therefore taken from
  the **stream name**, never from the `e` field.

### 5.3 Coverage

A REST snapshot holds at most `limit` = 1000 levels per side. Measured on 2026-10-11, that
covered only about 0.24 % of the BTCUSDT price on spot ($197 of bids) and 0.15 % on usdm ($125),
a range the price can cross within minutes. Binance documents that levels outside the snapshot
stay unknown until they change. The book therefore tracks how far it is known to be complete.

- A side whose snapshot has **fewer than `limit` levels is complete**: the snapshot holds the
  entire side.
- Otherwise its **coverage boundary** is the snapshot's worst price on that side (lowest bid,
  highest ask). Every price at or better than the boundary is known exactly. Beyond it, only
  prices that later diffs touched are known; untouched resting levels may exist there.
- Diffs never move the boundary: a diff beyond it reveals only the prices it names, not the
  levels in between.
- **Trimming** (§5.1) has two cases:
  - **Complete side:** any trim revokes completeness. The side becomes bounded, with its boundary
    at the worst retained price, because evicted levels still exist on the exchange.
  - **Bounded side:** if the trim removed any level at or better than the boundary, the boundary
    moves to the worst retained price; otherwise it is unchanged.
- A side's **coverage margin** is the number of levels at or better than its boundary (unbounded
  for a complete side).
- A side is **authoritative** if it is complete or its margin is at least 5. Only then can its
  top 5 be emitted: every level that could appear in the top 5 is known. Zero padding (§3.4)
  can therefore only occur on a complete side, and means the exchange has fewer levels.
- After processing any record, if either side is not authoritative, the book becomes Unsynced
  (counted as `coverage_exhausted`): no row is emitted for that record and a resync starts
  (§5.4).
- When either margin falls below 200 levels, a background refresh is requested
  (**enhancement**, §5.5), so that in normal operation coverage is renewed before it runs out.

### 5.4 Synchronisation state machine

Each symbol's book is either **Unsynced** (initial state; no order-book rows) or **Synced**.
Synchronisation follows Binance's documented procedure for each venue, using a REST snapshot with
`limit=1000`: spot `GET https://api.binance.com/api/v3/depth?symbol=S&limit=1000`, usdm
`GET https://fapi.binance.com/fapi/v1/depth?symbol=S&limit=1000`.

Let *L* be a snapshot's `lastUpdateId` and *id* the book's last applied update id.

**Sequence classification.** One function classifies a diff (`U`, `u`, and on usdm `pu`) against
a reference id *r*. It is used everywhere — appending to the window, applying to a Synced book,
and replaying the window — so the rules cannot drift apart:

| Venue | Stale | Gap | Continues |
|---|---|---|---|
| spot | `u ≤ r` | `U > r + 1` | otherwise (`U ≤ r + 1 < u + 1`; overlapping coverage is accepted, as Binance's update rule allows) |
| usdm | `u ≤ r` | `pu ≠ r` | otherwise |

**Recent-diff window.** Each symbol keeps the most recent contiguous diffs, decoded into owned
integers, in a circular arena of 20 000 levels and 2 000 diffs, reserved at startup. A diff
that does not fit is admitted by dropping the oldest diffs (counted as `window_trimmed`).
Every accepted diff fits: a diff with more than 20 000 levels is rejected while parsing
(§6.5) and handled as a gap, never truncated. A diff is classified against the window's last
`u`: stale → not appended (counted as `stale_diffs`); gap → the window is cleared and restarts
with this diff (counted as `window_resets`); continues → appended. While Unsynced the window is
the sync buffer. While Synced it holds the applied diffs that a background refresh needs (§5.5).
Dropping the oldest entries is safe because a usable snapshot is always newer than them; if it is
not, the snapshot is rejected as too old and a fresh one is requested.

**Unsynced:**

1. A diff is appended to the window, then **try_sync** runs.
2. A snapshot becomes the *pending* snapshot (a newer one replaces an older one), then
   **try_sync** runs.
3. A depth5 or trade record is ignored (no row).
4. **try_sync**, with a pending snapshot and a non-empty window:
   1. Find the first window diff the snapshot does not already contain — spot: first with
      `u > L`; usdm: first with `u ≥ L`. If there is none, wait for more diffs.
   2. That diff must bridge the snapshot — spot: `U ≤ L + 1 ≤ u`; usdm: `U ≤ L ≤ u`. If its `U`
      is larger, the snapshot is too old: discard it, request a new one (counted as
      `snapshot_too_old`).
   3. Otherwise build the book: load the snapshot (setting coverage, §5.3), apply the bridging
      diff and every later window diff in order, set *id* to the last applied `u`. If both sides
      are authoritative, switch to Synced and emit one `R` row (§5.8); if not, discard the
      snapshot and request a new one.
5. The processor requests a snapshot when the symbol enters Unsynced and whenever it discards one.
   At most one request per symbol is outstanding. Request bookkeeping never influences output,
   which keeps replay exact (§7).

The spot bridge condition is written `U ≤ L + 1 ≤ u` to agree with Binance's update rule (an
event is valid while `U ≤ id + 1`); it accepts an event that starts exactly at `L + 1`.

**Bootstrap.** Diffs that arrive while Unsynced are bootstrap input, not processed events. They
are recorded in the market-data CSV as they arrive, but applied only when a snapshot bridges them,
as one atomic synchronisation step. That step is followed by exactly one `R` row, whose timestamp
is the receive time of the record that completed it: the snapshot record, or a later diff that
supplied the bridge. The brief's "row after every applied depthUpdate" is read as applying to
updates applied to a synchronised book; the bootstrap batch is represented by its `R` row.

**Synced** — for each diff, classified against *id*:

- Stale: the book is unchanged (counted).
- Gap: §5.7.
- Continues: apply it, set `id = u`, append it to the window.

### 5.5 Background coverage refresh (enhancement)

Implemented after the mandatory behaviour (§5.3 exhaustion → resync) is complete and tested. It
renews coverage without interrupting output.

- **Trigger:** Synced, either coverage margin below 200, and no refresh in progress → request a
  snapshot.
- **Shadow sync:** when the refresh snapshot arrives, **try_sync** (§5.4) runs on a *shadow* book,
  with the window of applied diffs as its buffer. If the snapshot is newer than every window diff,
  it stays pending and try_sync re-runs after each subsequently applied diff. If it is too old,
  it is discarded and re-requested.
- **Swap:** when the shadow synchronises it has applied the same diffs as the live book, so both
  are at the same *id*. Their top 5 levels are compared:
  - Equal → the shadow (with fresh coverage) replaces the live book (counted as `refresh_ok`).
  - Different → the shadow still replaces the live book, because it was built by the documented
    procedure from fresher data; counted as `refresh_mismatch` and logged with both level sets.
- **Rows:** a snapshot record that completes a swap emits an `R` row; a diff that completes it
  emits its own `D` row, showing the post-swap state.
- If coverage runs out before the swap, §5.3 applies: the shadow is discarded and the pending
  refresh snapshot becomes the resync's pending snapshot.

### 5.6 depth5 cross-check

depth5 never modifies the book. While Unsynced it is ignored. While Synced, with *p* the depth5
id:

| Case | Action |
|---|---|
| `p < id` | Stale: no check, counted as `depth5_stale` |
| `p == id` | **Full check** now |
| `p > id` | Becomes the *pending check*. A newer depth5 replaces an older pending one, counted as `depth5_unchecked` |

A pending check is resolved when the next diff *d* = (`U`, `u`) is applied:

| Position of *p* | Check |
|---|---|
| `p < U` | usdm update ids are not contiguous; the book at *id* equals the state at *p* → **full check** before applying *d* |
| `U ≤ p < u` | **Partial check** before applying *d*: compare only prices that *d* does not touch |
| `p == u` | **Full check** after applying *d* |
| `p > u` | Keep pending |

- **Full check:** on each side the book's best levels equal the depth5 levels (same count,
  prices and quantities). A full check always runs on a book state that has passed the
  coverage rule, so the book's top 5 is authoritative and every level compared lies within
  coverage. After *d* is applied, coverage is evaluated first; if the book became Unsynced, the
  pending check is discarded.
- **Partial check:** compares only the prices that lie inside both depth5's price range and the
  book's trusted coverage on that side (at or better than the boundary; the whole range on a
  complete side), and that *d* does not touch. Each such price must have the same presence and
  quantity in the book and in depth5. Prices not touched by *d* did not change between *id* and
  *u*, so their state at *p* is known exactly.
  - depth5 describes an intermediate state at *p*, so its 5th level can lie beyond coverage
    even when the book's own top 5 do not. For example: trusted bids 10–6 with boundary 6, and
    *d* deletes 10 and adds 11. depth5 then shows 9–5, and 5 was never known.
  - Prices outside coverage are therefore never compared; they are counted as
    `depth5_levels_unchecked`. Top-5 authority does not extend to a wider range.
- **Match:** counted as `depth5_verified` or `depth5_partially_verified`.
- **Mismatch:** counted as `depth5_mismatch`; the first 10 are logged with both level sets. The
  book has been shown wrong somewhere, possibly below the depth5 range, so it becomes Unsynced and
  resynchronises from REST (§5.7). No row is emitted for the record whose processing found the
  mismatch.

Live probe (20 s, BTCUSDT, 2026-10-11):

- **spot:** every depth5 id equalled some diff's `u` (188/188), and depth5 usually arrived one
  message before that diff, so the *full check after applying d* path applies.
- **usdm:** depth5 ids never equalled a diff's `u` (0/130), so the *partial check* path applies.

### 5.7 Gaps, reconnects and errors

All state below is per symbol. No event for one symbol changes another symbol's state.

| Situation | Effect |
|---|---|
| A record (market-data or snapshot) of the symbol carries a `conn_epoch` greater than the last one the symbol saw | Reset before processing the record: book cleared, window, pending snapshot, pending depth5 check and shadow refresh discarded, state Unsynced, snapshot requested. The record is then processed normally |
| Diff gap while Synced | Unsynced; the window restarts with the gap diff; snapshot requested; counted as `gaps` |
| Diff payload parse error | Treated as a gap: a missing diff can't be skipped safely |
| depth5 or trade payload parse error | Counted; no row; book unchanged |
| Payload `s` ≠ stream symbol | Parse error (defensive) |
| Coverage exhausted | Unsynced and resync (§5.3) |
| depth5 mismatch | Unsynced and resync (§5.6) |
| Snapshot too old or not authoritative | Discarded; a new one is requested (§5.4) |
| REST request failure | Retried by the scheduler (§6.3) |
| Envelope invalid, stream not subscribed, or ring full | Counted; no market-data row; its `conn_seq` number stays unused |

Epochs are tracked per symbol, not per shard. A symbol has no market-data records between a
disconnect and its own first record on the new connection, so resetting at that record discards
no data. A snapshot received after the reconnect carries the new epoch and triggers the reset
itself. This makes each symbol's output a function of its own records only (§7). A shard-wide
reset, triggered by whichever symbol's record arrives first, would make a symbol's output depend
on other symbols' timing, which per-symbol files could not reproduce.

The brief's options were "reset, resync, or drop until snapshot". This design resets the book
and drops order-book output until a REST snapshot resynchronises it, on every new epoch, gap,
coverage exhaustion and depth5 mismatch.

### 5.8 Row emission rules

- **Unsynced:** no rows.
- **Completing a synchronisation** (Unsynced → Synced): exactly one `R` row, timestamped with the
  record that completed it (§5.4).
- **While Synced, and still Synced afterwards:** one row per successfully parsed record of the
  symbol, showing the book after processing it:
  - market-data records → `D`, `P`, `T` (`T` omitted with `--no-trade-rows`), including stale
    diffs and depth5 records that leave the book unchanged;
  - a snapshot record that completes a background refresh → `R`;
  - any other snapshot record → no row.
- **A record whose processing makes the book Unsynced** (gap, diff parse error, coverage
  exhaustion, depth5 mismatch) → no row.
- Consequently every order-book file starts with an `R` row, and every row shows a book that
  passed the sequence, coverage and cross-checks.

## 6. Architecture

### 6.1 Threads and data flow

```
 io thread (main thread, Boost.Asio io_context)        processing thread (single consumer)
 ┌────────────────────────────────────────────┐        ┌──────────────────────────────────────────┐
 │ WsShard ×N: resolve→TCP→TLS→WS→read loop    │        │ for each record, in order:               │
 │   recv_ns, conn_seq, envelope split ───────┼─┐      │  1. per-symbol epoch check (§5.7)        │
 │ RestScheduler: paced HTTPS snapshots ──────┼─┤ SPSC │  2. write market-data / sidecar row      │
 │ signal_set, --duration, reconnect timers   │ ├─ring─►  3. parse payload once (simdjson)        │
 │                                            │ │      │  4. sync / book / cross-check (§5)       │
 │ asio::post ◄── snapshot requests, fatal ───┼─┼──────┤  5. write order-book row (§5.8)          │
 └────────────────────────────────────────────┘ │      │  periodic: flush (1 s), stats line       │
                                                │      └──────────────────────────────────────────┘
 replay: CSV reader + sidecar injection ────────┘  (same processor; no io thread, no sockets)
```

| Thread | Owns | Never does |
|---|---|---|
| io | `io_context`, shards, REST scheduler, signal set, timers; envelope split; ring producer | Payload parsing, book updates, file I/O |
| processing | Ring consumer, simdjson parser, books, writers, metrics output | Socket I/O |

The threads share exactly three things:

1. The SPSC ring (acquire/release atomics) and its producer-closed flag.
2. `asio::post` from the processor to the io thread (snapshot requests, fatal-error shutdown).
3. Relaxed atomic metric counters.

No mutex is taken per message. The only mutex is on the ring's park/notify path, used when the
consumer is idle.

### 6.2 Network layer

| Purpose | Endpoint |
|---|---|
| spot streams | `wss://stream.binance.com:9443/stream?streams=<list>` |
| usdm streams | `wss://fstream.binance.com/public/stream?streams=<list>` (`@trade` verified on `/public`) |
| spot snapshot | `https://api.binance.com/api/v3/depth?symbol=<SYMBOL>&limit=1000` |
| usdm snapshot | `https://fapi.binance.com/fapi/v1/depth?symbol=<SYMBOL>&limit=1000` |

`<list>` is `btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade/…`, with symbols lowercase.

- **WsShard** (one per connection, `std::enable_shared_from_this`; every async handler holds a
  `shared_ptr` to its shard, so no callback can outlive its object): async resolve → TCP
  connect → TLS handshake → WebSocket handshake → read loop.
- **TLS:** `ssl::context::tls_client`, TLS ≥ 1.2, system CA store (`set_default_verify_paths`),
  `verify_peer`, `ssl::host_name_verification`, SNI. Verification is never disabled.
- **WebSocket:** `read_message_max` 4 MiB; 10 s handshake timeout; 30 s idle timeout with
  keep-alive pings. Beast answers server pings (spot every 20 s, usdm every 3 min) automatically
  and reassembles fragmented messages, so the processor only ever sees whole messages.
- **Per frame:** take `recv_ns` and the next `conn_seq`; split the envelope; look up the stream
  name → (symbol index, stream kind) in the shard's table; push the record to the ring.
- **Envelope split, fast path:** a validated byte scan, not a JSON parse. The frame must start
  with `{"stream":"`, the name ends at the next `"`, which must be followed by `,"data":`; the
  payload runs from there to the final `}` and must itself start with `{` and end with `}`.
- **Envelope split, fallback:** a frame that fails the fast path (for example `data` before
  `stream`) is parsed with simdjson On-Demand in a single pass over the envelope's fields,
  dispatching on the key, so field order does not matter: `stream` must be a string, `data` is
  captured with `raw_json()`, and each must appear exactly once. Anything else is counted as
  malformed. A reversed-envelope fixture covers this path.
- **Reconnect:** any read error, close or timeout schedules a reconnect with backoff (500 ms,
  doubling, capped at 30 s, ±20 % jitter, reset after 60 s connected). This also covers
  Binance's 24-hour disconnect. A successful handshake increments `conn_epoch`. Attempt rates
  stay far below spot's 300 connections per 5 minutes per IP.
- **`--fault-reconnect-every N`:** a timer force-closes each connection every N seconds.

### 6.3 REST snapshot scheduler

One scheduler per process: a process serves one venue, so all its requests share one IP weight
budget. Limits verified live on 2026-10-11 via `exchangeInfo` and `X-MBX-USED-WEIGHT-1M`:

| Venue | `REQUEST_WEIGHT` limit | Our budget (50 %) | Snapshot weight (`limit=1000`) |
|---|---|---|---|
| spot | 6 000 / min | 3 000 / min | 50 |
| usdm | 2 400 / min | 1 200 / min | 20 |

- **Pacing:** a token bucket with capacity = budget, refilled at budget / 60 per second. A
  request is dispatched only when the bucket holds its weight and fewer than 4 requests are in
  flight. Requests are served FIFO; a symbol has at most one queued or in-flight request.
- **Reconciliation:** every response's `X-MBX-USED-WEIGHT-1M` is read. If the server-reported
  usage leaves less than one request's weight below our budget, dispatch pauses until the start
  of the next minute.
- **429 / 418:** a global cooldown pauses all dispatch for `Retry-After` seconds (60 s if
  absent); the failed request is requeued at the head. 418 (IP ban) is also logged as an error.
  No request is sent during a cooldown.
- **Transport errors and timeouts** (10 s per request; body ≤ 2 MiB): requeued with per-request
  backoff (500 ms → 30 s), still subject to the bucket and any cooldown.
- **Delivery:** a `200` response is pushed to the ring as a snapshot record, stamped with the
  symbol's shard and current `conn_epoch`. If the ring is full, the push is retried on a 10 ms
  timer.
- **Burst cost:** one symbol synchronises immediately. At the 256-symbol limit, initial spot
  synchronisation needs 12 800 weight, so the last symbols synchronise after about 4 minutes;
  usdm needs 5 120 weight, also about 4 minutes. The sliding window (§5.4) bounds the memory of
  symbols waiting their turn.

### 6.4 Record queue (SPSC ring)

- One pre-allocated byte ring between the io thread and the processor. Its capacity is a power
  of two, as the position arithmetic requires: 16 MiB for 1–64 symbols, 32 MiB for 65–128, and
  64 MiB for 129–256 (256 KiB per symbol at the top of each band).
- Each record is contiguous and 64-byte aligned: a fixed header, the payload, then
  `SIMDJSON_PADDING` bytes so simdjson parses the payload in place. A record that does not fit
  before the end of the buffer is preceded by a wrap marker.
- Header: record kind (`market`, `snapshot`), `shard_id`, `conn_epoch`, `conn_seq`, `recv_ns`,
  `steady_ns` (for the latency metric), symbol index, stream kind, payload length.
- Positions are monotonically increasing 64-bit counters (capacity a power of two), published
  with release and read with acquire, on separate cache lines; each side caches the other side's
  position to avoid cache-line ping-pong.
- **Consumer wait:** spin briefly, then park on a condition variable with a 50 ms timeout (so
  periodic flushes and stats still run). The producer notifies only when the consumer is parked,
  so the busy path never takes a lock.
- **Full ring:** the producer drops the newest frame and counts `ring_drops`; it never blocks the
  io thread. The drop is visible as a `conn_seq` gap in the market-data CSV. A dropped diff
  produces a sequence gap, which the book handles by resyncing (§5.7). At the measured BTCUSDT
  rate (spot ≈ 7 KiB/s, usdm ≈ 15 KiB/s) a single-symbol ring covers about 18 minutes of
  processor stall.
- **Why a custom ring:** variable-length records are packed back to back, so memory is used in
  proportion to actual payload sizes. Each payload is contiguous and padded, so it can be parsed
  in place. A proven alternative — `boost::lockfree::spsc_queue` of slot indices plus a
  fixed-total-size buffer pool — would also enforce a hard memory cap. But each slot must either
  be sized for the largest message (wasting most of the pool on ~1–6 KB frames) or chain
  fixed-size chunks (making payloads non-contiguous, so they need copying before simdjson can
  parse them).
- **Risk control:** the ring is the riskiest component. It is the first task of M4, built and
  tested on its own before anything is wired to it. Its unit tests
  cover every wrap case, plus a randomised length fuzz and a two-thread stress test run under
  TSan. If it cannot be made to pass these, the fallback is the `spsc_queue` + pool design, with
  a per-slot size cap.

### 6.5 Processor

For each record, in ring order:

1. Apply the per-symbol epoch rule (§5.7).
2. Market record: write the market-data row (minify if needed, escape directly into the writer
   buffer). Snapshot record: write the sidecar row.
3. Parse the payload once into a reused typed event: `DepthDiff`, `Depth5` (fixed arrays of 5
   levels), `Trade` or `DepthSnapshot`.
4. Run the sync, book and cross-check logic (§5), then emit an order-book row when §5.8 says so.
5. Release the record.

**simdjson rules:**

- **Padding:** every payload is parsed in place from storage with at least `SIMDJSON_PADDING`
  readable bytes after it (ring records reserve them; replay reads into a padded buffer).
- **Traversal:** one forward pass per document. The object's fields are iterated once and
  dispatched on the key; unknown fields (`ps`, `st`, `X`, `M`, …) are skipped. `find_field` is
  never used, because it only searches forward and would make parsing depend on field order.
- **Validation:** On-Demand validates only what it visits, so every field is visited. Required
  fields must appear exactly once with the expected types, levels must be arrays of exactly two
  strings, and the document must be fully consumed (`at_end()`). Any simdjson error is a parse
  error.
- **Decoded-size limits.** JSON size does not bound decoded size: `["1","1"]` is about 10
  bytes of JSON but a 16-byte level. The parser therefore counts levels as it decodes them and
  stops at the limit, so the decode scratch (reserved at startup) never grows:

  | Event | Limit | Over the limit |
  |---|---|---|
  | Diff | 20 000 levels, bids + asks (the window's capacity) | Counted as `oversized_events`; handled as a diff parse error, i.e. a gap → resync (§5.7). Never truncated |
  | depth5 | 5 levels per side | Parse error |
  | REST snapshot | 1 000 levels per side (the requested `limit`) | Counted as `oversized_events`; snapshot discarded and re-requested |
  | Trade | — | — |

  Live traffic is far below these limits (largest diff in the probe: 154 levels).
- **Ownership:** decoded values are copied into owned integers (events, window, pending
  snapshot, pending depth5 check) before the record is released. No `string_view`,
  `ondemand::value` or document outlives the record it came from; the parser is reused for the
  next record.

The processor owns no timers. Periodic work (flush every 1 s, stats every `--stats-interval`) is
checked against a steady clock between records and on park timeouts; none of it affects output.

### 6.6 Writers and back-pressure

- Market-data and order-book files each have a 64 KiB user-space buffer over an unbuffered
  `std::FILE` (RAII handle). Sidecar rows (rare, about 60 KB each) are written straight through.
  Rows are formatted with `std::to_chars` straight into the buffer: no temporary strings.
- Buffers are flushed when full, every 1 s, and at shutdown.
- The result of every write, `fflush` and `fclose` is checked. Any failure is fatal (§6.7) and
  the exit code is 1.
- **Durability:** a flush hands data to the operating system; `fsync` is not used. A process
  crash loses at most the last second of unflushed rows. An operating-system crash or power loss
  can lose more, since data in the page cache is not guaranteed durable.
- **Back-pressure chain:** disk stall → processor stalls → the ring absorbs it → if the ring
  fills, frames are dropped and counted (§6.4). The socket thread never waits on disk.
- A dedicated writer thread was rejected: formatting is the dominant cost and stays on the
  processor, and buffered writes already amortise system calls. The ring already decouples the
  socket from disk. The benchmark (§10) records the processor's per-record cost to back this up.

### 6.7 Shutdown protocol

State:

- `stopping` — io thread only.
- The ring's `producer_closed` flag — atomic.
- `fatal` — processor only.
- The exit code — atomic.

Steps:

1. **First trigger** (SIGINT/SIGTERM, the `--duration` timer, or a fatal error posted by the
   processor) runs `begin_shutdown()` on the io thread:
   - set `stopping`;
   - cancel timers;
   - the scheduler stops dispatching and cancels in-flight requests;
   - every shard starts `async_close` with a 2 s timeout, and no reconnect is scheduled;
   - a 3 s deadline timer calls `io_context::stop()` if anything hangs.

   A read that completes after `stopping` was set may still push its record: that data was
   received and is kept.
2. **Second SIGINT/SIGTERM** calls `io_context::stop()` immediately.
3. **Closing production:** when `io_context::run()` returns (either way), no io handler can run
   again. main then calls `ring.close_producer()` (release store plus notify). This is the only
   termination signal. It needs no ring space, so a full ring cannot delay it, and nothing can
   push after it because only io handlers push.
4. **Consumer termination:** the processor pops until the ring is empty. When empty, it checks
   `producer_closed` (acquire); if set, it drains once more and finishes. Every record pushed
   before the close is processed.
5. **Processor finish:** flush and close every file (results checked), print the final summary,
   exit the thread.
6. **Teardown:** main joins the processor, releases its shard and scheduler references, and
   destroys the `io_context`, which destroys any handlers that never ran and with them the last
   `shared_ptr`s. The exit code is 1 if the processor hit a fatal error or any close failed, 2
   for usage errors, otherwise 0.

**Writer failure:** the processor sets `fatal`, stops writing (later records are drained and
discarded), and posts `begin_shutdown` with exit code 1. `asio::post` is thread-safe; a post that
arrives after `run()` returned is destroyed without running, which is harmless.

## 7. Replay mode and determinism

```
binance_capture --replay output/market_data_spot_BTCUSDT_2026-10-11.csv --output-dir replay_output
```

- Each input file is replayed independently, with fresh processor state and `seqNo` from 0: a
  file is one run of one symbol (§3.1). Files of different symbols or runs may be given in any
  order.
- Each file's header must match §3.3 exactly. Rows are parsed by an RFC 4180 reader (quoted
  fields, `""`, embedded separators/newlines, LF or CRLF) into the same record structure the ring
  carries.
- The sidecar `depth_snapshots_<venue>_<SYMBOL>_<date>.csv` must be in the same directory. Each
  of its rows is injected immediately before market-data row `apply_before_row`, carrying its
  `shard_id` and `conn_epoch`. A missing sidecar is an error (the book could never synchronise).
- Records feed the **same processor**, with the market-data and sidecar writers disabled and a
  no-op `SnapshotRequester`.
- Replay never constructs an `io_context`, socket or TLS context. CI verifies this with
  `strace -f -e trace=socket` (no `socket()` calls).

**Determinism argument.**

- Processor state is partitioned by symbol: epoch tracking, window, book, coverage, pending
  checks, `seqNo` and files are all per symbol, and nothing reads another symbol's state.
- Within a symbol, the logic has no timers, randomness or I/O-dependent branches, and
  snapshot-request bookkeeping cannot change output.
- A symbol's order-book output is therefore a pure function of its own ordered record sequence:
  its market-data records plus its snapshot records.
- The market-data file stores those records in processing order, and `apply_before_row` places
  each snapshot record exactly. Replay rebuilds the identical sequence and so produces
  byte-identical order-book files.
- CI enforces this on the committed sample run with `cmp`. Both runs must use the same
  `--no-trade-rows` setting.

**What this proves, and what it does not.** Live = replay proves the two executions are
consistent, not that the book is correct. Correctness evidence comes from independent sources:

- hand-derived expected outputs and the exchange-simulator property test (§13);
- the live depth5 cross-check against Binance's own top 5 (§5.6).

## 8. Observability

- **io-thread counters** (relaxed atomics):
  - `frames_received` and bytes per stream kind, `malformed_frames` (invalid envelope or
    unsubscribed stream), `ring_drops`;
  - `connects`, `reconnects`, `connect_failures`;
  - `rest_requests`, `rest_failures`, `rest_cooldowns`, `rest_weight_used`.
- **Processor counters:**
  - rows: `md_rows`, `ob_rows`, `snapshot_rows`;
  - sequencing: `parse_errors`, `oversized_events`, `stale_diffs`, `gaps`, `window_resets`,
    `window_trimmed`, `syncs`, `resyncs`, `snapshot_too_old`, `coverage_exhausted`;
  - refresh: `refresh_ok`, `refresh_mismatch`;
  - depth5: `depth5_verified`, `depth5_partially_verified`, `depth5_mismatch`, `depth5_stale`,
    `depth5_unchecked`, `depth5_levels_unchecked`;
  - other: `payloads_minified`, `ring_high_water`, `retained_bytes` (capacity of all
    per-symbol containers plus fixed buffers);
  - a fixed-bucket log2 histogram of receive → row-written latency (steady clock).
- **Stats line** on stderr every `--stats-interval` seconds, `key=value` for `grep`/`awk`:
  `stats uptime_s=10 frames=394 md_rows=394 ob_rows=377 parse_errors=0 gaps=0 resyncs=0
  reconnects=0 coverage_exhausted=0 depth5_verified=187 depth5_mismatch=0 ring_drops=0
  ring_hw_kib=12 retained_kib=1830 lat_p50_us=4 lat_p99_us=21`
- **Final summary** at exit: the same counters, totals and per symbol, plus peak RSS (`VmHWM`
  from `/proc/self/status`).
- **Frame accounting invariant:** for every (shard, epoch),
  `frames_received = md_rows + malformed_frames + ring_drops`. Every `conn_seq` number from 0
  to `frames_received − 1` therefore appears exactly once in the market-data CSVs or is
  accounted for by one of the two loss counters. It is checked in two places:
  - the final summary checks the totals;
  - `verify_outputs.sh` checks each (shard, epoch) against the `epoch_closed shard=… epoch=…
    frames=… malformed=… drops=…` line the io thread logs when a connection ends or at shutdown.
- **Logging:** stderr, levels `info`/`warn`/`error`, ISO-8601 UTC timestamps. Repeated errors
  are rate-limited (first 10, then every 1000th). Nothing is logged per message on the normal
  path.

## 9. Limits, memory budget and security

| Limit | Value | On violation |
|---|---|---|
| Symbols per process | 256 | Usage error |
| Streams per connection | 1 024 on both venues (341 symbols) — §15 | Usage error |
| Stream URL length | 8 192 bytes (often the binding limit: ≈ 94 bytes per 20-character symbol) | Usage error |
| WebSocket message | 4 MiB | Frame rejected (Beast error) → reconnect, counted |
| REST body | 2 MiB (a `limit=1000` snapshot is ≈ 64 KB spot, ≈ 42 KB usdm) | Request failed → retry, counted |
| Decoded diff | 20 000 levels | Rejected, handled as a gap (§6.5) |
| Decoded REST snapshot | 1 000 levels per side | Rejected, re-requested (§6.5) |
| Recent-diff window | 20 000 levels / 2 000 diffs per symbol | Oldest dropped, counted |
| Book side | 5 000 levels | Trimmed to 4 000, coverage adjusted |
| Concurrent REST requests | 4, plus weight pacing (§6.3) | Queued |

**Memory budget.** Every container below is reserved at startup and never grows (§10), so these
figures are both the steady state and the maximum.

| Item | Size | Scope |
|---|---|---|
| Ring (§6.4) | 16 / 32 / 64 MiB by symbol count | Fixed |
| simdjson parser (capacity 4 MiB, ≈ 5× capacity) | ≈ 20 MiB | Fixed |
| Decode scratch (20 000 diff levels + 2 000 snapshot levels, 16 B each) | ≈ 344 KiB | Fixed |
| Writer buffers (2 × 64 KiB) | 128 KiB | Per symbol |
| Books: live + refresh shadow, each 2 sides × 5 001 levels × 16 B | ≈ 313 KiB | Per symbol |
| Recent-diff window (20 000 levels × 16 B + 2 000 headers × 40 B) | ≈ 391 KiB | Per symbol |
| Pending snapshot (2 × 1 000 levels × 16 B) | ≈ 31 KiB | Per symbol |

- **Per symbol:** ≈ 0.85 MiB.
- **One symbol:** ≈ 16 + 20 + 0.3 + 0.85 ≈ 37 MiB.
- **256 symbols:** ≈ 64 + 20 + 0.3 + 216 ≈ 300 MiB.

The operating system commits pages only when they are touched, so a quiet symbol's resident
memory stays below its reservation. The `retained_bytes` gauge and the peak-RSS figure in the
final summary check these numbers in the live smoke test.

**Security:**

- Only public, unauthenticated endpoints are used, so no credentials are needed. There is no
  config file and no environment variable is read.
- TLS peer and hostname verification are always on (§6.2).
- No tracked file may contain the literal search string used by the reviewer's `git grep` check.
  The README quotes that command only inside a Markdown table, where the pipes are escaped.

## 10. Performance and allocation strategy

- **JSON:** simdjson On-Demand. Each payload is parsed exactly once, on the processor; envelope
  splitting is a bounded byte scan (§6.2). Levels are read with `get_raw_json_string()` (prices
  contain no escapes) straight into the decimal parser.
- **Zero heap allocations per record — scope of the claim:**
  - **Covered:** everything the processor does for a record, for every record kind and in every
    state, starting with the first record after startup:
    - payload parse;
    - sequence classification and window append;
    - synchronisation, resync and refresh (snapshot load, shadow build, swap);
    - book updates and trims;
    - depth5 checks;
    - row formatting and writes.

    This holds because every processor and per-symbol container is reserved to its cap at
    startup (§9), and the decode limits (§6.5) keep every event within those caps.
  - **Not covered:**
    - the network path: our buffers there (Beast `flat_buffer`, ring) are reused, but Asio,
      Beast and OpenSSL may allocate internally;
    - log lines: these are only formatted on the rate-limited warning/error path;
    - startup, archive moves, file creation and shutdown;
    - replay's CSV reader, whose row buffer grows to the longest row read.
  - **Proof:** a unit test replaces global `operator new`/`delete` with counting versions and
    arms the counter right after the processor is constructed. There is no warm-up: a warm-up
    would hide growth beyond what the warm-up traffic happened to reach. The logger suspends the
    counter while it formats. Records from the scenario fixtures and the exchange simulator
    (§13) exercise sync, gap, resync, coverage exhaustion, refresh and trimming, and the test
    asserts the count stays 0.
- **Benchmarks** (`bench/`, off by default, plain `std::chrono` harness with repetitions and
  percentiles):
  - decimal parsing;
  - payload parsing per stream kind;
  - book updates, flat vector vs `std::map`, with near-touch updates, deep updates, near-touch
    deletions and deep deletions;
  - ring push/pop;
  - end-to-end replay throughput (records/s) and per-record latency p50/p99/p99.9.

  Results, with the machine description, go into the README (X3).
- Release builds use `-O3` (CMake's Release default) on top of the documented
  `-Wall -Wextra -O2` flags; no `-march=native`, so binaries stay portable.

## 11. Source layout and build targets

```
src/
  main.cpp                 CLI entry: parse options, run capture or replay
  blc/
    core/                  types.hpp (enum class Venue, StreamKind; constexpr scales),
                           decimal.{hpp,cpp}, time.hpp, instrument_id.hpp, symbols.{hpp,cpp}
    csv/                   schema.hpp (exact headers, column counts), csv_writer.{hpp,cpp},
                           csv_reader.{hpp,cpp}
    parse/                 events.hpp, envelope.{hpp,cpp}, payload_parser.{hpp,cpp}
    book/                  book_side.hpp, order_book.{hpp,cpp} (incl. coverage),
                           sequence.{hpp,cpp}, diff_window.{hpp,cpp}, sync.{hpp,cpp},
                           depth5_check.{hpp,cpp}
    pipeline/              record.hpp, spsc_ring.hpp, processor.{hpp,cpp},
                           output_files.{hpp,cpp}, snapshot_requester.hpp, metrics.{hpp,cpp}
    replay/                replay_source.{hpp,cpp}
    net/                   endpoints.{hpp,cpp}, tls.{hpp,cpp}, ws_shard.{hpp,cpp},
                           rest_scheduler.{hpp,cpp}, capture_app.{hpp,cpp}
    cli/                   options.{hpp,cpp}
    util/                  logging.{hpp,cpp}
third_party/simdjson/      simdjson.h, simdjson.cpp, LICENSE, VERSION (pinned release)
tests/                     one test file per module; fixtures/ (real captured payloads);
                           scenarios/ (hand-built inputs with hand-derived expected outputs)
bench/                     blc_bench (BLC_BUILD_BENCH=OFF by default)
scripts/                   verify_outputs.sh, reviewer_checklist.sh, live_smoke.sh
samples/                   committed 1–2 minute capture + order book (S2)
docs/                      design.md, requirements.md
```

| Target | Contents | Depends on |
|---|---|---|
| `blc_core` (static) | `core`, `csv`, `parse`, `book`, `pipeline`, `replay`, `cli`, `util` | simdjson |
| `blc_net` (static) | `net` | `blc_core`, Boost (Asio, Beast), OpenSSL, Threads |
| `binance_capture` | `main.cpp` → `./build/binance_capture` | `blc_core`, `blc_net` |
| `unit_tests` | `tests/` → `./build/tests/unit_tests`; built by target `tests` | `blc_core`, `blc_net`, GoogleTest |
| `blc_bench` | `bench/` | `blc_core` |

Keeping Boost out of `blc_core` keeps the book, CSV and replay logic independent of the network
stack. The processor reaches the network only through the `SnapshotRequester` interface:
`blc_net` implements it with `asio::post`, and replay passes a no-op implementation.

## 12. Toolchain, dependencies and CI

- **Standard:** C++17 (`CMAKE_CXX_EXTENSIONS OFF`). Primary compiler GCC 12 (`g++-12`), also
  Clang 15. Target OS Ubuntu 22.04 (the documented environment), developed in WSL2 Ubuntu 22.04.
- **CMake ≥ 3.22**, Release by default. If no `c++` exists, the configure step falls back to
  `g++-12` so the bare `cmake -B build` works on a machine with only the README packages.
- **Dependencies:**

| Dependency | Version | How |
|---|---|---|
| Boost (Asio, Beast) | ≥ 1.74 (jammy) | `libboost-all-dev`; header-only use |
| OpenSSL | ≥ 1.1.1 (jammy: 3.0) | `libssl-dev` |
| simdjson | 4.6.11, recorded in `third_party/simdjson/VERSION` (latest of the mature 4.x line; 5.0 shipped 2026-09-28 and is still taking patch releases) | Vendored single-header amalgamation |
| GoogleTest | 1.17.0 | CMake `FetchContent`, URL + SHA-256 pinned, tests only |
| zlib | jammy | Listed in the brief's package list; not linked (Binance does not negotiate `permessage-deflate`) |

- **Network during the build:** the `binance_capture` target needs none. The default configure
  does download GoogleTest, because tests are enabled by default (the reviewer checklist builds
  them). For a fully offline build, configure with `-DBLC_BUILD_TESTS=OFF`, or with
  `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<local copy>`.
- **CI** (GitHub Actions, every job in an `ubuntu:22.04` container with the README package list).
  Each gate is added together with the module it checks:

| Job | Checks | Status |
|---|---|---|
| gcc-12 / clang-15 Release | `-Werror` build, unit tests via the README commands | Exists (M0) |
| Reviewer clean build | Bare `cmake -B build && cmake --build build` | Exists (M0) |
| clang-format | Formatting clean | Added in M1 |
| ASan + UBSan | Debug build: unit tests (+ replay of `samples/` from M8) | Added in M1 |
| TSan | Unit tests, including the ring stress test | Added with the ring (M4) |
| Output contract | Replay of scenario fixtures and `samples/` → `cmp` against expected files; exact headers; 26 columns; CSV validator; `strace` shows no sockets in replay; secrets grep empty | Scenario part with replay (M4); samples part in M8 |
| Valgrind | memcheck on replay, zero errors and leaks | Added with replay (M4) |
| clang-tidy | `.clang-tidy` clean | Added in M6 |

  Live capture is not run in CI: Binance blocks the US IPs GitHub runners use. Live checks run
  locally via `scripts/live_smoke.sh`.

## 13. Testing strategy

Two kinds of evidence are kept separate. **Consistency tests** (live = replay, golden files
produced by the program) catch regressions. **Correctness tests** compare against expected
results derived independently of the code under test.

| Suite | Covers |
|---|---|
| `decimal` | Table of valid/invalid inputs, exactness, 8-decimal boundary, overflow boundary, no floats |
| `csv_writer` / `csv_reader` | Quoting rules, `""` escaping, embedded comma/quote/newline, exact headers, round-trip, LF/CRLF input |
| `schema` | Header strings byte-exact, 26 / 9 / 8 column counts |
| `time`, `instrument_id` | ns split incl. boundaries; FNV values from §4.3 |
| `options` | Every flag, defaults, `=` form, all validation errors, symbol normalisation, exit code 2 |
| `endpoints` | URL building per venue, lowercase stream names, sharding, stream and URL-length caps |
| `envelope` | Fast path, reversed-field fallback, duplicate/missing keys, malformed frames, unknown streams |
| `payload_parser` | Real spot/usdm diff, depth5, trade and REST fixtures; field-order independence; missing/duplicate fields; trailing content; symbol mismatch; bad decimals |
| `book_side` | Insert/update/erase at top, middle and bottom; erase-absent no-op; ordering; trimming |
| `sequence` | Stale / gap / continues for both venues at every boundary, including spot overlap (`[100,105]` then `[103,108]`) |
| `diff_window` | Append, stale skip, reset on gap, oldest-first trimming by levels and by count, wrap-around |
| `sync` | Both venues: snapshot older/newer than the window, bridge conditions at exact boundaries, sync completed by a later diff (timestamp of that diff), too-old refetch, epoch reset, parse error as gap |
| `coverage` | Complete vs bounded sides, margin, exhaustion → Unsynced; trimming a bounded side inside and outside coverage; trimming a complete side revokes completeness (no zero padding afterwards) |
| `payload_limits` | Oversized diff (20 001 levels) → rejected as a gap, never truncated; oversized snapshot → re-requested; decode scratch never grows |
| `depth5_check` | All four resolution cases; full and partial match; mismatch → Unsynced; stale and unchecked counting; partial check excludes prices beyond coverage (the 10–6 / 9–5 example in §5.6 must not mismatch); pending check discarded when coverage is exhausted |
| `refresh` | Shadow sync from the window, snapshot newer than the book, too-old refetch, swap with equal and different top 5, exhaustion during refresh |
| `processor` | Row emission rules (§5.8), `type`/`side` mapping, `--no-trade-rows`, sidecar `apply_before_row`, per-symbol epoch isolation |
| `output_files` | Naming; archiving a symbol's existing files as one set into `replaced_<…>/`, including when only some exist; directory-name collisions; replay-output archiving |
| `spsc_ring` | Every wrap case, padding, full-ring drop, close protocol, randomised length fuzz, two-thread stress (run under TSan) |
| `shutdown` | Close with an empty ring, a full ring, a second signal, and a writer failure: the consumer always terminates and drains |
| `allocations` | Zero heap allocations on the covered path (§10) |
| **Exchange simulator** (property test) | A reference "exchange" holds a full-depth book and generates random activity: diffs, depth5, trades, REST snapshots truncated to 1 000 levels, reconnects. The output is fed through the processor. Every emitted row must equal the simulator's true top 5 at that point, and output may be suppressed only when the rules require it. 10⁵+ steps across seeds, both venues |
| **Scenario fixtures** | Small hand-written market-data + sidecar files with hand-derived expected order-book CSVs: initial sync with overlap, spot/usdm bridge boundaries, gap → resync, two symbols on one shard across a reconnect (each replayed alone), coverage exhaustion, depth5 mismatch → resync, depth5 extending beyond coverage (no false mismatch), oversized diff, sync completed by a later diff, reversed envelope, trade side mapping, and two successive same-day runs with the first run's archived set replayed from its `replaced_<…>/` directory |
| **Consistency** | Golden-file replay of captured fixtures; live → replay `cmp` on `samples/` |
| `live_smoke.sh` (local) | 60 s live capture per venue; `verify_outputs.sh`; replay → `cmp`; `depth5_mismatch=0`; peak RSS within budget |

TDD applies: each module's tests are written before its implementation.

## 14. Decisions and alternatives

| Decision | Chosen | Rejected because |
|---|---|---|
| Book synchronisation | REST snapshot + buffered diffs (official procedure, X1) | A depth5-seeded diff book shows holes when a top level is removed and level 6 was never seen |
| Finite snapshot depth | Coverage boundary; exhaustion → resync; background refresh (enhancement) renews it early | Ignoring it emits wrong top-5 rows after the price moves beyond the snapshot (≈ 0.15–0.24 % for BTCUSDT) |
| depth5 role | Exact cross-check only; mismatch → REST resync | Patching the book from depth5 leaves levels below its range untrusted; merging depth5 like a diff is wrong (no `qty 0` for levels leaving the top 5) |
| Trades | Annotation rows, book unchanged | Applying fills would double-count the diff stream |
| Epoch reset | Per symbol, at its first record with a newer epoch | A shard-wide reset makes a symbol's output depend on other symbols' timing and breaks per-file replay |
| Files | Per symbol, one set per run, named by the run's UTC start date | Daily rotation makes later files depend on earlier ones (no starting snapshot) and complicates replay |
| Existing output files | A symbol's whole file set is moved into `replaced_<timestamp>/`, keeping names, so it stays replayable | Truncating destroys earlier captures; refusing breaks the brief's README sequence, which reuses `./output`; renaming files one by one breaks the market-data ↔ sidecar pairing |
| REST usage | Weighted token bucket at 50 % of the IP budget, header reconciliation, global cooldown | Concurrency limits alone exceed the budget at scale; per-request `Retry-After` lets other requests continue during a cooldown |
| Threading | io thread + processor over an SPSC ring | One thread lets disk stalls block the socket; a writer thread adds a hop with no measured benefit |
| Queue | Custom byte ring, built first under heavy tests | `spsc_queue` + fixed-size pool also caps memory, but either wastes most of each slot or splits payloads, which then need copying before parsing (kept as fallback) |
| Ring full | Drop newest, counted, visible as `conn_seq` gap | Blocking stalls every connection and their pings, and loses more data through disconnects |
| Book structure | Sorted flat vector, best at back, galloping search | `std::map` allocates per insert and chases pointers; a price-indexed ladder needs tick sizes and wide ranges |
| JSON | simdjson On-Demand (vendored) | Boost.JSON requires Boost 1.75 (jammy has 1.74); RapidJSON/nlohmann are slower or allocate per value |
| WebSocket/TLS | Boost.Beast + OpenSSL | Already in the brief's package list; websocketpp is unmaintained; libwebsockets is C-callback based |
| Timestamps | Receive wall clock | Spot depth5 has no event time |
| Instrument id | Hash of venue:symbol | An index into `--symbols` changes with argument order and breaks live = replay |
| CSV line ending | LF | CRLF is RFC-literal but hostile to Unix tools; documented and the reader accepts both |
| Resync after reconnect | Always via REST | Continuing the sequence across connections saves one request but adds a rarely exercised path |

## 15. Verification status

**Resolved:**

1. **Streams per connection.** 1 024 on both venues, checked on 2026-10-11 against the
   [spot WebSocket streams page](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams),
   the [USD-M market streams page](https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams)
   and the [USD-M Connect page](https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams/Connect).
   (An earlier revision claimed a conflicting figure of 200; that came from a search summary and
   is not on these pages.) In practice the 8 192-byte URL limit binds first, at around 87
   symbols of 20 characters.
2. **REST weights and limits.** Measured on 2026-10-11 (§6.3).
3. **Snapshot coverage.** Measured on 2026-10-11 (§5.3).

**Checked during live testing:**

1. **depth5 consistency (M6):** `depth5_mismatch` must be 0 over a 10-minute capture per venue
   (spot full checks, usdm partial checks). Any mismatch is investigated before submission.
2. **Coverage (M6, then M7):** count `coverage_exhausted` over a 30-minute capture before the
   refresh enhancement exists, then `coverage_exhausted` and `refresh_ok` after it; record both
   in the README.
3. **US IP block (M5):** confirm GitHub-hosted runners receive HTTP 451 from Binance; if they do
   not, a live smoke job is added to CI.

## 16. Milestones

Milestone IDs used throughout this document. Each milestone ends with CI green and every
requirement row it delivers moved to **Done** in [requirements.md](requirements.md).

| ID | Scope | Depends on | Exit criteria |
|---|---|---|---|
| M0 | Scaffold, CI, requirements matrix, design | — | Done |
| M1 | Core primitives: `types`, `decimal`, `time`, `instrument_id`, `symbols`, CSV schema/writer/reader, CLI options, logging. CI: clang-format, ASan + UBSan | M0 | Unit tests green under ASan + UBSan |
| M2 | Parsing: vendored simdjson, envelope split (fast path + order-independent fallback), payload parsers with decoded-size limits, real-payload fixtures | M1 | Parser and limit tests green |
| M3 | Order book: `book_side`, coverage, sequence classifier, recent-diff window, sync state machine, depth5 check, exchange simulator | M2 | Simulator property test and sync, coverage and depth5 suites green |
| M4 | Pipeline + replay: SPSC ring first, alone (TSan job); then processor, output files with archive rule, sidecar, replay mode, scenario fixtures, allocation test. CI: output contract (scenario part), Valgrind | M3 | Every scenario fixture replays to its hand-derived expected output; zero allocations |
| M5 | Networking (needs WSL): endpoints, TLS, `WsShard`, REST scheduler, capture app, signals and `--duration`, shutdown protocol, metrics and stats | M4 | `live_smoke.sh` passes for both venues (60 s), frame-accounting invariant holds |
| M6 | Hardening: clang-tidy job, `--fault-reconnect-every`, shutdown tests, limit validation, 10-minute depth5 and 30-minute coverage runs | M5 | All CI jobs green; `depth5_mismatch=0` live |
| M7 | Enhancement and performance: background coverage refresh (§5.5), benchmarks, README performance numbers | M6 | Refresh suite green; live 30-minute run recorded; benchmark numbers in README |
| M8 | Submission: 1–2 minute sample captures, live = replay `cmp`, full README (including the brief's submission section), `reviewer_checklist.sh`, tag `v1.0.0`, repository made public with the user's confirmation | M7 | Every mandatory row in requirements.md is Done |
