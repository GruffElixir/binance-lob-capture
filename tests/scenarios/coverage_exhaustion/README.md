# coverage_exhaustion

Pins §5.3: a side whose snapshot holds the full 1000 levels is bounded at its worst price, and
rows stop as soon as fewer than 5 levels at or better than that boundary remain. Units as in
`spot_sync_overlap`; literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=11 u=11, bid 2000@2 | buffered | — |
| 1 | snapshot L=10: 1000 bids 2000…1001 @1 (boundary 1001); asks 2001, 2002 @1 (complete) | bridges; bid margin 1000 | `R` bids 2000@2 1999 1998 1997 1996 |
| 2 | diff U=12: deletes bids 1999…1006 (994 levels) | margin 6 (2000, 1005…1001) | `D`,`B` bids 2000@2 1005 1004 1003 1002 |
| 3 | diff U=13: deletes 1005 | margin 5 | `D`,`B` bids 2000@2 1004 1003 1002 1001 |
| 4 | diff U=14: deletes 1004, adds 500@9 | 500 lies beyond the boundary and does not count: margin 4 → Unsynced (`coverage_exhausted`) | — |
| 5 | trade | Unsynced | — |
| 6 | snapshot L=14: bids 2000@2 1003 1002 1001 500@9 (complete); asks 2001, 2002 | every buffered diff is contained: waits | — |
| 7 | diff U=15 u=15, ask 2003@1 | bridges the waiting snapshot | `R` bids 2000@2 1003 1002 1001 500@9, asks 2001 2002 2003 |

Unlisted quantities are 1.
