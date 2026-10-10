# Design: binance-lob-capture

Approved 2026-10-11. This document is the specification the implementation follows; the README's
design notes are a summary of it. Requirement IDs (F*, S*, Q*, X*) refer to
[requirements.md](requirements.md).

## 1. Scope

`binance_capture` is a single C++17 executable with two modes:

- **Capture** — connects to Binance public combined WebSocket streams for one venue (`spot` or
  `usdm`) and a list of symbols, subscribes to `<symbol>@depth@100ms`, `<symbol>@depth5@100ms` and
  `<symbol>@trade`, and writes:
  - **Deliverable A** — a market-data audit CSV: one row per inbound data message, holding the
    inner Binance `data` payload.
  - **Deliverable B** — an order-book CSV: one top-5 snapshot row after each processed event.
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
| `--no-trade-rows` | both | off | Trades still go to the market-data CSV but produce no order-book rows (§5.6) |
| `--symbols-per-connection` | capture | `50` | 1 – venue cap (spot 341 = 1023 streams; usdm 66 = 198 streams, §15) |
| `--stats-interval` | both | `10` | Seconds between stats lines on stderr; `0` = final summary only |
| `--fault-reconnect-every` | capture | `0` = off | Testing aid: force-close every connection each N seconds to exercise reconnect + resync live |
| `--replay` | replay | — | One or more market-data CSVs, processed in the order given (§7) |

Both `--opt value` and `--opt=value` are accepted. Unknown options, missing values and invalid
values are usage errors.

Exit codes: `0` normal completion (including shutdown by signal or `--duration`), `1` runtime
failure (output I/O error, malformed replay input, unrecoverable configuration error), `2` usage
error.

The default capture `--output-dir` is `.` because the reviewer checklist runs a capture without
`--output-dir` and then checks `*_orderbook.csv` in the working directory.

## 3. Output files

### 3.1 Naming, rotation, overwrite

Capture writes three files per (venue, symbol, UTC date):

| File | Content |
|---|---|
| `market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv` | Deliverable A (§3.3) |
| `market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>_orderbook.csv` | Deliverable B (§3.4) |
| `depth_snapshots_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv` | REST snapshots used for sync (§3.5) |

