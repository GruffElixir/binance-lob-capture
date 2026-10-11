#!/usr/bin/env python3
"""Writes the hand-derived replay scenarios under tests/scenarios/.

Inputs (market-data CSV + depth-snapshot sidecar) are built from the event lists below. Every
expected order-book row is written out literally here, from the book state worked out by hand in
each scenario's README.md -- nothing in this script computes a book. Run it from anywhere:

    python3 tests/scenarios/generate.py
"""

import os
import shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
T0 = 1791669450 * 1_000_000_000  # receive time of a scenario's first record
STEP = 1_000_000  # 1 ms between records
DATE = "2026-10-11"
SCALE = 100_000_000

MD_HEADER = "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json"
OB_HEADER = (
    "tsec,tnsec,seqNo,id,type,side,bid0,bid1,bid2,bid3,bid4,"
    "bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,ask0,ask1,ask2,ask3,ask4,"
    "ask_size0,ask_size1,ask_size2,ask_size3,ask_size4"
)
SC_HEADER = "recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_json"


def fnv_id(venue, symbol):
    h = 2166136261
    for c in (venue + ":" + symbol).encode():
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h & 0x7FFFFFFF


def dec(x):
    """Decimal text for a price/qty given in whole units or as a string."""
    if isinstance(x, str):
        return x
    return "%d.00000000" % x


def scaled(x):
    if isinstance(x, str):
        whole, _, frac = x.partition(".")
        return int(whole) * SCALE + int((frac + "00000000")[:8])
    return x * SCALE


def levels(ls):
    return "[" + ",".join('["%s","%s"]' % (dec(p), dec(q)) for p, q in ls) + "]"


def csv_text(s):
    return '"' + s.replace('"', '""') + '"'


def split(ns):
    return ns // 1_000_000_000, ns % 1_000_000_000


class Symbol:
    """One symbol's input files, written in processing order."""

    def __init__(self, venue, symbol):
        self.venue, self.symbol = venue, symbol
        self.md, self.sidecar, self.expected = [], [], []

    # --- payloads -----------------------------------------------------------------------------
    def diff_payload(self, U, u, pu, bids, asks):
        s = '{"e":"depthUpdate","E":1791669450000,'
        if self.venue == "usdm":
            s += '"T":1791669450000,'
        s += '"s":"%s","U":%d,"u":%d,' % (self.symbol, U, u)
        if self.venue == "usdm":
            s += '"pu":%d,' % pu
        return s + '"b":%s,"a":%s}' % (levels(bids), levels(asks))

    def depth5_payload(self, p, bids, asks):
        if self.venue == "spot":
            return '{"lastUpdateId":%d,"bids":%s,"asks":%s}' % (p, levels(bids), levels(asks))
        return ('{"e":"depthUpdate","E":1791669450000,"T":1791669450000,"s":"%s","U":%d,'
                '"u":%d,"pu":%d,"b":%s,"a":%s}' % (self.symbol, p, p, p - 1, levels(bids),
                                                   levels(asks)))

    def trade_payload(self, trade_id, m):
        mm = "true" if m else "false"
        if self.venue == "spot":
            return ('{"e":"trade","E":1791669450000,"s":"%s","t":%d,"p":"10.00000000",'
                    '"q":"1.00000000","T":1791669450000,"m":%s,"M":true}' % (self.symbol, trade_id, mm))
        return ('{"e":"trade","E":1791669450000,"T":1791669450000,"s":"%s","t":%d,'
                '"p":"10.00000000","q":"1.00000000","X":"MARKET","m":%s}' % (self.symbol, trade_id, mm))

    def snapshot_payload(self, L, bids, asks):
        extra = ',"E":1791669450000,"T":1791669450000' if self.venue == "usdm" else ""
        return '{"lastUpdateId":%d%s,"bids":%s,"asks":%s}' % (L, extra, levels(bids), levels(asks))

    # --- records (t is the record's position on the scenario clock) ----------------------------
    def market(self, t, kind, shard, epoch, seq, payload):
        sec, nsec = split(T0 + t * STEP)
        self.md.append("%d,%d,%s,%s,%d,%d,%d,%s,%s" % (sec, nsec, self.venue, kind, shard, epoch,
                                                       seq, self.symbol, csv_text(payload)))

    def diff(self, t, shard, epoch, seq, U, u, bids=(), asks=(), pu=None):
        self.market(t, "depth_diff", shard, epoch, seq,
                    self.diff_payload(U, u, U - 1 if pu is None else pu, bids, asks))

    def depth5(self, t, shard, epoch, seq, p, bids, asks):
        self.market(t, "depth5", shard, epoch, seq, self.depth5_payload(p, bids, asks))

    def trade(self, t, shard, epoch, seq, trade_id, m):
        self.market(t, "trade", shard, epoch, seq, self.trade_payload(trade_id, m))

    def snapshot(self, t, shard, epoch, L, bids, asks):
        sec, nsec = split(T0 + t * STEP)
        self.sidecar.append("%d,%d,%s,%d,%d,%s,%d,%s" % (
            sec, nsec, self.venue, shard, epoch, self.symbol, len(self.md),
            csv_text(self.snapshot_payload(L, bids, asks))))

    # --- expected rows, written by hand ---------------------------------------------------------
    def row(self, t, typ, side, bids, asks):
        sec, nsec = split(T0 + t * STEP)

        def pad(ls):
            ls = list(ls) + [(0, 0)] * (5 - len(ls))
            return [scaled(p) for p, _ in ls], [scaled(q) for _, q in ls]

        bp, bq = pad(bids)
        ap, aq = pad(asks)
        fields = [sec, nsec, len(self.expected), fnv_id(self.venue, self.symbol), typ, side]
        fields += bp + bq + ap + aq
        self.expected.append(",".join(str(f) for f in fields))

    def stem(self):
        return "market_data_%s_%s_%s" % (self.venue, self.symbol, DATE)


