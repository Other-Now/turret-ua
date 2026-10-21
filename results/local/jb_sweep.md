60 s per run, 1% network loss, 10 seeds per cell (mean)

### lan (sd 2 ms)

| depth ms | late % | unplayed % (late + lost) | mean RFC 3550 J ms |
|---:|---:|---:|---:|
| 20 | 0.00 | 0.90 | 1.3 |
| 40 | 0.00 | 0.99 | 1.3 |
| 60 | 0.00 | 1.03 | 1.3 |
| 80 | 0.00 | 1.03 | 1.3 |
| 100 | 0.00 | 0.97 | 1.3 |
| 140 | 0.00 | 0.98 | 1.3 |
| 200 | 0.00 | 0.98 | 1.3 |

smallest depth with late < 1%: 20 ms = 15.4 x J

### wan (sd 10 ms)

| depth ms | late % | unplayed % (late + lost) | mean RFC 3550 J ms |
|---:|---:|---:|---:|
| 20 | 0.73 | 1.63 | 6.5 |
| 40 | 0.00 | 0.99 | 6.6 |
| 60 | 0.00 | 1.03 | 6.6 |
| 80 | 0.00 | 1.03 | 6.5 |
| 100 | 0.00 | 0.97 | 6.6 |
| 140 | 0.00 | 0.98 | 6.5 |
| 200 | 0.00 | 0.98 | 6.5 |

smallest depth with late < 1%: 20 ms = 3.1 x J

### congested (sd 25 ms)

| depth ms | late % | unplayed % (late + lost) | mean RFC 3550 J ms |
|---:|---:|---:|---:|
| 20 | 17.96 | 18.87 | 17.0 |
| 40 | 4.97 | 5.96 | 17.3 |
| 60 | 0.91 | 1.94 | 17.3 |
| 80 | 0.04 | 1.07 | 17.1 |
| 100 | 0.02 | 0.99 | 17.2 |
| 140 | 0.00 | 0.99 | 17.1 |
| 200 | 0.00 | 0.98 | 17.1 |

smallest depth with late < 1%: 60 ms = 3.5 x J

### wifi spikes (sd 5 + 2% x150 ms)

| depth ms | late % | unplayed % (late + lost) | mean RFC 3550 J ms |
|---:|---:|---:|---:|
| 20 | 1.95 | 2.92 | 7.5 |
| 40 | 2.02 | 3.03 | 7.7 |
| 60 | 1.92 | 2.93 | 7.4 |
| 80 | 1.90 | 2.90 | 7.6 |
| 100 | 1.30 | 2.34 | 7.5 |
| 140 | 0.21 | 1.24 | 7.4 |
| 200 | 0.00 | 0.98 | 7.9 |

smallest depth with late < 1%: 140 ms = 17.8 x J

