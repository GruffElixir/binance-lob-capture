# sync_by_later_diff

Pins §5.4: a snapshot newer than every buffered diff waits, and the `R` row carries the receive
time of the diff that later bridges it, not the snapshot's. Units as in `spot_sync_overlap`;
literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=101 u=101, bid 10@2 | buffered | — |
| 1 | snapshot L=103: bids 10, 9 @1; ask 11@1 | no buffered diff has `u > L`: waits | — |
| 2 | diff U=102 u=103, ask 11@2 | contained (`u ≤ L`): still waits | — |
| 3 | diff U=104 u=104, bid 10@5 | bridges (104 ≤ 104 ≤ 104); only this diff is applied | `R` at t=3: bids 10@5 9@1, ask 11@1 |
| 4 | trade m=true | annotation | `T`,`B` |
