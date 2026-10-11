# gap_resync

Pins: a gap while Synced emits no row, the book stays silent until a new snapshot, and the
resync emits a fresh `R`. Units as in `spot_sync_overlap`; literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=101 u=101, bid 10@2 | buffered | — |
| 1 | snapshot L=100: bids 10, 9 @1; asks 11, 12 @1 | bridges (101 ≤ 101 ≤ 101) | `R` bids 10@2 9@1, asks 11@1 12@1 |
| 2 | diff U=102 u=102, ask 11@5 | applied | `D`,`S` asks 11@5 12@1 |
| 3 | diff U=105 u=106, bid 9@3 | gap (`105 > id+1 = 103`): Unsynced; the window restarts with it | — |
| 4 | trade | Unsynced | — |
| 5 | diff U=107 u=107, bid 8@1 | buffered | — |
| 6 | snapshot L=106: bids 10@2 9@3; asks 11@5 12@1 | 105–106 is contained; 107 bridges and is applied | `R` bids 10@2 9@3 8@1, asks 11@5 12@1 |
| 7 | trade, m=false | annotation | `T`,`S` (same book) |
