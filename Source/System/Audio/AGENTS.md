# Host audio and API forwarding

- AudioThunk's shared state and callback coordination live in
  `AudioThunk/AudioThunkInternal.hpp`. Keep each API backend with its dispatch
  stage and maintain one shared callback reentrancy guard across translation units.
- Android startup must initialize AudioThunk and wire its guest runner. Managed
  AAudio callbacks run on the Bionic adapter's own pthread, preserving Bionic TLS;
  the host pump skips streams claimed by that guest thread. Bounded callback
  waits must allow group exit to complete.
- `AudioThunk/AudioThunk.cpp` owns export registration; the Android adapter builder
  reads its AAudio export list. Keep that tooling in sync when moving registration.
- Native thunk return polarity must preserve the SVC dispatch convention:
  a handled host return is not permission to run a second syscall dispatcher.
- Managed `isMusicActive` reflects real started host output streams. Keep audio
  service answers tied to supported state rather than unconditional success.
- Validate host callback stress/frame counters and managed Bionic AAudio thread
  tests in JIT/interpreter/Tier-2. Host tests alone do not validate Android TLS.
