# usdm_bridge_boundary

Pins the USD-M bridge rule `U ≤ L ≤ u` at both edges, one file per edge, plus the snapshot just
outside the lower edge. Units as in `spot_sync_overlap`; literal rows in `../generate.py`.

**BTCUSDT (L == U):**

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=200 u=205 pu=199, bid 50@2 | buffered | — |
| 1 | snapshot L=199 | the first diff with `u ≥ L` is 200–205, but `U = 200 > L`: too old, discarded | — |
| 2 | snapshot L=200: bids 50, 49 @1; asks 51, 52 @1 | `200 ≤ 200 ≤ 205`: bridges; the diff is applied | `R` bids 50@2 49@1, asks 51@1 52@1 |
| 3 | diff U=206 u=210 pu=205, ask 51@3 | `pu == id`: continues | `D`,`S` asks 51@3 52@1 |

**ETHUSDT (L == u):**

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=300 u=305 pu=299, bid 20@4 | buffered | — |
| 1 | snapshot L=305: bids 20, 19 @1; ask 21@1 | not contained (`u < L` is false); `300 ≤ 305 ≤ 305` bridges | `R` bids 20@4 19@1, ask 21@1 |
| 2 | diff U=306 u=306 pu=305, bid 19 deleted | continues | `D`,`B` bid 20@4 |
