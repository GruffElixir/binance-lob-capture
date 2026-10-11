# oversized_diff

Pins §6.5: a diff with 20 001 levels (one over the window's capacity) is rejected while parsing
and handled as a gap: never truncated and never applied. Units as in `spot_sync_overlap`;
literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=101 u=101, bid 10@2 | buffered | — |
| 1 | snapshot L=100: bid 10@1; ask 11@1 | bridges | `R` bid 10@2, ask 11@1 |
| 2 | diff U=102 u=102 with 20 001 bid levels | `oversized_events`: Unsynced, window cleared | — |
| 3 | diff U=103 u=103, bid 9@1 | buffered | — |
| 4 | snapshot L=102: bid 10@1; ask 11@1 | 103 bridges (103 ≤ 103 ≤ 103) | `R` bids 10@1 9@1, ask 11@1 |
