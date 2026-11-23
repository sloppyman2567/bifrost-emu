# Performance

The published CoreMark and `bench_*` figures. The [project site](https://sloppyman2567.github.io/bifrost-emu/jit/) reads the two tables below, so keep their headers when re-measuring.

CoreMark (November 23, 2026), measured on a Ryzen 7 5800X3D with the CPU governor on `performance` (boost on), every run pinned to
one core: 200,000 iterations, one warmup and three interleaved measured runs per binary, every CRC validated. The method and its
limits are in [the JIT history](archive/JIT-history.md#coremark-and-benchmark-re-measurement-2026-11-23).

| Build | Median iterations/s |
|---|---|
| bifrost-emu at `68a384ca` (JIT, shipped SDL/GL build) | 16,880 |
| same code, headless build | 16,889 |
| bifrost-emu 2.0.0-alpha.2 (JIT, headless build) | 12,635 |
| QEMU 11.1.3 user mode (TCG) | 8,024 |

On this integer and control-flow benchmark the shipped build runs about 2.1 times as many iterations per second as QEMU, and the
headless build of the same code is about 34% faster than the alpha.2 release built the same way. Neither predicts game FPS. The runs
spread 0.3% to 0.6%, so differences of about 1% are noise: compare numbers within one table, not across dates.

The `bench_*` programs (shipped build, pinned to one core, median of three runs, each program's own timer except `bench_mips`, which
is process wall time; JIT compilation is included, so the millisecond-scale times are coarse):

| Benchmark | Interpreter | JIT | Speedup |
|---|---|---|---|
| fib(35) | 7.22 s | 0.065 s | ~111x |
| memcpy 256 MiB | 185 MiB/s | ~13,956 MiB/s | ~75x |
| qsort 100K ints | 4.35 s | 0.071 s | ~61x |
| matrix 256×256 | 12.7 MFLOPS | ~1,086 MFLOPS | ~86x |
| bench_mips (800M instructions) | ~52 MIPS | ~4,482 MIPS | ~86x |

Repeatable runs with checksums and host metadata: `Scripts/Bench/run_benchmarks.py`. The JIT uses AVX-512, AVX2, SSE4.2,
AES-NI/PCLMULQDQ/SHA-NI, BMI1/BMI2 and FMA3 where the host has them, with fallbacks. Game profiling notes live on each app page, for
example [Doom 3](Apps/doom3.md).