- **One file per symbol** (the brief's per-symbol naming option). The `symbol` column is still
  written. Per-symbol files keep each book independently replayable and its `seqNo` contiguous.
- The sidecar deliberately does not match the `market_data_*.csv` or `*_orderbook.csv` globs.
- `<YYYY-MM-DD>` is the UTC date of the receive timestamp of the record that opened the files.
- **Rotation:** the three files of a symbol are opened together when its first record arrives.
  When a record's UTC date is later than the open files' date, all three are flushed, closed and
  reopened for the new date. Rotation never goes backwards if the wall clock steps back.
  `seqNo` continues across rotation.
- **Existing files** are truncated and rewritten; a warning naming each file is logged.
- **Replay** writes `<output-dir>/<input file stem>_orderbook.csv` for each input file.

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

- One row per WebSocket data frame with a valid `{"stream":…,"data":…}` envelope for a subscribed
  stream, in processing order: the processor writes the row immediately before applying the event
  to the book.
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
| `tsec`, `tnsec` | int64, int32 | Receive time of the record that produced the row (§4.1); `tsec * 1e9 + tnsec` = ns since the Unix epoch |
| `seqNo` | uint64 | 0-based, +1 per row of this (venue, symbol) for the process lifetime (§4.4) |
| `id` | int32 | Instrument id (§4.3) |
| `type` | char | Class of the event that produced the row (table below) |
| `side` | char | `B`, `S` or `N` (table below) |
| `bid0`…`bid4` | int64 | Best five bid prices, best (highest) first, scaled (§4.2) |
| `bid_size0`…`bid_size4` | int64 | Quantities at those bids, scaled |
| `ask0`…`ask4` | int64 | Best five ask prices, best (lowest) first, scaled |
| `ask_size0`…`ask_size4` | int64 | Quantities at those asks, scaled |

A side with fewer than five levels fills the missing levels with price `0` and size `0`.

| `type` | Event | `side` |
|---|---|---|
| `R` | REST snapshot applied; the book became synchronised (§5.3) | `N` |
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
recv_tsec,recv_tnsec,venue,symbol,apply_before_row,payload_json
```

- One row per REST depth snapshot received for the symbol, whether or not it was used.
- `apply_before_row` is the number of data rows already written to the paired market-data file
  when the processor consumed the snapshot, i.e. the 0-based index of the market-data row it
  precedes. Replay injects the snapshot at exactly that position. A value equal to the file's
  row count means after the last row. Rows are written in processing order, so the column is
  non-decreasing; replay rejects a sidecar where it is not.
- `payload_json` is the minified REST response body (`{"lastUpdateId":…,"bids":[…],"asks":[…]}`,
  plus `E`/`T` on usdm).

## 4. Policies

### 4.1 Timestamps

- **Receive time** = `std::chrono::system_clock::now()` taken in the WebSocket read-completion
  handler, after the whole message has been read and before any parsing. Nanoseconds since the
  Unix epoch, UTC (CLOCK_REALTIME on Linux).
- `recv_tsec = recv_ns / 1 000 000 000`, `recv_tnsec = recv_ns % 1 000 000 000`.
- Order-book rows use the receive time of the record that produced them; an `R` row uses the
  receive time of the REST response.
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
  overflow — are parse errors: counted, and handled per §5.5. Values are never rounded.

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
| `conn_seq` | per (shard, epoch) | 0 | Each WebSocket data frame received. A frame rejected as malformed consumes its number, so drops are visible as gaps |
| `seqNo` | per (venue, symbol) order-book stream | 0 | Each order-book row, continuing across rotated files |

## 5. Order book

### 5.1 Data structure

Each side is a `std::vector<Level{int64 price; int64 qty;}>` kept sorted so that the **best price
is at the back**: bids ascending, asks descending.

- `set(price, qty)`: binary search (O(log n)); insert, overwrite or erase. Only the elements
  between the position and the back (the levels *better* than the price) move. Updates cluster at
  the touch, so the typical cost is a handful of element moves.
- Top-5 read: O(1) per level, from the back.
- Capacity is reserved at startup. A side is capped at 20 000 levels; beyond that the worst levels
  are trimmed in one batch down to 15 000. Trimmed levels are far from the touch and never
  visible in the top 5; a later diff for such a price re-inserts it with its absolute quantity.

| | Flat vector, best at back (chosen) | `std::map<int64,int64>` |
|---|---|---|
| Update near touch | O(log n) search + few contiguous moves; no allocation | O(log n) pointer chasing + node allocation on insert |
| Deep update | O(n) contiguous move (cheap: 16-byte elements, n ≈ 1000) | O(log n) |
| Top-5 read | 5 contiguous loads | 5 node hops |
| Allocation per message | none after warm-up | one per inserted level |

The trade-off is measured by the book benchmark (§10) and reported in the README.

### 5.2 Event semantics

- **Diff** (`depthUpdate` on `@depth@100ms`): each `[price, qty]` is the **absolute** quantity at
  that price; `qty == 0` erases the level. Erasing a level that is not in the book is a no-op
  (Binance documents this as normal). Levels are applied in payload order.
- **REST snapshot**: replaces the whole book.
- **depth5** (`@depth5@100ms`): the exact top 5 levels per side at update id *p* (spot:
  `lastUpdateId`; usdm: `u`). It has **replace** semantics: a level that left the top 5 is simply
  absent — it is *not* sent with `qty 0` — so depth5 must never be merged level-by-level like a
  diff. Range replace: for bids with 5 levels, every book level priced ≥ `bid[4]` is replaced by
  the depth5 bids; with fewer than 5 levels the whole side is replaced. Asks are symmetric
  (≤ `ask[4]`). §5.4 defines when range replace is used. A depth5 payload with more than 5
  levels on a side is a parse error.
- **Trade**: never changes the book. Fills already reach the book through the diff stream;
  applying trades as well would double-count. Trades produce annotation rows only (§5.6).
- usdm `depth5` payloads also carry `"e":"depthUpdate"`; the event class is therefore taken from
  the **stream name**, never from the `e` field.

### 5.3 Synchronisation state machine

Each (venue, symbol) book is either **Unsynced** (initial state; no order-book rows) or
**Synced**. Synchronisation follows Binance's documented procedure for each venue, using a REST
snapshot with `limit=1000`: spot `GET https://api.binance.com/api/v3/depth?symbol=S&limit=1000`,
usdm `GET https://fapi.binance.com/fapi/v1/depth?symbol=S&limit=1000`.

Let *L* be the snapshot's `lastUpdateId` and *id* the book's last applied update id.

**Unsynced, both venues**

1. A diff is appended to the sync buffer. If the buffer is not empty and the diff does not
   continue it — spot: `U == last.u + 1`; usdm: `pu == last.u` — the buffer is cleared first
   (counted as `buffer_resets`).
2. A snapshot becomes the *pending* snapshot (replacing an older one).
3. After either event, **try_sync** runs if there is a pending snapshot and a non-empty buffer:
   1. Drop buffered diffs that the snapshot already contains — spot: `u ≤ L`; usdm: `u < L`.
   2. Buffer now empty: wait for more diffs.
   3. The first remaining diff must bridge the snapshot — spot: `U ≤ L + 1 ≤ u`; usdm:
      `U ≤ L ≤ u`. If its `U` is too large, the snapshot is too old: discard it and request a new
      one.
   4. Otherwise load the snapshot, apply every buffered diff in order, set *id* to the last
      applied `u`, switch to Synced and emit an `R` row.
4. The sync buffer holds at most 10 000 diffs. On overflow it is cleared and the newest diff is
   kept (counted).
5. The processor requests a snapshot when it enters Unsynced and when it discards a snapshot as
   too old, with at most one request outstanding per symbol. Request bookkeeping never influences
   output, which keeps replay exact (§7).

The spot bridge condition is written `U ≤ L + 1 ≤ u` to agree with Binance's update rule (an
event is valid while `U ≤ id + 1`); it accepts an event that starts exactly at `L + 1`.

