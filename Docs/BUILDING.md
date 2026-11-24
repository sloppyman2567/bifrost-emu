# Build, configure and contribute

For people who want to build Bifrost themselves, change its settings, report a bug or send a change. To just install and run
programs, [Install and update](INSTALL.md) is enough. To use Bifrost from your own program, see [Embedding](EMBEDDING.md).

## Building

| Requirement | Needed for |
|---|---|
| g++ 9+ (C++17), make, Linux x86_64 | Everything (required) |
| x86-64 CPU with SSE4.1 and FMA3 | The JIT (`--no-jit` runs on any x86-64 CPU, much more slowly) |
| SDL2 development headers (`./Tools/Fetch/fetch-sdl2-headers.sh`) | Framebuffer, guest SDL2 and GL thunks |
| SDL3 ≥ 3.2 headers and runtime | Android host windows |
| FFmpeg libavcodec/libavutil/libswscale development files | Android host media decoding (`USE_MEDIA=1`, default) |
| AArch64 cross toolchain (`./Tools/Fetch/fetch-musl-toolchain.sh`) | Building guest test programs |

`make deps` installs all of these except the cross toolchain, using apt, dnf, zypper or pacman
([what has been checked](SUPPORT.md)).

| Command | Builds |
|---|---|
| `make bootstrap` | Host packages, the emulator and Asgard, installed under `~/.local` or `PREFIX` |
| `make` | Default; auto-detects SDL2 and enables host GL/EGL thunking |
| `make USE_SDL2=1 USE_SDL3=1 USE_THUNK_GL=1` | Android SDL3 windows plus guest SDL2/GL thunks |
| `make USE_SDL2=0 USE_SDL3=0` | Headless build |
| `make USE_MEDIA=0` | No host media decoders; Codec2 advertises no host codecs |
| `make debug` | Sanitizer build at `build/debug/bifrost-emu-dbg` |
| `make lib` | `libbifrost.a` for [embedding](EMBEDDING.md) |

Each configuration gets its own `build/release-sdl*-gl*` directory; the default
release binary is also copied to `./bifrost-emu`. `make clean` removes them all.
Guest test programs build with `make setup-tests`, or one at a time with
`make cross SRC=Tests/Apps/my_test.c OUT=Tests/Apps/my_test.elf`.

## Testing

```bash
make check            # standard suite (benchmarks included)
make check-quick      # without benchmarks
make check-nojit      # quick suite under the reference interpreter (a test mode, not a way to run programs)
make verify           # for JIT changes: replay JIT blocks against the reference interpreter
make ci               # local correctness gate (generated files, APIs, both engines)
./Scripts/Testing/run_tests.sh --strict --filter "sig|brk|pipe"   # by name
```

Missing fixtures fail strict mode, and environment skips (exit 77) stay visible. Results, history and fixture licensing are in the
[test guide](TESTS.md) and the [third-party notes](THIRD_PARTY.md).

## Configuration

Settings come from a TOML file, environment variables and flags:

```bash
./bifrost-emu --config bifrost.toml my_app.elf   # use a config file
./bifrost-emu --print-config                     # show effective settings

BIFROST_ROOT=/path/to/rootfs    # guest root filesystem
BIFROST_TIER2=1                 # experimental Tier-2 region compilation
BIFROST_JIT_VERIFY=1            # JIT vs interpreter divergence check
BIFROST_SYSCALL_TRACE=1         # trace syscalls
BIFROST_DYNLINK_TRACE=1         # trace the dynamic linker
BIFROST_THUNK_TRACE=1           # trace host GL/audio/display calls
```

Useful switches:

- `-d` traces every instruction.
- `--no-jit` forces the reference interpreter. It is slow: use it for debugging, or on a CPU without SSE4.1 and FMA3.
- `--window-stats` shows FPS and load statistics (see [window stats](window-stats.md)).
- `--jit-disk-cache` is experimental. It keeps translations in `~/.cache/bifrost/jit` so later launches start faster
  ([JIT guide](JIT.md#persistent-translation-cache)).

[`bifrost.toml.sample`](../bifrost.toml.sample) lists every option, and `Source/System/Core/DebugFlags.hpp` lists the
diagnostic switches.

## Contributing

1. Fork the repository and follow the existing code style.
2. Build guest fixtures with `make setup-tests`, then run the relevant JIT and
   interpreter checks (`make check-all` for provisioned coverage).
3. For JIT changes, read the [JIT guide](JIT.md) and run `make verify`;
   report existing verifier failures separately from new ones.
4. Preserve upstream notices for reused code and include validation evidence
   with your pull request. Original contributions, including tests, use the
   Unlicense; reused code retains its upstream license and attribution.

Bug reports help most with the AArch64 binary (or a minimal reproducer), the
exact command line, and the tail of `./bifrost-emu -v -d ... 2>&1`.
