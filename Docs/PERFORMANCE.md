# Performance

How fast the JIT is, measured on one machine: CoreMark on one and on several threads against the same program compiled for the PC, against QEMU and against
the last release, then five small benchmark programs against the reference interpreter. These figures compare builds on the same
host. They do not predict how fast a game runs.

## CoreMark, one thread

CoreMark (November 25, 2026), measured on a Ryzen 7 5800X3D with the CPU governor on `performance` (boost on, about 4.44 GHz), every run
pinned to one core. Three interleaved runs per row, each at least ten seconds, every run validated by CoreMark itself. The
method and its limits are in [the JIT history](archive/JIT-history.md#coremark-native-and-multithreaded-2026-11-25).

| Build | Median iterations/s |
|---|---|
| Native x86_64 (gcc 16.2.1, `-O2`) | 38,647 |
| bifrost-emu 2.0.0-rc.1 (JIT) | 19,133 |
| bifrost-emu 2.0.0-beta.3 (JIT, headless build) | 14,898 |
| QEMU 11.1.3 user mode (TCG) | 7,648 |

On this integer and control-flow benchmark the JIT reaches about half (49.5%) of native speed, 2.5 times QEMU, and 28% more than
beta.3. The first run of each row was the slowest (by 2 to 5% for the emulated ones), so treat differences of a few percent as noise, and compare
numbers within one table, not across dates: earlier tables used another guest build of CoreMark.

## CoreMark, several threads

The same programs with one CoreMark thread per row count, not pinned. Iterations per second, all threads together, median of three runs.

| Threads | Native | bifrost-emu | QEMU | bifrost-emu / native |
|---|---|---|---|---|
| 1 | 38,647 | 19,133 | 7,648 | 50% |
| 2 | 76,967 | 34,916 | 16,071 | 45% |
| 4 | 151,774 | 68,021 | 31,713 | 45% |
| 8 | 297,122 | 130,007 | 61,101 | 44% |
| 16 | 404,756 | 136,246 | 67,004 | 34% |

The CPU has eight cores with two threads each. Emulated code fills a core, so eight threads scale well (6.8 times one) and the second eight add almost nothing (QEMU
shows the same), while native code still gains 36%. On eight threads beta.3 made 102,191, so rc.1 is 27% faster there too.

## Translating

How long it takes to turn guest code into host code matters while a game loads. A test that only translates (about 500,000 blocks), wall time, median
of seven runs, before and after several threads could translate at once:

| Threads | Before (2.0.0-beta.3 plus earlier changes) | 2.0.0-rc.1 |
|---|---|---|
| 1 | 2.49 s | 2.09 s |
| 2 | 3.53 s | 1.09 s |
| 4 | 3.71 s | 0.59 s |
| 8 | 3.99 s | 0.39 s |
| 16 | 4.14 s | 0.41 s |

Starting the Android runtime (window-free) takes 12% less translation time and 9% fewer CPU cycles than before the change. No game has been timed.

## Small programs

The `bench_*` programs (shipped build, pinned to one core, median of three runs after one warmup, each program's own timer except `bench_mips`, which
is process wall time; JIT compilation is included, so the millisecond-scale times are coarse):

| Benchmark | Interpreter | JIT | Speedup |
|---|---|---|---|
| fib(35) | 6.81 s | 0.064 s | ~106x |
| memcpy 256 MiB | 201 MiB/s | ~12,071 MiB/s | ~60x |
| qsort 100K ints | 4.31 s | 0.071 s | ~61x |
| matrix 256×256 | 13.2 MFLOPS | ~1,091 MFLOPS | ~83x |
| bench_mips (800M instructions) | ~53 MIPS | ~4,450 MIPS | ~83x |

Repeatable runs with checksums and host metadata: `Scripts/Bench/run_benchmarks.py`. The JIT needs SSE4.1 and
FMA3, and uses AVX2, SSE4.2, F16C, LZCNT and PCLMULQDQ where the host has them. It emits no AVX-512. Game profiling notes live on each app page, for
example [Doom 3](Apps/doom3.md).
