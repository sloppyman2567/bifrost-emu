# Plan: Fix QtGui windowed mode (qtgui_test.elf on Xwayland :0)

## Objective
Get `qtgui_test.elf` rendering a real visible window on X display `:0` (Xwayland)
via the guest xcb plugin, and produce the app's `QWidget::grab()` screenshot
(`rootfs/tmp/qtgui_render.png`). **Offscreen mode FULLY PASSES** (exit 0 + valid
PNG with content) — the entire Qt raster pipeline works. Windowed (xcb) mode is
blocked by a **guest Qt 5.15 D-Bus/a11y deadlock: `QDBusConnection::sessionBus()`
called synchronously from `QWidget::show()` (accessibility bridge) parks t1 on a
`QSemaphore::acquire` that only the never-running main event loop can release**
(suspended-delivery; see STATUS UPDATE 7). The emulator's D-Bus data path is
proven correct by a standalone libdbus probe. An earlier racy `malloc(): unaligned
tcache` heap-corruption suspicion (STATUS UPDATE 6) has NOT reproduced and is
dropped.

## Environment run discipline
- Repo: `/home/gamingpc/Downloads/bifrost-emu-1.5.0-alpha`; builds clean with
  `make -j8`. Debug runs use `BIFROST_NO_JIT=1` (JIT currently fails earlier at
  dlopen/rtld init, exit 134 — a separate, de-prioritized issue).
- Env: `DISPLAY=:0`,
  `XAUTHORITY=/run/user/1000/xauth_OKmOmC` (name changes per session; cookie
  `351765bae982691653bb1e32ad487345`), `BIFROST_ROOT=$PWD/rootfs`.
- Pre-run hygiene: `pkill -9 -x bifrost-emu`; `rm -f
  /tmp/opencode/xseq.bin`; `rm -f rootfs/var/cache/fontconfig/*.cache-8`.
- `timeout -k 2 <N>` is mandatory — the emulator ignores SIGTERM; plain `timeout`
  hangs the shell for 120s.
- Run template:
  `timeout -k 2 8 env BIFROST_NO_JIT=1 BIFROST_ROOT=$PWD/rootfs DISPLAY=:0
  XAUTHORITY=/run/user/1000/xauth_OKmOmC BIFROST_XTRACE=1
  BIFROST_XSEQLOG=/tmp/opencode/xseq.bin ./bifrost-emu
  rootfs/usr/local/bin/qtgui_test.elf > /tmp/opencode/runN.log 2>&1`

## STATUS UPDATES (this session)
- **2026-08-05 (1): WINDOW NOW OPENS.** Root cause of "no window" was the
  single-structure LD1 mis-decode (see "ROOT CAUSE FOUND & FIXED" section
  below): `ld1 {v0.h}[2]` wrote to lane 0 instead of lane 2, corrupting
  xcb_create_window's SIMD width/height packing → width=0 → BadValue. Fixed in
  interp_fp.cpp; verified with vecpack_isolate.elf.
- **2026-08-05 (2): NEW BUG — window is pure black.** After the fix the window
  opens (CreateWindow accepted, MapWindow ok, window exists on screen), but the
  content is black. X wire analysis (`[XREQ]`/`[XPRE]`/`xseq`):
  - Guest renders TEXT via the **RENDER extension** (op 0x8c): CompGlyphs into
    internal pictures (ids 0x21/0x1b9/0xba, NOT the window 0x1400005) —
    these are Qt font glyph caches, not the window blit.
  - The guest sends **NO image blit whatsoever**: no core PutImage (op 0x48/72),
    no MIT-SHM ShmPutImage, no RENDER FillRectangles/Composite clip to the
    window. The 520x320 backing-store QImage is never uploaded to the X server.
  - Main window 0x1400005 receives only PropertyNotify + one Damage event; its
    backing store flush path is never exercised (or fails before any X write).
  - So the LD1 fix restored the window but the RASTER BACKING STORE UPLOAD path
    is the next thing to fix. Candidate locus: Qt raster flush
    (QXcbBackingStore → xcb_put_image / libxcb-image / MIT-SHM shm) — whether it
    is reached at all and whether pointer-args (QImage data / shm segment) are
    marshalled correctly.
  - TODO NEXTPROBE: trace guest calls to `xcb_put_image`, `xcb_shm_put_image`,
    `xcb_image_put`, `xcb_render_c`, and the MIT-SHM attach/create; confirm
    whether the backing-store flush is reached and where it stalls.
