# trade_side

Pins the `T` row's side (§3.4): the side of the resting (maker) order. `m=true` means the buyer
was the maker, so the resting order was a bid (`B`); `m=false` gives `S`. Trades never change the
book. Units as in `spot_sync_overlap`; literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=100 u=102 pu=99, bid 30@2 | buffered | — |
| 1 | snapshot L=101: bids 30, 29 @1; ask 31@1 | `100 ≤ 101 ≤ 102`: bridges | `R` bids 30@2 29@1, ask 31@1 |
| 2 | trade m=true | annotation | `T`,`B` |
| 3 | trade m=false | annotation | `T`,`S` |