**Synced, spot** — for each diff:

- `u ≤ id`: stale, counted; the book is unchanged.
- `U > id + 1`: gap → §5.5.
- Otherwise apply it and set `id = u`.

**Synced, usdm** — for each diff:

- `u ≤ id`: stale, counted.
- `pu ≠ id`: gap → §5.5.
- Otherwise apply it and set `id = u`.

### 5.4 depth5 cross-check and repair

The REST + diff book is authoritative. depth5 is used as an **independent exact cross-check**,
and its replace semantics are applied only to repair a disagreement. While Unsynced, depth5 is
ignored. While Synced, with *p* the depth5 id:

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

- **Full check**: on each side the book's top levels equal the depth5 levels (same count, prices
  and quantities).
- **Partial check**: within depth5's price range on each side, every price not present in *d*
  has the same presence and quantity in the book and in depth5. Prices not touched by *d* did not
  change between *id* and *u*, so their state at *p* is known exactly.
- **Match**: counted as `depth5_verified` or `depth5_partially_verified`.
- **Mismatch**: counted as `depth5_mismatch`; the first 10 are logged with both level sets. The
  book is repaired by range replace with the depth5 levels.

Why range replace at *p ≥ id* followed by the remaining diffs yields the true book: let *T* be the
range covered by depth5 at *p*. After the replace, every level in *T* holds its true value at *p*,
and levels outside *T* hold their value at *id*. The diffs that follow carry the absolute
quantity of every level that changed after *id*. A level that changed is overwritten with its
true newer value. A level that did not change keeps a value that was already correct: inside *T*
its value at *p* equals its current value, and outside *T* its value at *id* does. A level the
replace removed but which exists later must have changed after *p*, so a following diff restores
it.

Live probe (20 s, BTCUSDT, 2026-10-11):

- **spot:** every depth5 id equalled some diff's `u` (188/188), and depth5 usually arrived one
  message before that diff, so the *full check after applying d* path applies.
- **usdm:** depth5 ids never equalled a diff's `u` (0/130), so the *partial check* path applies.