def write_scenario(name, symbols):
    base = os.path.join(ROOT, name)
    for sub in ("input", "expected"):
        path = os.path.join(base, sub)
        shutil.rmtree(path, ignore_errors=True)
        os.makedirs(path)

    def write(path, header, rows):
        with open(path, "w", newline="\n") as f:
            f.write(header + "\n")
            for r in rows:
                f.write(r + "\n")

    for s in symbols:
        write(os.path.join(base, "input", s.stem() + ".csv"), MD_HEADER, s.md)
        write(os.path.join(base, "input", "depth_snapshots_%s_%s_%s.csv" % (s.venue, s.symbol, DATE)),
              SC_HEADER, s.sidecar)
        write(os.path.join(base, "expected", s.stem() + "_orderbook.csv"), OB_HEADER, s.expected)


# =================================================================================================

def spot_sync_overlap():
    s = Symbol("spot", "BTCUSDT")
    s.diff(0, 0, 0, 0, 99, 101, bids=[(97, 5)])            # contained in the snapshot: skipped
    s.diff(1, 0, 0, 1, 102, 104, bids=[(100, 3)], asks=[(103, 0)])  # U=102 <= L+1=103 <= u=104
    s.snapshot(2, 0, 0, 102, bids=[(100, 1), (99, 1), (98, 1)], asks=[(101, 1), (102, 1), (103, 1)])
    s.diff(3, 0, 0, 2, 105, 105, bids=[(99, 0)])
    s.diff(4, 0, 0, 3, 106, 107, asks=[(104, 2)])
    s.diff(5, 0, 0, 4, 107, 108, bids=[(97, 1)], asks=[(101, 0)])  # overlaps id=107 by one
    s.row(2, "R", "N", [(100, 3), (99, 1), (98, 1)], [(101, 1), (102, 1)])
    s.row(3, "D", "B", [(100, 3), (98, 1)], [(101, 1), (102, 1)])
    s.row(4, "D", "S", [(100, 3), (98, 1)], [(101, 1), (102, 1), (104, 2)])
    s.row(5, "D", "N", [(100, 3), (98, 1), (97, 1)], [(102, 1), (104, 2)])
    write_scenario("spot_sync_overlap", [s])


