Environment: 2 CPU cores. Median of 5 runs (± = standard deviation). CPU = user+sys time of all processes.

### Experiment 1: execution time vs input size

| Lines | Single (ms) | Pipeline (ms) | Speedup | CPU single (ms) | CPU pipeline (ms) |
|---:|---:|---:|---:|---:|---:|
| 1,000 (0.1 MB) | 2.4 ± 0.3 | 3.3 ± 0.4 | 0.72x | 2 | 3 |
| 10,000 (0.6 MB) | 2.7 ± 0.4 | 5.0 ± 0.4 | 0.53x | 2 | 6 |
| 100,000 (5.8 MB) | 8.2 ± 0.5 | 25.3 ± 1.3 | 0.33x | 8 | 43 |
| 1,000,000 (57.9 MB) | 66.6 ± 8.3 | 474.0 ± 114.1 | 0.14x | 65 | 767 |
| 2,000,000 (115.9 MB) | 123.0 ± 10.8 | 613.1 ± 44.3 | 0.20x | 123 | 981 |

### Experiment 2: speedup vs work per record (100,000 lines)

| Work (-w) | Single (ms) | Pipeline (ms) | Speedup | CPU single (ms) | CPU pipeline (ms) |
|---:|---:|---:|---:|---:|---:|
| 0 | 8.6 ± 0.3 | 27.0 ± 0.6 | 0.32x | 8 | 46 |
| 2 | 38.7 ± 2.2 | 50.1 ± 2.8 | 0.77x | 38 | 82 |
| 5 | 94.9 ± 2.7 | 82.6 ± 3.0 | 1.15x | 94 | 142 |
| 10 | 187.9 ± 5.5 | 146.8 ± 17.9 | 1.28x | 187 | 243 |
| 20 | 379.9 ± 10.4 | 261.5 ± 14.7 | 1.45x | 375 | 439 |
| 50 | 894.1 ± 5.6 | 606.0 ± 52.8 | 1.48x | 892 | 1011 |
| 100 | 1781.9 ± 39.5 | 1174.3 ± 22.2 | 1.52x | 1779 | 1971 |

### Experiment 3: pipe buffer size (2,000,000 lines)

| Pipe buffer | Single (ms) | Pipeline (ms) | Speedup | CPU single (ms) | CPU pipeline (ms) |
|---:|---:|---:|---:|---:|---:|
| 64 KB (default) | 149.5 ± 14.0 | 910.2 ± 231.3 | 0.16x | 145 | 1219 |
| 256 KB | 142.4 ± 28.9 | 931.2 ± 328.8 | 0.15x | 136 | 1130 |
| 1 MB | 146.7 ± 93.8 | 904.9 ± 527.5 | 0.16x | 143 | 1110 |