- **2026-08-05 (3): FLUSH PATH IS NEVER REACHED — two-thread X-socket starvation.
  The backing-store flush (`xcb_put_image`@0x5103d07150) resolves but is called
  ZERO times. X wire re-decode (correct event offsets: Expose window field is at
  +4, MapNotify/ConfigureNotify at +8) confirms MAIN window 0x1400005 DOES
  receive MapNotify + Expose(x0 y0 520x320 count0) + 3x ConfigureNotify via
  recvmsg (xcap.recv rec72). Latest PPOLL run (BIFROST_PPOLL_TRACE added to
  ppoll case in misc_io.cpp) reveals the real blocker — TWO guest threads share
  fd 3:
    - t1 (GUI/main): polls fd3 events=0x5 (POLLIN|POLLOUT) → ALWAYS gets
      revents=0x4 (POLLOUT only; never POLLIN). 75 polls, all 0x4. → main thread
      never sees the socket readable → never dispatches Expose → never paints.
    - t2 (reader): polls fd3 events=0x1 (POLLIN) → ALWAYS gets revents=0x0x1
      (POLLIN). 52 polls, all 0x1. → t2 greedily consumes EVERY byte off host
      fd 11 the instant it arrives, starving t1 of POLLIN.
  Events read by t2 sit in ITS queue; the widget's paint/flush path (which needs
  t1) is never driven → black window + zero X writes after MapWindow. Hypothesis:
  either (a) Qt XCB reader-thread config: impossible to have BOTH a dedicated
  reader (t2, recvmsg) AND main-thread (t1) direct-poll on the same fd, so this
  is likely an emulator fd-relay issue where both threads resolve to host fd 11
  and t2 drains first; or (b) this Qt build expects main (t1) to read X directly
  but t2 (QSocketNotifier reader) steals the bytes. Next: identify what t2 is
  (which guest function/lambda) and whether the emulator fd sharing is correct;
  confirm t1 reads ZERO X bytes while t2 reads all.
- **2026-08-05 (4): CONFIRMED — t1=GUI writer, t2=event reader, GUI starved.
  Thread-id trace on the X relay (XPRE/XHOST recvmsg in misc.cpp, XREQ writev in
  fs.cpp) splits cleanly:
    - t1 (main GUI): ALL 75 X WRITES (XREQ tid=1), only ~26/20 early READS
      (setup phase), then spins on ppoll(fd3, events=0x5) getting POLLOUT(0x4)
      only — never POLLIN (t2 drains). Zero X writes after MapWindow → paint
      flush (`xcb_put_image`@0x5103d07150) never reached.
    - t2 (Qt QXcbEventReader): ALL 624 post-setup recvmsg READS (XPRE/XHOST
      tid=2, always POLLIN). Consumes MapNotify+Expose+ConfigureNotify for
      0x1400005 into ITS queue; t1 never dispatches them.
    - t3: poll(fd6 hfd14 POLLIN) ret=0 — idle helper, unrelated.
  Events ARE delivered, window id/masks ARE correct, but the Expose never reaches
  the widget paint path. Either (a) emulator fd-relay race: both threads map
  guest fd3→host fd11, whichever poll/recv runs first consumes, so t1's xcb_read
  finds an empty socket; or (b) guest/Qt threading broken by emulator scheduling.
  NEXT: determine whether t1 ever calls xcb_read/xcb_poll_for_queued_event and
  gets nothing (fd-relay race) vs never being dispatched.