def usdm_bridge_boundary():
    btc = Symbol("usdm", "BTCUSDT")  # L == U
    btc.diff(0, 0, 0, 0, 200, 205, pu=199, bids=[(50, 2)])
    btc.snapshot(1, 0, 0, 199, bids=[(50, 1), (49, 1)], asks=[(51, 1), (52, 1)])  # L < U: too old
    btc.snapshot(2, 0, 0, 200, bids=[(50, 1), (49, 1)], asks=[(51, 1), (52, 1)])  # L == U
    btc.diff(3, 0, 0, 1, 206, 210, pu=205, asks=[(51, 3)])
    btc.row(2, "R", "N", [(50, 2), (49, 1)], [(51, 1), (52, 1)])
    btc.row(3, "D", "S", [(50, 2), (49, 1)], [(51, 3), (52, 1)])

    eth = Symbol("usdm", "ETHUSDT")  # L == u
    eth.diff(0, 0, 0, 0, 300, 305, pu=299, bids=[(20, 4)])
    eth.snapshot(1, 0, 0, 305, bids=[(20, 1), (19, 1)], asks=[(21, 1)])
    eth.diff(2, 0, 0, 1, 306, 306, pu=305, bids=[(19, 0)])
    eth.row(1, "R", "N", [(20, 4), (19, 1)], [(21, 1)])
    eth.row(2, "D", "B", [(20, 4)], [(21, 1)])
    write_scenario("usdm_bridge_boundary", [btc, eth])


def gap_resync():
    s = Symbol("spot", "BTCUSDT")
    s.diff(0, 0, 0, 0, 101, 101, bids=[(10, 2)])
    s.snapshot(1, 0, 0, 100, bids=[(10, 1), (9, 1)], asks=[(11, 1), (12, 1)])
    s.diff(2, 0, 0, 1, 102, 102, asks=[(11, 5)])
    s.diff(3, 0, 0, 2, 105, 106, bids=[(9, 3)])  # gap: 105 > id + 1 = 103
    s.trade(4, 0, 0, 3, 1, True)                  # Unsynced: no row
    s.diff(5, 0, 0, 4, 107, 107, bids=[(8, 1)])
    s.snapshot(6, 0, 0, 106, bids=[(10, 2), (9, 3)], asks=[(11, 5), (12, 1)])
    s.trade(7, 0, 0, 5, 2, False)
    s.row(1, "R", "N", [(10, 2), (9, 1)], [(11, 1), (12, 1)])
    s.row(2, "D", "S", [(10, 2), (9, 1)], [(11, 5), (12, 1)])
    s.row(6, "R", "N", [(10, 2), (9, 3), (8, 1)], [(11, 5), (12, 1)])
    s.row(7, "T", "S", [(10, 2), (9, 3), (8, 1)], [(11, 5), (12, 1)])
    write_scenario("gap_resync", [s])


def two_symbols_one_shard_reconnect():
    btc = Symbol("spot", "BTCUSDT")
    eth = Symbol("spot", "ETHUSDT")
    # Epoch 0: conn_seq interleaves across the two files.
    btc.diff(0, 0, 0, 0, 101, 101, bids=[(10, 2)])
    eth.diff(1, 0, 0, 1, 201, 201, asks=[(21, 3)])
    btc.snapshot(2, 0, 0, 100, bids=[(10, 1), (9, 1)], asks=[(11, 1)])
    eth.snapshot(3, 0, 0, 200, bids=[(20, 1)], asks=[(21, 1), (22, 1)])
    btc.trade(4, 0, 0, 2, 1, True)
    eth.trade(5, 0, 0, 3, 2, False)
    # Epoch 1 (reconnect): each symbol resets at its own first record.
    btc.trade(6, 0, 1, 0, 3, True)                       # reset: no row
    eth.diff(7, 0, 1, 1, 601, 601, asks=[(22, 0)])       # reset: buffered
    btc.diff(8, 0, 1, 2, 500, 500, bids=[(10, 7)])
    eth.snapshot(9, 0, 1, 600, bids=[(20, 1)], asks=[(21, 3), (22, 1)])
    btc.snapshot(10, 0, 1, 499, bids=[(10, 1), (9, 1)], asks=[(11, 1)])
    btc.row(2, "R", "N", [(10, 2), (9, 1)], [(11, 1)])
    btc.row(4, "T", "B", [(10, 2), (9, 1)], [(11, 1)])
    btc.row(10, "R", "N", [(10, 7), (9, 1)], [(11, 1)])
    eth.row(3, "R", "N", [(20, 1)], [(21, 3), (22, 1)])
    eth.row(5, "T", "S", [(20, 1)], [(21, 3), (22, 1)])
    eth.row(9, "R", "N", [(20, 1)], [(21, 3)])
    write_scenario("two_symbols_one_shard_reconnect", [btc, eth])