### 5.5 Gaps, reconnects and errors

| Situation | Effect |
|---|---|
| New `conn_epoch` on a shard (the processor sees an epoch greater than the last one seen for that shard) | Every symbol on the shard becomes Unsynced: book cleared; sync buffer, pending snapshot and pending depth5 check discarded; new snapshot requested. `conn_seq` restarts at 0 |
| Diff sequence gap while Synced | That symbol becomes Unsynced (book cleared, the gap diff seeds the new buffer, snapshot requested); counted as `gaps` |
| Diff payload parse error | Treated as a gap: a missing diff can't be skipped safely |
| depth5 or trade payload parse error | Counted; no row; book unchanged |
| Payload `s` ≠ stream symbol | Parse error (defensive) |
| Snapshot too old | Discarded and a new one requested (§5.3) |
| REST request failure | Retried by the network layer with backoff; `418`/`429` honour `Retry-After` |
| Envelope invalid or stream not subscribed | Counted; no market-data row; its `conn_seq` number stays unused |

The brief's options were "reset, resync, or drop until snapshot". This design resets the book and
drops order-book output until a REST snapshot resynchronises it, on every new epoch and every gap.

### 5.6 Row emission rules

- Unsynced: no order-book rows.
- Becoming Synced: exactly one `R` row (the book after the snapshot and the buffered diffs).
- Synced: exactly one row per successfully parsed market-data record of that symbol — `D`, `P`
  and `T` (`T` omitted with `--no-trade-rows`) — showing the book after processing the record,
  including stale diffs and depth5 records that leave the book unchanged.
- Consequently every order-book file starts with an `R` row, and every row shows a book that
  passed the sequence checks.

## 6. Architecture

### 6.1 Threads and data flow

```
 io thread (main thread, Boost.Asio io_context)        processing thread (single consumer)
 ┌────────────────────────────────────────────┐        ┌──────────────────────────────────────────┐
 │ WsShard ×N: resolve→TCP→TLS→WS→read loop    │        │ for each record, in order:               │
 │   recv_ns, conn_seq, envelope split ───────┼─┐      │  1. rotate files if the UTC date changed │
 │ RestClient: HTTPS depth snapshots ─────────┼─┤ SPSC │  2. write market-data / sidecar row      │
 │ signal_set, --duration, reconnect timers   │ ├─ring─►  3. parse payload once (simdjson)        │
 │                                            │ │      │  4. book: sync state machine (§5)        │
 │ asio::post ◄── snapshot requests, fatal ───┼─┼──────┤  5. write order-book row (§5.6)          │
 └────────────────────────────────────────────┘ │      │  periodic: flush (1 s), stats line       │
                                                │      └──────────────────────────────────────────┘
 replay: CSV reader + sidecar injection ────────┘  (same processor; no io thread, no sockets)
```

| Thread | Owns | Never does |
|---|---|---|
| io | `io_context`, shards, REST client, signal set, timers; envelope split; ring producer | Payload parsing, book updates, file I/O |
| processing | Ring consumer, simdjson parser, books, writers, metrics output | Socket I/O |

The threads share exactly three things:

1. The SPSC ring (acquire/release atomics).
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
- **Envelope split:** a validated byte scan, not a JSON parse. The frame must start with
  `{"stream":"`, the name ends at the next `"`, which must be followed by `,"data":`; the payload
  runs from there to the final `}` and must itself start with `{` and end with `}`. A frame that
  fails the fast path is retried with simdjson On-Demand (`find_field("stream")`,
  `find_field("data").raw_json()`); if that also fails, the frame is counted as malformed.
- **Reconnect:** any read error, close or timeout schedules a reconnect with backoff (500 ms,
  doubling, capped at 30 s, ±20 % jitter, reset after 60 s connected). This also covers
  Binance's 24-hour disconnect. A successful handshake increments `conn_epoch`. Attempt rates
  stay far below spot's 300 connections per 5 minutes per IP.
