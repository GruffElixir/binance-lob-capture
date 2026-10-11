# spot_sync_overlap

Pins the spot bridge rule `U ≤ L+1 ≤ u`, a buffered diff that overlaps the snapshot, and the
first row being `R`. Prices and quantities are in whole units (×10^8 in the CSV); `t` is
milliseconds after the first record. The literal expected rows are in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=99 u=101, bid 97@5 | buffered | — |
| 1 | diff U=102 u=104, bid 100@3, ask 103 deleted | buffered | — |
| 2 | snapshot L=102: bids 100, 99, 98 @1; asks 101, 102, 103 @1 | diff 99–101 is contained (`u ≤ L`), so 97@5 is never applied. Diff 102–104 bridges (102 ≤ 103 ≤ 104) and is applied on top of the snapshot. id=104 | `R` bids 100@3 99@1 98@1, asks 101@1 102@1 |
| 3 | diff U=105 u=105, bid 99 deleted | applied | `D`,`B` bids 100@3 98@1 |
| 4 | diff U=106 u=107, ask 104@2 | applied | `D`,`S` asks 101@1 102@1 104@2 |
| 5 | diff U=107 u=108 (overlaps id=107), bid 97@1, ask 101 deleted | `U ≤ id+1`: continues | `D`,`N` bids 100@3 98@1 97@1, asks 102@1 104@2 |