def coverage_exhaustion():
    s = Symbol("spot", "BTCUSDT")
    bids = [(p, 1) for p in range(2000, 1000, -1)]  # 1000 levels: bounded side, boundary 1001
    asks = [(2001, 1), (2002, 1)]                    # complete side
    s.diff(0, 0, 0, 0, 11, 11, bids=[(2000, 2)])
    s.snapshot(1, 0, 0, 10, bids=bids, asks=asks)
    s.diff(2, 0, 0, 1, 12, 12, bids=[(p, 0) for p in range(1999, 1005, -1)])  # margin 1000 -> 6
    s.diff(3, 0, 0, 2, 13, 13, bids=[(1005, 0)])                               # margin 5
    s.diff(4, 0, 0, 3, 14, 14, bids=[(1004, 0), (500, 9)])  # margin 4; 500 is beyond the boundary
    s.trade(5, 0, 0, 4, 1, True)                                               # Unsynced: no row
    s.snapshot(6, 0, 0, 14, bids=[(2000, 2), (1003, 1), (1002, 1), (1001, 1), (500, 9)], asks=asks)
    s.diff(7, 0, 0, 5, 15, 15, asks=[(2003, 1)])  # bridges the waiting snapshot
    s.row(1, "R", "N", [(2000, 2), (1999, 1), (1998, 1), (1997, 1), (1996, 1)], asks)
    s.row(2, "D", "B", [(2000, 2), (1005, 1), (1004, 1), (1003, 1), (1002, 1)], asks)
    s.row(3, "D", "B", [(2000, 2), (1004, 1), (1003, 1), (1002, 1), (1001, 1)], asks)
    s.row(7, "R", "N", [(2000, 2), (1003, 1), (1002, 1), (1001, 1), (500, 9)], asks + [(2003, 1)])
    write_scenario("coverage_exhaustion", [s])


def depth5_mismatch():
    s = Symbol("spot", "BTCUSDT")
    s.diff(0, 0, 0, 0, 101, 101, bids=[(10, 2)])
    s.snapshot(1, 0, 0, 100, bids=[(10, 1), (9, 1)], asks=[(11, 1), (12, 1)])
    s.depth5(2, 0, 0, 1, 101, [(10, 2), (9, 1)], [(11, 1), (12, 1)])  # full check: match
    s.depth5(3, 0, 0, 2, 101, [(10, 3), (9, 1)], [(11, 1), (12, 1)])  # full check: mismatch
    s.diff(4, 0, 0, 3, 102, 102, bids=[(9, 4)])
    s.snapshot(5, 0, 0, 102, bids=[(10, 3), (9, 4)], asks=[(11, 1), (12, 1)])
    s.diff(6, 0, 0, 4, 103, 103, asks=[(12, 0)])  # bridges the waiting snapshot
    s.row(1, "R", "N", [(10, 2), (9, 1)], [(11, 1), (12, 1)])
    s.row(2, "P", "N", [(10, 2), (9, 1)], [(11, 1), (12, 1)])
    s.row(6, "R", "N", [(10, 3), (9, 4)], [(11, 1)])
    write_scenario("depth5_mismatch", [s])