- **REST client:** one session object per request (Beast HTTP over TLS, 10 s timeout, body
  ≤ 16 MiB, at most 4 concurrent requests). A `200` response is pushed to the ring as a snapshot
  record (retried on a 10 ms timer if the ring is full). `418`/`429` wait for `Retry-After`
  (default 60 s); other failures retry with the same backoff as the WebSocket.
- **`--fault-reconnect-every N`:** a timer force-closes each connection every N seconds.

### 6.3 Record queue (SPSC ring)

- One pre-allocated byte ring (64 MiB) between the io thread and the processor.
- Each record is contiguous and 64-byte aligned: a fixed header, the payload, then
  `SIMDJSON_PADDING` bytes so simdjson parses the payload in place. A record that does not fit
  before the end of the buffer is preceded by a wrap marker.
- Header: record kind (`market`, `snapshot`, `end`), `shard_id`, `conn_epoch`, `conn_seq`,
  `recv_ns`, `steady_ns` (for the latency metric), symbol index, stream kind, payload length.
- Head and tail are atomics on separate cache lines, published with release and read with
  acquire; each side caches the other side's index to avoid cache-line ping-pong.
- **Consumer wait:** spin briefly, then park on a condition variable with a 50 ms timeout (so
  periodic flushes and stats still run). The producer notifies only when the consumer is parked,
  so the busy path never takes a lock.
- **Full ring:** the producer drops the newest frame and counts `ring_drops`; it never blocks the
  io thread. The drop is visible as a `conn_seq` gap in the market-data CSV. A dropped diff
  produces a sequence gap, which the book handles by resyncing (§5.5). At the measured BTCUSDT
  rate (spot ≈ 7 KiB/s, usdm ≈ 15 KiB/s) 64 MiB covers over an hour of processor stall per
  symbol.

### 6.4 Processor

For each record, in ring order:

1. On an `end` record: flush, close, print the final summary and exit the thread.
2. Detect a new `conn_epoch` for the record's shard (§5.5).
3. Rotate the symbol's files if the record's UTC date is later than the open files' date (§3.1).
4. Market record: write the market-data row (minify if needed, escape directly into the writer
   buffer). Snapshot record: write the sidecar row.
5. Parse the payload once with the reused simdjson On-Demand parser into a reused typed event:
   `DepthDiff`, `Depth5` (fixed arrays of 5 levels), `Trade` or `DepthSnapshot`. Fields are read
   by key, so field order does not matter; unknown fields (`ps`, `st`, `X`, `M`, …) are ignored.
6. Run the book logic (§5), then emit an order-book row when §5.6 says so.
7. Release the record.

The processor owns no timers. Periodic work (flush every 1 s, stats every `--stats-interval`) is
checked against a steady clock between records and on park timeouts; none of it affects output.

### 6.5 Writers and back-pressure

- Each output file has a 1 MiB user-space buffer over an unbuffered `std::FILE` (RAII handle).
  Rows are formatted with `std::to_chars` straight into the buffer: no temporary strings.
- The buffer is flushed when full, every 1 s, on rotation and on shutdown. `fsync` is not used,
  so a crash loses at most about the last second.
- A short or failed write is fatal: logged, counted, and the processor posts a shutdown with
  exit code 1.
- **Back-pressure chain:** disk stall → processor stalls → the ring absorbs it → if the ring
  fills, frames are dropped and counted (§6.3). The socket thread never waits on disk.
- A dedicated writer thread was rejected: formatting is the dominant cost and stays on the
  processor, and 1 MiB writes already amortise system calls. The ring already decouples the
  socket from disk. The benchmark (§10) records the processor's per-record cost to back this up.

### 6.6 Shutdown

1. **Trigger:** SIGINT/SIGTERM (`asio::signal_set`), the `--duration` timer, or a fatal error
   posted by the processor.
2. **io thread:** mark stopping; cancel timers and REST sessions; `async_close` each WebSocket
   (2 s timeout); push an `end` record (retrying until there is space); return from
   `io_context::run()`.
3. **Processor:** drain the ring up to `end`, flush and close every file, print the final
   summary.
4. **main:** join the processor and return the exit code.

