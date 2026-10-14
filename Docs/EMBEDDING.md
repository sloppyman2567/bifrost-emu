# Embedding Bifrost

Bifrost can run inside another program through a C API, and exposes an
Android native-bridge adapter built on that API.

## C API (libbifrost)

bifrost-emu exposes a stable C API (`Source/System/API/Bifrost.h`) for embedding the
emulator in host programs. Build it with `make lib` (produces
`libbifrost.a`; `Source/Main.cpp` is excluded — link your own driver).
Link with the `LDFLAGS=` line recorded in that build's
`build/<config>/.build-config`. Media-enabled builds (`USE_MEDIA=1`) need
FFmpeg's libraries, and SDL/GL builds need theirs:

```c
#include "Bifrost.h"

bifrost_emu_t* emu = bifrost_create();
bifrost_load_elf(emu, "hello.elf", argc, argv);
int exit_code = bifrost_run(emu);   // JIT is on by default
bifrost_destroy(emu);
```

Key features:

- **JIT default-on.** `bifrost_run` enables the frostJIT automatically;
  call `bifrost_set_jit(emu, 0)` to force the interpreter, or
  `bifrost_set_jit_verify(emu, 1)` (before run) for JIT-vs-interpreter
  divergence checking.
- **Register/FP/flag/memory access** — `bifrost_get/set_reg`,
  `bifrost_get/set_fp_reg_*`, `bifrost_get/set_pstate`, flags,
  `bifrost_read/write_mem`, SP/PC getters and setters.
- **Step-loop debugging** — `bifrost_step`/`bifrost_step_n` plus real
  breakpoints: `bifrost_set_breakpoint(emu, addr)` makes the next
  `bifrost_step` stop *before* executing that instruction and return 1.
  (`bifrost_run` ignores breakpoints and runs to completion.)
- **Guest function calls** — `bifrost_call(emu, fn, iargs, n, fargs, n)`
  invokes any guest function with up to 8 integer + 8 FP arguments,
  saving/restoring all CPU state around the call and returning x0.
  `bifrost_call_f` returns the FP (d0) result instead — use it for
  functions returning `double`/`float`.
- **Symbol lookup** — `bifrost_lookup_symbol(emu, "name")` resolves a
  symbol across loaded dynamic objects and thunk-registered libraries.
- **Syscall hook** — `bifrost_set_svc_hook(emu, fn, userdata)` installs
  a callback invoked for every guest syscall before emulator dispatch;
  return 1 and set `*result` to handle the syscall yourself (or 0 to
  pass it through). Note: the internal thunk fast path (syscall
  `0x1000`) bypasses the hook.
- **Guest dlopen/dlsym/dlclose** — `bifrost_dlopen(emu, path, flags)`
  loads a guest shared object and returns its base address as an opaque
  handle (re-load bumps the refcount), `bifrost_dlsym(emu, handle, name)`
  resolves a guest symbol (0 on miss), `bifrost_dlclose(emu, handle)`
  unloads it (0 success / -1 error). Foundation for the native bridge
  adapter below.

The full API reference and semantics live in `Source/System/API/Bifrost.h`. A
host-side test (`Tests/Unit/test_capi.c`, built via `make test-capi`, wired
into `make check-all`) covers the API with 55 checks.

## Android native bridge adapter

bifrost exposes an Android native-bridge integration adapter. Its host
API tests do not demonstrate an actual ART runtime or a drop-in Android
translation environment. `Source/System/API/NativeBridge.h` is a clean-room ABI mirror of Android's
`NativeBridgeCallbacks`; `Source/System/API/NativeBridge.cpp` fills the table over
the C API:

```c
#include "NativeBridge.h"
bifrost_emu_t* emu = bifrost_create();
bifrost_load_elf(emu, "system_lib.elf", argc, argv);
bifrost_nb_init(emu);                     // fills NativeBridgeItf
void* lib = NativeBridgeItf.loadLibrary("game_native.so", 0);
// ART: NativeBridgeGetTrampoline(lib, "Java_com_example_Native_foo", "JID", 3)
long (*foo)(void*, void*, long, double) =
    (long(*)(void*, void*, long, double))
    NativeBridgeItf.getTrampoline(lib, "Java_com_example_Native_foo", "JID", 3);
long r = foo(NULL, NULL, 42, 2.5);        // borrows the CPU, calls into guest
bifrost_nb_shutdown();
```

The thin adapter scope: `loadLibrary`/`isSupported`/`getError`/
`getSignalHandler` + borrow-CPU trampolines for scalar shorty
signatures (JNIEnv/jobject prefix, x/d-reg split per AAPCS, libffi
closures, `bifrost_call`/`bifrost_call_f` drive). No namespaces, no
CriticalNative, no borrowed method pointers — `version = 4` claims the
pre-Q ABI so ART uses the legacy `getTrampoline` path. Host test
`Tests/Unit/test_nb.c` (61 checks, `make test-nb`, wired into `check-all`).

This adapter is a separate integration interface, not the Asgard Android
runtime described in [the Android runtime guide](Android/ANDROID_RUNTIME.md).
