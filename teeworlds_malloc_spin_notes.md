# teeworlds startup hang — glibc `_int_malloc` smallbin->tcache stash loop spin

Date: 2026-08-18
Status: ROOT-CAUSED (guest heap corruption → legitimate glibc livelock). Not a JIT bug.

## Symptom
- teeworlds (`ctest_real/teeworlds/teeworlds`, ET_EXEC, entry 0x406cc0) hangs at startup.
- NO console output at all (not even "running on unix-linux"), no window.
- Process alternates S/R, sometimes 98.8% CPU; 3 threads (main + 2 host libusb threads).
- Both JIT and interp (`BIFROST_NO_JIT=1`) hang at the same spot → guest-side, not codegen.

## Root cause
glibc `_int_malloc` smallbin→tcache "stash" refill loop (malloc.c ~4112 in glibc 2.42)
livelocks when a smallbin's `bk` chain contains a NULL while the bin is non-empty.

```c
  size_t tc_idx = csize2tidx (nb);
  if (tcache != NULL && tc_idx < mp_.tcache_small_bins)
    {
      mchunkptr tc_victim;
      while (tcache->num_slots[tc_idx] != 0
             && (tc_victim = last (bin)) != bin)
        {
          if (tc_victim != NULL)
            {
              bck = tc_victim->bk;
              set_inuse_bit_at_offset (tc_victim, nb);
              if (av != &main_arena)
                set_non_main_arena (tc_victim);
              bin->bk = bck;
              bck->fd = bin;
              tcache_put (tc_victim, tc_idx);
            }
        }
    }
```

When `bin->bk == NULL` but `bin != NULL`: `last(bin)` returns NULL forever, the
`if (tc_victim != NULL)` body (the only thing that mutates `bin->bk`) is skipped,
and the `while` test `bin->bk != bin` stays true → infinite loop at libc offset
0x93450–0x9345c. **Real AArch64 hardware would spin identically.** glibc assumes
`last(bin)` is never NULL (well-formed bin is self-linked-empty or points to a real
chunk).

## Versions / key facts
- Rootfs libc is **glibc 2.42** (Arm GNU Toolchain 15.2.Rel1), NOT Debian bookworm 2.36:
  `strings -a rootfs/lib/libc.so.6 | rg "GNU C Library"`
  → `GNU C Library (Arm GNU Toolchain 15.2.Rel1 (Build arm-15.86)) stable release version 2.42.`
- 2.42 semantics: `num_slots[]` (uint16_t, 76 bins) is FREE CAPACITY (inits to
  `mp_.tcache_count`=7); put decrements, get increments. `num_slots==0` ⇒ bin full.
- `tcache_perthread_struct` = `uint16_t num_slots[76]` (0x98 bytes) + `entries[76]` at offset 0x98.

## Instruction → register mapping (libc.so.6 file offsets)
Function `_int_malloc` starts at file offset 0x92f44. Spin region 0x93410–0x93460:

```
0x93410: adrp x3, 19f000
0x93414: ldr  x3, [x3, #3424]      ; tcache TLS offset (GOT 0x19fd60 = 0x50, +8)
0x93418: mrs  x2, tpidr_el0        ; TLS base — TLS path is CORRECT (ruled out)
0x9341c: add  x2, x2, x3
0x93420: ldr  x3, [x2, #8]         ; x3 = tcache
0x93424: cbz  x3, 0x93460          ; tcache == NULL → skip
0x93428: adrp x6, 1a0000
0x9342c: lsr  x5, x27, #4          ; x5 = nb>>4
0x93430: sub  x2, x5, #2           ; x2 = tc_idx
0x93434: ldr  x6, [x6, #536]       ; mp_.tcache_small_bins
0x93438: cmp  x6, x2
0x9343c: b.ls 0x93460              ; tc_idx >= tcache_small_bins → skip
0x93440: add  x7, x3, w5, uxtw #1  ; x7 = tcache + tc_idx*2
0x93444: ldurh w6, [x7, #-4]       ; w6 = num_slots[tc_idx]
0x93448: cbnz w6, 0x93454          ; num_slots != 0 → loop
0x9344c: b    0x93460              ; bin full → exit
0x93450: cbnz x2, 0x937d4          ; LOOP: tc_victim != NULL → stash body
0x93454: ldr  x2, [x1, #24]        ; x2 = bin->bk = last(bin)  ← re-read, x1 fixed
0x93458: cmp  x1, x2               ; bin vs last(bin)
0x9345c: b.ne 0x93450              ; != bin → loop  (SPIN when x2==NULL forever)
0x93460: ...                       ; return chunk2mem(victim)
```

Register map:
| reg | value |
|-----|-------|
| x24 | av (arena, arg0) |
| x27 | nb (0x20 here; (bytes+0x17)&~0xf) |
| x1  | bin head = `bin_at(av, smallbin_index(nb))` = av+0x60+(idx-1)*16; for nb=0x20 → av+0x70 |
| x2  | tc_victim = `last(bin)` = bin->bk, loaded `[x1,#24]` |
| x3  | tcache (from TLS, correct) |
| x5  | tc_idx = (nb>>4)-2 |
| x6  | num_slots[tc_idx] (16-bit) |

Stash body 0x937d4–0x93838: unlink (`bin->bk=bck; bck->fd=bin`) + tcache_put
(entries[tc_idx] at `tcache + (nb>>4)*8 + 0x88` = 0x98 + tc_idx*8); loops back to 0x93448.

## Corrupt data
- The corrupt word is the **`bk` of a free 0x20-smallbin chunk** = `chunk+0x18` = `user+0x8`
  (only 8 bytes past the previous chunk's user buffer).
- For nb=0x20 the failing slot is `av + 0x88` = `&av->bins[3]` (or a chunk's bk in that bin).
- NOT the tcache struct, NOT the TLS tcache pointer (verified correct).

## Root-cause candidates (not yet narrowed)
1. Chunk freed twice → smallbin re-entry with scrambled links.
2. Heap overflow ≥8 bytes from a neighboring ≤32-byte allocation (zeroes next chunk's bk).
3. Use-after-free: game scribbles freed-chunk metadata.
4. Emulator zeroing/writing arena memory (must rule in/out via diagnostic).

Note: tcache double-free itself is caught by the `e->key == tcache_key` check, so this
corruption came through the bin path, not the tcache path.

## Diagnostic next step (TODO)
At spin time, dump arena around `av+0x70..0x90`, walk the `bk` chain to find the NULL
node; compare `main_arena` against known-good init (bins self-linked) to rule out an
emulator write-through bug. Then find WHO zeroes it (heap overflow vs double-free).

## Environment / reproduction
- Run from `ctest_real/teeworlds/`:
  `BIFROST_ROOT=.../rootfs DISPLAY=:0 timeout -k 3 12 .../bifrost-emu ./teeworlds`
- Last translated JIT blocks (BIFROST_JIT_DUMP=1) map to libc offsets 0x93428–0x93838
  (= spin cluster), NOT freetype (earlier mis-attribution — lib base shifts per run).
- Library bases per-run via `BIFROST_DYNLINK_TRACE=1` (libc.so.6 base e.g. 0x300d9000).

## Files / artifacts
- /tmp/opencode/tw16.log — BIFROST_DYNLINK_TRACE + BIFROST_JIT_DUMP capture (block IR of spin)
- /tmp/opencode/tw_interp.log — interp-mode run (also hangs, empty output)
- rootfs/lib/libc.so.6 — glibc 2.42, function _int_malloc @ 0x92f44
