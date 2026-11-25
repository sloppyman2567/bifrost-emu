# Performance

How fast the JIT is on one PC. These numbers are for comparing builds on the same machine. They don't tell you how fast a game will run.

## CoreMark, one thread

CoreMark (November 25, 2026), measured on a Ryzen 7 5800X3D with the CPU governor on `performance`. Each number is the median of three runs of at least ten
seconds, pinned to one core. [How it was measured](archive/JIT-history.md#coremark-native-and-multithreaded-2026-11-25).

| Build | Median iterations/s |
|---|---|
| Native x86_64 (gcc 16.2.1, `-O2`) | 38,647 |
| bifrost-emu 2.0.0-rc.1 (JIT) | 19,133 |
| bifrost-emu 2.0.0-beta.3 (JIT, headless build) | 14,898 |
| QEMU 11.1.3 user mode (TCG) | 7,648 |

That's about half of native speed, 2.5 times QEMU, and 28% faster than beta.3. Runs differ by a few percent, and older versions of this page used a different build of
CoreMark, so only compare numbers from the same table.

## CoreMark, several threads

Iterations per second for all threads together.

| Threads | Native | bifrost-emu | QEMU | bifrost-emu / native |
|---|---|---|---|---|
| 1 | 38,647 | 19,133 | 7,648 | 50% |
| 2 | 76,967 | 34,916 | 16,071 | 45% |
| 4 | 151,774 | 68,021 | 31,713 | 45% |
| 8 | 297,122 | 130,007 | 61,101 | 44% |
| 16 | 404,756 | 136,246 | 67,004 | 34% |

This CPU has eight cores with two threads each. Going from eight threads to sixteen helps native code but hardly helps Bifrost or QEMU. Beta.3 reached
102,191 with eight threads, so rc.1 is 27% faster there.

## Translating code

How long it takes to translate about 500,000 blocks of code, before and after several threads could do it at once (median of seven runs):

| Threads | Before | 2.0.0-rc.1 |
|---|---|---|
| 1 | 2.49 s | 2.09 s |
| 2 | 3.53 s | 1.09 s |
| 4 | 3.71 s | 0.59 s |
| 8 | 3.99 s | 0.39 s |
| 16 | 4.14 s | 0.41 s |

"Before" is the version just before this change, with Carry already on, not beta.3. Compared with it, starting the Android runtime spends 12% less time translating
and uses 10% fewer CPU cycles. Game loading times haven't been measured yet.

## Small programs

Each benchmark with the interpreter and with the JIT, pinned to one core (median of three runs). The JIT times include translating.

| Benchmark | Interpreter | JIT | Speedup |
|---|---|---|---|
| fib(35) | 6.81 s | 0.064 s | ~106x |
| memcpy 256 MiB | 201 MiB/s | ~12,071 MiB/s | ~60x |
| qsort 100K ints | 4.31 s | 0.071 s | ~61x |
| matrix 256×256 | 13.2 MFLOPS | ~1,091 MFLOPS | ~83x |
| bench_mips (800M instructions) | ~53 MIPS | ~4,450 MIPS | ~83x |

You can run them yourself with `Scripts/Bench/run_benchmarks.py`. The JIT needs SSE4.1 and FMA3, and uses AVX2, SSE4.2, F16C, LZCNT and PCLMULQDQ when the CPU
has them. It doesn't use AVX-512. Notes on single games are on their pages, for example [Doom 3](Apps/doom3.md).