A second SIGINT/SIGTERM stops the `io_context` immediately; the processor still drains and
flushes. All resources are RAII-owned, so a normal exit is clean under ASan, LSan and Valgrind.

## 7. Replay mode and determinism

```
binance_capture --replay output/market_data_spot_BTCUSDT_2026-10-11.csv --output-dir replay_output
```

- Input files are processed in the order given. Book state, `seqNo` and per-shard epoch tracking
  persist across files, so passing a symbol's daily files in date order reproduces a multi-day
  capture.
- Each file's header must match §3.3 exactly. Rows are parsed by an RFC 4180 reader (quoted
  fields, `""`, embedded separators/newlines, LF or CRLF) into the same record structure the ring
  carries.
- The sidecar `depth_snapshots_<venue>_<SYMBOL>_<date>.csv` must be in the same directory as each
  market-data file. Each snapshot row is injected immediately before market-data row
  `apply_before_row`. A missing sidecar is an error (the book could never synchronise).
- Records feed the **same processor**, with the market-data and sidecar writers disabled and a
  no-op snapshot-request sink.
- Replay never constructs an `io_context`, socket or TLS context. CI verifies this with
  `strace -f -e trace=socket` (no `socket()` calls).

**Determinism argument.** Order-book output is a pure function of the record sequence: the book
logic has no timers, randomness or I/O-dependent branches, and the snapshot-request bookkeeping
cannot change output. The market-data CSV stores records in processing order, and the sidecar
stores each snapshot's position, so replay feeds the identical sequence and produces
byte-identical order-book files. CI enforces this on the committed sample run with `cmp` (live
order book vs replay order book). Both runs must use the same `--no-trade-rows` setting.

## 8. Observability

- **io-thread counters** (relaxed atomics): frames and bytes received per stream kind,
  `malformed_frames`, `ring_drops`, `connects`, `reconnects`, `connect_failures`,
  `rest_requests`, `rest_failures`, `rest_throttled`.
- **Processor counters:** `md_rows`, `ob_rows`, `snapshot_rows`, `parse_errors`, `stale_diffs`,
  `gaps`, `resyncs`, `syncs`, `buffer_resets`, `buffer_overflows`, `depth5_verified`,
  `depth5_partially_verified`, `depth5_mismatch`, `depth5_stale`, `depth5_unchecked`,
  `payloads_minified`, `ring_high_water`, and a fixed-bucket log2 histogram of receive →
  row-written latency (steady clock).
- **Stats line** on stderr every `--stats-interval` seconds, `key=value` for `grep`/`awk`:
  `stats uptime_s=10 frames=394 md_rows=394 ob_rows=377 parse_errors=0 gaps=0 resyncs=0
  reconnects=0 depth5_verified=187 depth5_mismatch=0 ring_drops=0 ring_hw_kib=12 lat_p50_us=4
  lat_p99_us=21`
- **Final summary** at exit: the same counters, totals and per symbol.
- **Logging:** stderr, levels `info`/`warn`/`error`, ISO-8601 UTC timestamps. Repeated errors
  are rate-limited (first 10, then every 1000th). Nothing is logged per message on the normal
  path.

## 9. Limits and security

| Limit | Value | On violation |
|---|---|---|
| Symbols per process | 256 | Usage error |
| Symbols per connection | spot 341, usdm 66 | Usage error |
| Stream URL length | 8 192 bytes | Usage error |
| WebSocket message | 4 MiB | Frame rejected (Beast error) → reconnect, counted |
| REST body | 16 MiB | Request failed → retry, counted |
| Ring | 64 MiB | Newest frame dropped, counted |
| Sync buffer | 10 000 diffs per symbol | Buffer reset, counted |
| Book side | 20 000 levels | Trimmed to 15 000 worst-first |
| Concurrent REST requests | 4 | Queued |

- Only public, unauthenticated endpoints are used, so no credentials are needed. There is no
  config file and no environment variable is read.
- TLS peer and hostname verification are always on (§6.2).
- No tracked file may contain the literal search string used by the reviewer's `git grep` check.
  The README quotes that command only inside a Markdown table, where the pipes are escaped.

## 10. Performance and allocation strategy

