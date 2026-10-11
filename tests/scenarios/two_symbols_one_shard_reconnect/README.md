# two_symbols_one_shard_reconnect

Two symbols share shard 0, so their `conn_seq` values interleave across the two files; the
connection then reconnects (epoch 1). Each file is replayed alone and must give its own output:
a symbol's book depends only on its own records, and each symbol resets at its own first record
of the new epoch. Units as in `spot_sync_overlap`; literal rows in `../generate.py`.

**BTCUSDT** (conn_seq 0, 2 in epoch 0; 0, 2 in epoch 1):

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | e0 diff U=101 u=101, bid 10@2 | first record: reset, buffered | — |
| 2 | e0 snapshot L=100: bids 10, 9 @1; ask 11@1 | bridges | `R` bids 10@2 9@1, ask 11@1 |
| 4 | e0 trade m=true | annotation | `T`,`B` |
| 6 | e1 trade | new epoch: reset, no row | — |
| 8 | e1 diff U=500 u=500, bid 10@7 | buffered | — |
| 10 | e1 snapshot L=499: bids 10, 9 @1; ask 11@1 | bridges | `R` bids 10@7 9@1, ask 11@1 |

**ETHUSDT** (conn_seq 1, 3 in epoch 0; 1 in epoch 1):

| t | Record | Book effect | Row |
|---|---|---|---|
| 1 | e0 diff U=201 u=201, ask 21@3 | first record: reset, buffered | — |
| 3 | e0 snapshot L=200: bid 20@1; asks 21, 22 @1 | bridges | `R` bid 20@1, asks 21@3 22@1 |
| 5 | e0 trade m=false | annotation | `T`,`S` |
| 7 | e1 diff U=601 u=601, ask 22 deleted | new epoch: reset, buffered | — |
| 9 | e1 snapshot L=600: bid 20@1; asks 21@3 22@1 | bridges | `R` bid 20@1, ask 21@3 |