def depth5_beyond_coverage():
    s = Symbol("spot", "BTCUSDT")
    middle = ["6.%03d00000" % k for k in range(995, 0, -1)]  # 6.995 .. 6.001
    bids = [(10, 1), (9, 1), (8, 1), (7, 1)] + [(p, 1) for p in middle] + [(6, 1)]  # 1000 levels
    asks = [(12, 1), (13, 1)]
    s.diff(0, 0, 0, 0, 101, 101, bids=[(p, 0) for p in middle])  # trusted bids 10..6, boundary 6
    s.snapshot(1, 0, 0, 100, bids=bids, asks=asks)
    # depth5 at p=103, inside the next diff (U=102, u=104), which deletes 10 and adds 11: at 103,
    # 10 is gone and 11 not yet added, so depth5 shows 9..5; 5 lies beyond coverage.
    s.depth5(2, 0, 0, 1, 103, [(9, 1), (8, 1), (7, 1), (6, 1), (5, 1)], asks)
    s.diff(3, 0, 0, 2, 102, 104, bids=[(10, 0), (11, 1)])
    s.row(1, "R", "N", [(10, 1), (9, 1), (8, 1), (7, 1), (6, 1)], asks)
    s.row(2, "P", "N", [(10, 1), (9, 1), (8, 1), (7, 1), (6, 1)], asks)
    s.row(3, "D", "B", [(11, 1), (9, 1), (8, 1), (7, 1), (6, 1)], asks)
    write_scenario("depth5_beyond_coverage", [s])


def oversized_diff():
    s = Symbol("spot", "BTCUSDT")
    s.diff(0, 0, 0, 0, 101, 101, bids=[(10, 2)])
    s.snapshot(1, 0, 0, 100, bids=[(10, 1)], asks=[(11, 1)])
    s.diff(2, 0, 0, 1, 102, 102, bids=[("1", "0")] * 20001)  # one level over the limit
    s.diff(3, 0, 0, 2, 103, 103, bids=[(9, 1)])
    s.snapshot(4, 0, 0, 102, bids=[(10, 1)], asks=[(11, 1)])
    s.row(1, "R", "N", [(10, 2)], [(11, 1)])
    s.row(4, "R", "N", [(10, 1), (9, 1)], [(11, 1)])
    write_scenario("oversized_diff", [s])


def sync_by_later_diff():
    s = Symbol("spot", "BTCUSDT")
    s.diff(0, 0, 0, 0, 101, 101, bids=[(10, 2)])
    s.snapshot(1, 0, 0, 103, bids=[(10, 1), (9, 1)], asks=[(11, 1)])  # newer than every diff
    s.diff(2, 0, 0, 1, 102, 103, asks=[(11, 2)])                       # still contained
    s.diff(3, 0, 0, 2, 104, 104, bids=[(10, 5)])                       # bridges: R row here
    s.trade(4, 0, 0, 3, 1, True)
    s.row(3, "R", "N", [(10, 5), (9, 1)], [(11, 1)])
    s.row(4, "T", "B", [(10, 5), (9, 1)], [(11, 1)])
    write_scenario("sync_by_later_diff", [s])


def trade_side():
    s = Symbol("usdm", "BTCUSDT")
    s.diff(0, 0, 0, 0, 100, 102, pu=99, bids=[(30, 2)])
    s.snapshot(1, 0, 0, 101, bids=[(30, 1), (29, 1)], asks=[(31, 1)])
    s.trade(2, 0, 0, 1, 1, True)   # buyer is maker: the resting order was a bid
    s.trade(3, 0, 0, 2, 2, False)  # seller is maker: the resting order was an ask
    s.row(1, "R", "N", [(30, 2), (29, 1)], [(31, 1)])
    s.row(2, "T", "B", [(30, 2), (29, 1)], [(31, 1)])
    s.row(3, "T", "S", [(30, 2), (29, 1)], [(31, 1)])
    write_scenario("trade_side", [s])


if __name__ == "__main__":
    assert fnv_id("spot", "BTCUSDT") == 2014473836
    for make in (spot_sync_overlap, usdm_bridge_boundary, gap_resync,
                 two_symbols_one_shard_reconnect, coverage_exhaustion, depth5_mismatch,
                 depth5_beyond_coverage, oversized_diff, sync_by_later_diff, trade_side):
        make()