- **JSON:** simdjson On-Demand. Each payload is parsed exactly once, on the processor; envelope
  splitting is a bounded byte scan. Levels are read with `get_raw_json_string()` (prices contain
  no escapes) straight into the decimal parser.
- **Steady-state allocations: zero per message.** Everything is allocated once and reused:
  - the ring (once at startup);
  - the simdjson parser (allocated for the maximum document size);
  - typed events (`std::vector` levels keep their capacity; depth5 uses fixed arrays);
  - book sides (reserved);
  - the sync buffer (reused `DepthDiff` slots);
  - writer buffers;
  - the Beast `flat_buffer`.

  Formatting uses `std::to_chars`. A unit test replaces global `operator new`/`delete` with
  counting versions, replays recorded traffic after a warm-up, and asserts the count is 0.
- **Benchmarks** (`bench/`, off by default, plain `std::chrono` harness with repetitions and
  percentiles):
  - decimal parsing;
  - payload parsing per stream kind;
  - book updates, flat vector vs `std::map`, with near-touch and deep update mixes;
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
    book/                  book_side.hpp, order_book.{hpp,cpp}, sync.{hpp,cpp},
                           depth5_check.{hpp,cpp}
    pipeline/              record.hpp, spsc_ring.hpp, processor.{hpp,cpp},
                           output_files.{hpp,cpp}, metrics.{hpp,cpp}
    replay/                replay_source.{hpp,cpp}
    net/                   endpoints.{hpp,cpp}, tls.{hpp,cpp}, ws_shard.{hpp,cpp},
                           rest_client.{hpp,cpp}, capture_app.{hpp,cpp}
    cli/                   options.{hpp,cpp}
    util/                  logging.{hpp,cpp}
third_party/simdjson/      simdjson.h, simdjson.cpp, LICENSE, VERSION (pinned release)
tests/                     one test file per module, fixtures/ (real captured payloads)
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
stack. The processor reaches the network only through a small `SnapshotRequester` interface:
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
| simdjson | 4.6.11, recorded in `third_party/simdjson/VERSION` (latest of the mature 4.x line; 5.0 shipped 2026-09-28 and is still taking patch releases) | Vendored single-header amalgamation, so the main build needs no network |
| GoogleTest | 1.17.0 | CMake `FetchContent`, URL + SHA-256 pinned, tests only |
| zlib | jammy | Listed in the brief's package list; not linked (Binance does not negotiate `permessage-deflate`) |

- **CI** (GitHub Actions, every job in an `ubuntu:22.04` container with the README package list):

| Job | Checks |
|---|---|
| gcc-12 / clang-15 Release | `-Werror` build, unit tests via the README commands |
| Reviewer clean build | Bare `cmake -B build && cmake --build build` |
| ASan + UBSan | Debug build: unit tests + replay of `samples/` |
| TSan | Unit tests (including a ring stress test) + replay |
| Valgrind | memcheck on replay of `samples/`, zero errors and leaks |
| clang-tidy, clang-format | `.clang-tidy`, `.clang-format` clean |
| Output contract | Replay `samples/` → `cmp` against the committed order book; exact headers; 26 columns; CSV validator; `strace` shows no sockets in replay; secrets grep empty |

  Live capture is not run in CI: Binance blocks the US IPs GitHub runners use. Live checks run
  locally via `scripts/live_smoke.sh`.

## 13. Testing strategy

