# depth5_mismatch

Pins §5.6: a full depth5 check that matches emits its `P` row; one that does not match emits no
row and forces a resync. Units as in `spot_sync_overlap`; literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=101 u=101, bid 10@2 | buffered | — |
| 1 | snapshot L=100: bids 10, 9 @1; asks 11, 12 @1 | bridges; id=101 | `R` bids 10@2 9@1, asks 11@1 12@1 |
| 2 | depth5 id=101, equal to the book | full check (`p == id`): verified | `P` (same book) |
| 3 | depth5 id=101 with bid 10@3 | full check: mismatch → Unsynced | — |
| 4 | diff U=102 u=102, bid 9@4 | buffered | — |
| 5 | snapshot L=102: bids 10@3 9@4; asks 11, 12 | every buffered diff is contained: waits | — |
| 6 | diff U=103 u=103, ask 12 deleted | bridges | `R` bids 10@3 9@4, ask 11@1 |