- **2026-08-05 (5): DECISIVE — it is NOT an fd-relay race (a); it is (b)
  guest/Qt threading: the main thread blocks in a plain `futex(WAIT)` that no
  thread ever wakes. Evidence (NO_ASLR run, module bases fixed, XCBIMG +
  PPOLL + FUTEX traces in /tmp/opencode/futx.log):
    - t1's two ppoll call-sites both sit inside libc's own
      `__syscall_cancel_arch_end` (0x89aec) — they are the xcb `_xcb_conn_wait`
      fd-wait, not a Qt event loop. nfds=1, blocking (to=-1), and the constant
      `events=0x5` (POLLIN|POLLOUT) matches xcb requesting POLLOUT exactly when
      `out->queue_len != 0` (pending output) — NOT a permanent Qt write notifier.
    - `xcb_flush` is only ever called 9× by t1, all during setup; the writev
      relay (fs.cpp case 66) returns full counts (XWRSHORT 0 hits) so the guest
      out-queue drains fine. After MapWindow t1 does ZERO writev / ZERO xcb_flush.
    - post-MapWindow t1 is SILENT in the syscall trace: no ppoll spin at all in
      the FINAL runs (earlier "spin" was a different phase). t2 (Qt
      QXcbEventReader) does 978 xcb_poll_for_queued_event/wait_for_event calls
      and 725 ppoll(POLLIN) — it reads every Expose/ConfigureNotify.
    - The final futex sequence is the smoking gun:
        t1 WAIT op=9 addr=0x7ffffff738 (WAIT_BITSET, `__syscall_cancel_arch_end`)
        t2 WAKE addr=0x7ffffff738 (pthread_cond_signal)     ← wakes t1 once
        t1 WAIT op=0 addr=0x7ffffff6b0 pc=`syscall`+0x28    ← plain FUTEX_WAIT,
            infinite timeout, raw syscall() — NEVER WOKEN (file ends; t3 only
            unrelated WAIT on 0x5001abf838).
    - The xcb internal spinlock (0x50059601f8, __lll_lock_wait/wake) handshake
      between t1 and t2 works perfectly — futex machinery itself is fine.
    - CONCLUSION: after MapWindow, t1 (Qt GUI thread) parks on a stack-local
      futex (`0x7ffffff6b0`) waiting for an X reply/event that t2 has already
      consumed into ITS xcb read queue, but t1 is never woken to go fetch it.
      This is a guest Qt threading/wakeup break under the emulator's thread
      scheduler, NOT a host fd relay data race (both threads CAN share fd11; the
      relay is byte-correct). 
    NEXT-STEP FORK: (i) instrument the guest side: find who is SUPPOSED to
    write 1→0x7ffffff6b0 / FUTEX_WAKE it (Qt's QXcbEventReader → main-thread
    handshake, likely a QSocketNotifier on a pipe or a semaphore posted after
    reading) and why that store never happens under BIFROST_NO_JIT=1 interp
    scheduling; or (ii) check the emulator's thread scheduling — the reader
    thread t2 may never yield back to t1 (both are host pthreads; the futex wait
    for t1 on 0x7ffffff6b0 blocks its host thread, and t2 is in a tight
    ppoll(POLLIN)=immediately-ready loop that never lets t1's waker run). The
    t2 POLLIN-always-ready loop (725 iterations, all ret=1) is suspicious — the
    host poll on fd11 returns instantly because t2's own recvmsg keeps the
    socket empty AND readable-ready, starving host time-slice for t1's waker.

- **2026-08-05 (6): D-BUS/A11Y FOUND & FIXED, BUT IS NOT THE RENDER BLOCKER;
  XCB-PATH HEAP CORRUPTION IS. Full D-Bus relay instrumentation added
  (`[DBUSSEND]` sendto@206, `[DBUSMSG]` sendmsg@211, `[DBUSRECV]` recvmsg@212,
  `[DBUSREAD]/[DBUSWRITE]` read/write@63/64, `[DBUSWRV]/[DBUSREADV]`
  writev/readv@66/65, all fd==7, gated BIFROST_XTRACE). Findings:
  - **ROOT CAUSE of the old `0x7ffffff6b0` never-woken futex (UPDATE 5): the
    AT-SPI accessibility bridge inside `QWidget::show()` blocked forever on a
    D-Bus connection-ready `QSemaphore::acquire` (libQt5Core+0xd0600).
    The session-bus AUTH EXTERNAL was REJECTED: the guest reported
    `getuid()=0` (deliberate guest-as-root design) but the host bus socket's
    SO_PEERCRED is uid 1000. Before the fix the daemon answered `REJECTED`
    (ret=19 `52454a45`).**
  - **FIX (src/syscalls/misc_id.cpp):** getuid/geteuid/getgid/getegid
    (cases 174-177) now return host `::getuid()`/etc. instead of 0. D-Bus
    handshake now completes fully (verified wire sequence): connect →
    `AUTH EXTERNAL 31303030` → `OK <guid>` (37B) → `NEGOTIATE_UNIX_FD` →
    `AGREE` (15B) → `BEGIN` → HELLO (128B) → HELLO reply (262B, `DBUSRECV`).
    After this the old `0x7ffffff6b0` fatal futex NEVER appears again.
  - **QT_ACCESSIBILITY=0 is NOT honored by this Qt 5.15.8 build** — with it
    propagated (added `QT_ACCESSIBILITY`/`QT_LINUX_ACCESSIBILITY_ALWAYS_ON` to
    the build_default_guest_env propagate list) + XTRACE, the FULL 8-line D-Bus
    exchange still happens. (Earlier "a11y bypass works" was a trace-gating
    artifact: XTRACE was off.) Keep the uid fix, it is the real repair.
  - **D-Bus is NOT the render blocker:** with
    `DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent_dbus` (env overrides Qt's
    X `_DBUS_SESSION_BUS_ADDRESS` property; connect fails fast res=-1, ZERO
    D-Bus protocol), the app STILL hangs/crashes in the xcb path. So a11y was
    one gate, not the cause of the black window.
  - **OFFSCREEN PROVES THE WHOLE Qt RASTER PIPELINE WORKS:**
    `QT_QPA_PLATFORM=offscreen` → app **exits 0** and writes a valid
    **520×320 RGB PNG** (`rootfs/tmp/qtgui_render.png`, 14945 B) with real
    content: 2511 unique colors, 95.6% black bg + colored regions. So the
    event loop, `QTimer::singleShot`(400ms), `QWidget::grab`, `QPainter`, and
    `QPixmap::save` all work under emulation. The bug is X-window-specific.
  - **CURRENT BLOCKER (superseded by UPDATE 7): xcb real-window path.** Guest
    maps the window (XREQ opcode 8), sets input focus (45), then either (a) hangs
    with t1 in `QSemaphore::acquire` (libQt5Core+0xd0600, stack futex 0x7ffffff600),
    or (b) **crashes `malloc(): unaligned tcache chunk detected`** (signal 6).
    The emulator's crash dump backtrace puts the corrupting allocation deep in
    **libQt5XcbQpa** (`+0x155aac`, `+0x159764`, `+0x1750a8`, `+0xd140`,
    `+0xeb04`, `+0x1728`) ← libQt5Gui(+0x13def0/+0x13f328) ←
    libQt5Widgets(+0x191c88) ← main. It was **racy**: clean 70s run
    (longtest.log) hung w/o crash; a FUTEX_BT run crashed at the same stage.
    recvmsg@212 writeback is bounded (writes guest iov_len per iov — safe);
    writev@66 relay byte-correct. Likely an emulator X-event/reply delivery
    overrun or thread-scheduling race in the xcb path, not a Qt logic bug.
    **REVISED 2026-08-06 (see UPDATE 7): the heap corruption has NOT reproduced
    since and is no longer treated as the blocker.** Clean runs (e.g. cur.log)
    now consistently hang at MapWindow with NO malloc crash. The deterministic
    gate is (a): the D-Bus/a11y `sessionBus()` QSemaphore deadlock — a guest Qt
    5.15 bug, emulator exonerated by the standalone libdbus probe.
  - Instrumentation added this session: minimal `BIFROST_FUTEX_BT` (addr+pc+lr
    only — the full find_object walk in the old version was unstable/crashed),
    `[XBT]` opcode-8 MapWindow backtrace (fp walk + stack word scan;
    `mem_.read`-based, try/catch), `[DBUSWRV]/[DBUSREADV]`, `[DBUSSEND]`,
    `[DBUSMSG]`, `[DBUSRECV]`.
  - FUTURE STATE of the uid change: reverses the deliberate guest-as-root
    design comment in misc_id.cpp ("so setuid programs work"). Regression
    suite has NOT been run since; may need gating or reverting if it breaks
    setuid-based tests, but it is REQUIRED for D-Bus.
  - **RESOLVED 2026-08-06 (regression gate): `./scripts/run_tests.sh --test-all`
    now passes 193/193 in JIT mode.** Two `whoami` tests were updated to expect
    `^$(whoami)$` (host username) instead of `^root$` — matching the intended
    consequence of the uid→host-uid change (guest now reports the real user,
    needed for D-Bus AUTH EXTERNAL). No other test regressions from the uid
    change, `bt_sym`, or the D-Bus/X traces. Also fixed an **ungated `[XREQ]`
    writev@66 trace (fs.cpp) that printed on every write and interleaved into
    guest stdout**, breaking `toybox_ls`/`rw_toybox_uname` pattern matches; it is
    now `getenv("BIFROST_XTRACE")`-gated like the other X traces.

- **2026-08-06 (7): D-BUS "sessionBus HANG" ROOT-CAUSED — EMULATOR EXONERATED,
  GUEST QT 5.15 SUSPENDED-DELIVERY DEADLOCK. UPDATE 6 fixed the AUTH/uid gate and
  the wire now completes through the 262B HELLO reply, but in some runs t1 still
  hangs in `QSemaphore::acquire` (stack futex 0x7ffffff640). This session proved
  where and why:
  - **Guest-symbolized backtrace (new `bt_sym()` tooling, dbg16)**: t1 futex WAIT
    `0x7ffffff640`, lr=`_ZN10QSemaphore7acquireEi+0xbc`; chain =
    `QWidget::setVisible → QWidgetPrivate::setVisible → QWidgetPrivate::show_helper
    → QAccessible::updateAccessibility → QAccessible::isActive →
    QXcbIntegration::accessibility → QDBusConnection::sessionBus →
    qDBusBindToApplication → QDBusVirtualObject::qt_metacall →
    QObject::setProperty → QSemaphore::acquire`. **No `exec()`/event-loop frame**
    (tops out at `__libc_start_main`) — t1 blocks during `show()` BEFORE the Qt
    event loop runs.
  - **Send-side (dbg14)**: only HELLO is ever transmitted (AUTH EXTERNAL →
    NEGOTIATE_UNIX_FD → BEGIN → HELLO 128B); the a11y sync call was never sent.
    The 262B hello reply arrives byte-perfect; then t3 (Qt DBus manager thread)
    wakes its eventfd 5×/drains to 0, one `to=0` ppoll, then an infinite `-1`
    ppoll on gfd6(gfd18)+gfd7(gfd19). **No futex WAKE ever targets 0x7ffffff640.**
  - **Mechanism (Qt 5.15 source `qdbusconnection.cpp`/`_p.h`, fetched from
    code.qt.io h=5.15)**: `QDBusConnection::sessionBus()` from the main thread
    (since `qApp->thread()==QThread::currentThread()`) sets `suspendedDelivery`
    + `setDispatchEnabled(false)` and connects via `BlockingQueuedConnection` to
    the manager thread; re-enabling is deferred to the main-thread event loop via
    `QTimer::singleShot(0)`/`invokeMethod(QueuedConnection)`. With t1 parked in
    `show` ahead of `exec`, the enable never fires → the reply is read but never
    dispatched → the QSemaphore is never released. Guest Qt bug, not emulator.
  - **Emulator exonerated for the data path**: standalone guest libdbus probe
    (`dbus_bus_get` → `dbus_bus_register`(HELLO) →
    `send_with_reply_and_block`(ListNames)) returns **SUCCESS exit 0** under the
    emulator (reply type=2, sig=`as`). Cross-compiled `/tmp/opencode/dbus_probe.c`
    with `tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc`, staged at
    `rootfs/usr/local/bin/dbus_probe.elf`.
  - **`QT_ACCESSIBILITY=0`/`OFF`, `QT_LINUX_ACCESSIBILITY_ALWAYS_ON=0`,
    `QT_ACCESSIBILITY_AUTO=0` all still hang (exit 137)** — 5.15.8 ignores them;
    the a11y bus call happens regardless.
  - **`DBUS_VERBOSE*` dead end**: rootfs libdbus is a musl build with verbose
    compiled out.
  - **Verdict / next action**: keep the uid fix (UPDATE 6, REQUIRED). The residual
    QSemaphore-acquire hang is Qt 5.15 suspended-delivery (bus opened synchronously
    from the a11y path before the loop). Options to record: (a) guest-side app/demo
    change to defer/async the a11y session-bus connect; (b) treat as a known Qt
    5.15 limitation; (c) only if unavoidable an emulator shim — but the standalone
    probe proves the emulator data path, so (c) is not justified.
  - New tooling this session: `bt_sym()` (nearest-symbol scan via
    `find_object_by_addr` + dynsym walk) wired into `[BT]`/`[THR]`/`[THR2]` in
    threads.cpp, plus a public `dyn_linker()` accessor on `Emulator`
    (`src/core/emulator.h`). All uncommitted.

## Status: CONFIRMED WORKING (was previously misdiagnosed as broken)
These have been verified and are NOT the bug:
- **X handshake works end-to-end.** The 6740-byte setup success reply from
  Xwayland is delivered and consumed via **`recvfrom` (syscall 207)** — never
  logged before, so it appeared "missing". The guest reads
  `01000b0000009306` (8B) + `7539bd00…` (6732B), byte-identical to what a real
  X client receives (verified by direct probes to both `/tmp/.X11-unix/X0` and
  the abstract `@/tmp/.X11-unix/X0` socket).
- `rid_base=0x01400000`, `rid_mask=0x1f` — matches guest window IDs
  (0x1400003/5/7); **no ID mismatch**. Root = 0x379; real screen 6000x1440.
- **writev relay is byte-correct.** `[XREQ]` reads the guest iovec via
  `mem_.read` and the relay `node->write`s the same bytes. Iovec base is a high
  runtime address (e.g. `0x5837e45394`), varies per run, but the bytes are read
  faithfully from guest memory.

## Root cause (isolated)
**Xwayland rejects every CreateWindow with `BadValue` (error 0x02, bad=0).**
An X11 window cannot have width=0, and the guest sends width=0 + height=<garbage>.
So the window is never created, and every subsequent op (MapWindow,
ChangeProperty, ChangeWindowAttributes) fails with `BadWindow` → no visible window.

Error chain observed in the capture (xseq23.bin):
```
R101: 0x02 BadValue, major=CreateWindow, bad=0   (0x1400003)
R103: 0x03 BadWindow  ChangeWindowAttributes 0x1400003
R144: 0x02 BadValue, major=CreateWindow, bad=0   (0x1400005)
R148: 0x03 BadWindow  ChangeProperty 0x1400007
... everything afterward fails with BadWindow
```
Proof: the wire/handshake/relay are correct (server processes all requests
byte-for-byte). The defect is wholly guest-side geometry computation
(width=0 / garbage height).

### Evidence details
- CreateWindow header (opcode/depth/len/wid/parent/x/y/class/visual/mask) is
  **byte-identical across runs**. Only `height` (bytes 18-19) varies:
  4895/4893, 2977/2975, 0/49207, 16837/16838, 14307, 50300, 41498, 14176, 9.
- Width is *always* exactly 0.
- Two main windows (0x1400003, 0x1400005) get heights differing by **exactly 2**
  in the same run (4895/4893, 2977/2975) — same source value read twice,
  deterministic within a run, varying across runs (ASLR-flavored → looks like
  uninitialized/mis-computed guest memory, not deterministic logic).
- 0x1400007 height is always 9.
- rec144 CreateWindow: request `len=15` words, but mask `0x2e19` needs 8 value
  words (=16 words) — guest built `len` from one mask and wrote a different mask.
- **No ConfigureWindow (op 12) is ever sent** — so Qt never sizes the window.

## 🔥 BREAKTHROUGH (new, 2026-08-05): guest args are CORRECT; serialization is broken

### New tooling added
- **`BIFROST_XCB_CW=1` interpreter trace** (src/interp/interpreter.cpp): resolves
  `xcb_create_window` via `resolve_symbol()`, dumps x0-x11 at entry.
- Requires `#include "frontend/dynamic_linker.h"` in interpreter.cpp (full type
  needed; emulator.h only forward-declares). Added at line 13.
- env-var-address variant first failed because libxcb base is ASLR-randomized
  per run (0x5309d0f000 / 0x51d9d25000 / 0x56f9ca9230 / ...). Use
  `resolve_symbol()` instead — never hand-compute the address.

### Finding: Qt passes CORRECT geometry; xcb lib corrupts it during serialization
Register dump at `xcb_create_window` entry (run26, BIFROST_NO_JIT=1):

| call | wid | x | y | **w** | **h** | class | visual | mask | vlist |
|------|-----|---|---|-------|-------|-------|--------|------|-------|
| 1 | 0x1400003 | 0 | 0 | **3** | **3** | 2 | 0x4 | ... | 0x7f...901270 |
| 2 | 0x1400005 | 0 | 0 | **520** | **320** | 11801? | 0x25 | 0x1138 | 0x7f...9208 |
| 3 | 0x1400007 | 0 | 0 | **1** | **1** | 3 | 0x4 | 0x1138 | 0x7f...9000 |

- **Main window 0x1400005 gets w=520, h=320** in registers — EXACTLY the offscreen
  render size. So Qt is blameless.
- The garbage seen in the wire (`w=0, h=garbage, class=11801, len=15 vs mask
  0x2e19`) is produced **inside $xcb_create_window itself** while it serializes
  the correct register args into the request buffer.
- NOTE: the register-to-arg mapping needs care. xcb AArch64 calling conv for
  `xcb_create_window(c, depth, wid, parent, x, y, width, height, border,
  _class, visual, mask, vlist)`: x0=c, x1=depth, x2=wid, x3=parent, x4=x,
  x5=y, x6=width, x7=height, x8=border(w), x9=class(w), x10=visual(w), x11=mask,
  x12=vlist. Dump showed w=520 in x6 & h=320 in x7 ✓ (call 2). class showed as
  32-bit garbage so border/class/visual/mask offsets in the print were
  read-only-approximations; the DISASM above maps the real stack layout.

### Disassembly (xcb_create_window, libxcb.so offset 0x12230)
- `and w6,w6,#0xffff` + `fmov d0,x6` + `mov v0.h[1],w7` + `ld1 {v0.h}[2]` +
  `mov v0.h[3],v1.h[0]` → packs width/height/border/class into **v0.8h**.
- `str d0,[sp,#56]` → stores packed v0 to outgoing struct.
- Builds value-list struct on stack, calls `xcb_create_window_value_list_sizeof`
  then `xcb_send_request`. Has `__stack_chk_fail` guard (0x12300).
- **This SIMD packing (fmov/mov v.h[n]/ld1/str d) is the prime suspect.** Qt's
  width/height come in via integer regs but are moved through SIMD v0. If the
  interpreter mishandles `mov v0.h[1], w7` / `ld1 {v0.h}[2], [x0]` /
  `mov v0.h[3], v1.h[0]` (vector element insert/load from h-registers), the
  packed height gets corrupted → explains garbage height in wire while regs
  x6=520/x7=320 stay correct.
- The `class=11801` garbage + `len=15 vs mask` mismatch support a vector-pack /
  stack-packing corruption, not a Qt-geometry bug.

## ROOT CAUSE FOUND & FIXED (2026-08-05): single-structure LD1 lane decode

### Debugging tools (still in tree, cross-built test files):
- `ctest_real/vecpack_test.elf` + `vecpack_isolate.c`: reproduce libxcb's
  `xcb_create_window` SIMD packing (`fmov d0,x6; mov v0.h[1],w7;
  ld1 {v0.h}[2]; mov v0.h[3],v1.h[0]; str d0`).
- `ctest_real/encgen.c` → /tmp/opencode/encgen.elf: emits every LD1
  size·index encoding; disassembled to derive exact bit layout.

### The bug (in `src/interp/interp_fp.cpp` single-structure LD1/ST1 path)
After proving regs hold w=520,h=320, the **guest libxcb preserved width/height
& border/class by packing them into v0.8h with an indexed `ld1`**. The
interpreter mis-decoded **single-structure LD1 lane/index**:
- The old code read `size_field=(d.raw>>13)&3` (bits[14:13]) as the element
  size and an ad-hoc index from bits[12:11]. For `ld1 {v0.h}[2]` (0x0D405020)
  that yielded esize=4, idx=0 → **wrote a 4-byte value into lane 0**, destroying
  the width already placed there and blanking height. Hence wire showed
  width=0, height=garbage, border in h0, class=11801.
- Both interp AND JIT were wrong identically because **single-structure LD1 is
  not native in the JIT — it falls back to CALL_INTERP** (ir_translate_fp.cpp
  :514), so the interpreter is the single source. One fix covers both.

### The fix (interp_fp.cpp, single-structure indexed decode)
Authoritative ARM decode (verified against all 7 encgen encodings + shared
decode): opcode=bits[15:13], **scale=opcode[2:1]**(bits[14:13]), **S=bit[12]**,
**size=bits[11:10]**, **Q=bit[30]**. Index per scale:
- scale 00 → **B**: esize=1, idx = Q:S:size      (bits[30],[12],[11:10])
- scale 01 → **H**: esize=2, idx = Q:S:size<1>   (bits[30],[12],[11])
- scale 10, size<0>==0 → **S**: esize=4, idx = Q:S (bits[30],[12])
- scale 10, size<0>==1 → **D**: esize=8, idx = Q   (bit[30])
- scale 11 → LD1R replicate (handled separately above).
Replaced the old `size_field`/`idx` block (lines ~179-213). `ld1 {v0.h}[2]`
now loads into lane 2 correctly.

### Post-fix confirmation (isolate, interp):
- `fmov d0,x6` = 0000000000000208 ✓ (520)
- `mov v0.h[1],w7` = 0000000001400000 ✓ (320)
- `ld1 {v0.h}[2]` = 0000020800000000 ✓ (**520 at byte4=h[2]**, was h[0])
- `mov v0.h[3],v1.h[0]` = 0001000000000000 ✓
All four packing instructions now correct. (The `ff ff` in the un-isolated
full test's h0 is a register-asm test artifact, NOT the emulator.)

## Next move
0. (From STATUS UPDATE 7 — the deterministic current gate) The heap-corruption
   hunt below is DROPPED: it has not reproduced and is no longer the blocker.
   Clean runs hang at MapWindow with the guest Qt 5.15 `sessionBus()` suspended-
   delivery deadlock (t1 `QSemaphore::acquire` from the a11y path in `show`,
   before `exec`). Emulator data path proven correct by the standalone libdbus
   probe (SUCCESS exit 0). Remaining ways forward, choose one:
   (a) guest-side: defer/async the a11y bus connect or avoid synchronous
       `QDBusConnection::sessionBus()` during `show()` (needs demo/Qt tweak);
   (b) accept as a known Qt 5.15 limitation and treat offscreen mode as the
       working path (it exits 0 + writes a valid PNG);
   (c) re-attack the (only) other real blocker if it reappears: the racy xcb
       heap corruption. Candidates preserved here for reference: X event/reply
       delivery overrun in the relay (recvmsg@212 writeback is bounded;
       re-check read@63/readv@65 writeback and the t2 event reader's 32-byte
       reads / phantom-POLLIN), or emulator thread-scheduling race. Reproduce
       WITHOUT `BIFROST_FUTEX_BT` (it perturbs); capture the corrupting
       allocation site via the crash dump's libQt5XcbQpa offsets.
1. Run the real GUI test and see the window now appear (the serialization is
   now correct, so CreateWindow should carry real w=520/h=320 and be accepted
   by Xwayland). Also verify the framesync/ggbuffer under DISPLAY=:0.
2. Rebuild clean; run `make USE_SDL2=1 USE_THUNK_GL=1`,
   `./scripts/run_tests.sh --unit --quick`, `--dynamic`,
   `ctest_real/test_sdl_gl_triangle.elf`, and cross-tests to ensure the LD1
   rewrite didn't regress multi-structure LD1/ST1 — and confirm the uid change
   (getuid/geteuid/getgid/getegid → host IDs) does not regress setuid guests.
3. Debug hygiene (DONE 2026-08-06): D-Bus investigation traces removed
   (`[DBUSSEND]/[DBUSMSG]/[DBUSRECV]/[DBUSGMEM]/[DBUSPOST]`,
   `[DBUSREAD]/[DBUSWRITE]/[DBUSWRV]/[DBUSREADV]`, `[EVENTREAD]/[EVENTWRITE]`).
   Remaining X/fd/futex/interp diagnostics consolidated behind
   `include/debug_flags.h` (single `BIFROST_TRACE=1` master switch; individual
   `BIFROST_XTRACE`/`BIFROST_FUTEX_BT`/`BIFROST_PPOLL_PEEK`/etc. still work).
   `--test-all` passes 193/193 with the cleanup. `bt_sym`/`[BT]`/`[XBT]` kept as
   opt-in diagnostics.
5. Re-verify: `make USE_SDL2=1 USE_THUNK_GL=1`,
   `./scripts/run_tests.sh --unit --quick`, `--dynamic`,
   `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf`.

## Relevant files
- `src/syscalls/fs.cpp`: writev case 66 relay + `[XREQ]`/`[XFULL]`/`[XWBYTES]`/
  `[XWRDELAY]` traces + `xseq_append` (XSEQLOG capture). Add guest-register
  tracing at xcb_create_window call site here.
- `src/syscalls/misc.cpp`: `[XPRE]`/`[XDELAY]`/`[XRECVF]` traces.
- `src/syscalls/misc_io.cpp`: `[XCONN]` + getpeername trace.
- `src/syscalls/misc_id.cpp`: getuid/geteuid/getgid/getegid (cases 174-177) —
  EDIT (uncommitted): now return HOST ids; required for D-Bus AUTH EXTERNAL.
- `src/syscalls/threads.cpp`: minimal `BIFROST_FUTEX_BT` (addr+pc+lr; old full
  find_object walk removed as unstable) + optional stack-word scan.
- `src/core/emulator.cpp`: `build_default_guest_env()` propagate list now includes
  `DBUS_SESSION_BUS_ADDRESS`, `XDG_RUNTIME_DIR`, `QT_ACCESSIBILITY`,
  `QT_LINUX_ACCESSIBILITY_ALWAYS_ON` (a11y vars are NOT honored by Qt 5.15.8).
- Evidence: `/tmp/opencode/uid2.log` (D-Bus OK+AGREE), `bt7/bt8.log` (full
  handshake incl. HELLO reply), `deaddbus2.log` (dead-bus: no D-Bus, still
  hangs → a11y not the blocker), `offscreen.log` + `rootfs/tmp/qtgui_render.png`
  (exit 0, valid 520×320 PNG — raster pipeline proven), `rawbt.log` (malloc
  crash dump, libQt5XcbQpa frames), `a11y0.log`/`a11ycheck.log` (QT_ACCESSIBILITY
  ineffective under XTRACE), `dbg14.log` (send side: only HELLO), `dbg16.log`
  (symbolized BT), `/tmp/opencode/dbus_probe.c` + `rootfs/usr/local/bin/dbus_probe.elf`
  (standalone libdbus probe — SUCCESS exit 0, emulator data path proven).
- `/tmp/opencode/*.py`: `xdec2.py`, `xparse.py`, `wdec.py` — sequence decoders.
- `/tmp/opencode/absx.c`, `splitx.c`, `scr2.c`, `fullprobe.c`: host probes.
- `rootfs/usr/local/bin/qtgui_test.elf`; offscreen ref `rootfs/tmp/qtgui_render.png`
  (520x320).
- `rootfs/var/cache/fontconfig/*.cache-8`: delete before every run.

## Bugs to NOT chase here (de-prioritized / separate)
- **JIT `munmap_chunk(): invalid pointer` crash** and JIT dlopen/rtld init failure
  (exit 134, `rtld_global_ro` zeroed / `dl_pagesize=0`). Separate from the X
  geometry defect; interpreter mode avoids it.
- The `git` "unity build" uncommitted temp instrumentation — strip before final
  commit.