# depth5_beyond_coverage

The §5.6 example: trusted bids 10–6 with boundary 6, and a diff that deletes 10 and adds 11.
depth5 taken inside that diff shows 9–5, and 5 was never known. A correct partial check compares
only 9, 8, 7, 6 (and the complete ask side), so there is no false mismatch. All quantities are 1;
literal rows in `../generate.py`.

| t | Record | Book effect | Row |
|---|---|---|---|
| 0 | diff U=101 u=101: deletes the 995 bids 6.995…6.001 | buffered | — |
| 1 | snapshot L=100: 1000 bids 10, 9, 8, 7, 6.995…6.001, 6 (boundary 6); asks 12, 13 (complete) | bridges; bids 10 9 8 7 6, margin exactly 5 | `R` bids 10 9 8 7 6, asks 12 13 |
| 2 | depth5 p=103: bids 9 8 7 6 5; asks 12 13 | `p > id`: pending check; book unchanged | `P` (same book) |
| 3 | diff U=102 u=104: bid 10 deleted, 11 added | `U ≤ p < u`: partial check before applying. Bid prices in depth5's range, inside coverage (≥ 6) and untouched by the diff: 9, 8, 7, 6, all equal. 5 is beyond coverage: unchecked. Asks (complete side): equal. Verified; the diff is applied; margin 5 (11 9 8 7 6) | `D`,`B` bids 11 9 8 7 6, asks 12 13 |