| Suite | Covers |
|---|---|
| `decimal` | Table of valid/invalid inputs, exactness, 8-decimal boundary, overflow boundary, no floats |
| `csv_writer` / `csv_reader` | Quoting rules, `""` escaping, embedded comma/quote/newline, exact headers, round-trip, LF/CRLF input |
| `schema` | Header strings byte-exact, 26 / 9 / 6 column counts |
| `time`, `instrument_id` | ns split incl. boundaries; FNV values from §4.3 |
| `options` | Every flag, defaults, `=` form, all validation errors, symbol normalisation, exit code 2 |
| `endpoints` | URL building per venue, lowercase stream names, sharding, URL-length limit |
| `envelope` | Fast path, fallback path, malformed frames, unknown streams |
| `payload_parser` | Real spot/usdm diff, depth5, trade and REST fixtures; field order independence; symbol mismatch; bad decimals |
| `book_side` | Insert/update/erase at top, middle and bottom; erase-absent no-op; ordering; top-5 with < 5 levels; trimming |
| `order_book` property test | 10⁶ random operations vs a `std::map` reference model; top-5 equal after every step |
| `sync` | Spot and usdm: happy path, stale buffered diffs, snapshot too old → refetch, bridge conditions at exact boundaries, gap while Synced, buffer reset on discontinuity, buffer overflow, epoch change, parse-error-as-gap |
| `depth5_check` | All four resolution cases, full/partial match and mismatch, repair correctness (replace then diffs = truth), stale and unchecked counting |
| `processor` | Row emission rules (§5.6), `type`/`side` mapping, `--no-trade-rows`, rotation at UTC midnight, `seqNo` continuity, sidecar `apply_before_row` |
| `replay` | Golden files: fixture capture → replay → byte-equal expected order book; sidecar injection; header validation errors |
| `spsc_ring` | Wrap-around, padding, full-ring drop, multi-threaded stress (run under TSan) |
| `allocations` | Zero heap allocations per record in steady state |
| `live_smoke.sh` (local) | 60 s live capture per venue; `verify_outputs.sh`; replay → `cmp` |

TDD applies: each module's tests are written before its implementation.

## 14. Decisions and alternatives

| Decision | Chosen | Rejected because |
|---|---|---|
| Book synchronisation | REST snapshot + buffered diffs (official procedure, X1) | A depth5-seeded diff book shows holes when a top level is removed and level 6 was never seen |
| depth5 role | Exact cross-check; range replace only to repair | Replacing on every arrival would make the check circular; merging depth5 like a diff is wrong (no `qty 0` for levels leaving the top 5) |
| Trades | Annotation rows, book unchanged | Applying fills would double-count the diff stream |
| Threading | io thread + processor over an SPSC ring | One thread lets disk stalls block the socket; a writer thread adds a hop with no measured benefit |
| Ring full | Drop newest, counted, visible as `conn_seq` gap | Blocking stalls every connection and their pings, and loses more data through disconnects |
| Book structure | Sorted flat vector, best at back | `std::map` allocates per insert and chases pointers; a price-indexed ladder needs tick sizes and wide ranges |
| JSON | simdjson On-Demand (vendored) | Boost.JSON requires Boost 1.75 (jammy has 1.74); RapidJSON/nlohmann are slower or allocate per value |
| WebSocket/TLS | Boost.Beast + OpenSSL | Already in the brief's package list; websocketpp is unmaintained; libwebsockets is C-callback based |
| Files | Per symbol per UTC date, rotation at midnight | One multi-symbol file per run mixes independent books and breaks per-file replay |
| Timestamps | Receive wall clock | Spot depth5 has no event time |
| Instrument id | Hash of venue:symbol | An index into `--symbols` changes with argument order and breaks live = replay |
| CSV line ending | LF | CRLF is RFC-literal but hostile to Unix tools; documented and the reader accepts both |
| Exchange-side resync after reconnect | Always resync via REST | Attempting to continue the sequence across connections saves one request but adds a rarely exercised path |

## 15. Verify during implementation

These defaults are set; each must be confirmed and the constant or this document updated.

1. **usdm streams per connection:** the cap is assumed to be 200 (→ 66 symbols). Confirm in the
   USD-M docs; adjust `kMaxStreamsPerConnection` per venue.
2. **REST weights:** spot `limit=1000` is weight 50, usdm `limit=1000` is weight 20 — confirm,
   and keep sync bursts after a reconnect well under the per-minute IP limit.
3. **depth5 consistency live:** `depth5_mismatch` must be 0 over a 10-minute capture per venue
   (spot full checks, usdm partial checks). Any mismatch is investigated before the repair policy
   is kept.
4. **US IP block:** confirm GitHub-hosted runners receive HTTP 451 from Binance; if they do not, a
   live smoke job is added to CI.
