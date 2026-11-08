# Host audio and API forwarding

- AudioThunk's shared state and callback coordination live in
  `AudioThunk/AudioThunkInternal.hpp`. Keep each API backend with its dispatch
  stage and maintain one shared callback reentrancy guard across translation units.
- Android startup must initialize AudioThunk and wire its guest runner. Managed
  AAudio callbacks run on the Bionic adapter's own pthread, preserving Bionic TLS;
  the host pump skips streams claimed by that guest thread. Bounded callback
  waits must allow group exit to complete.
- With no host device (SDL could not open one, or a headless build) `Audio` drains unpaused streams at the device rate, like
  a silent device (`drain_without_device_locked_`), so writers neither hang nor run unpaced. A device takes over
  through the mixer; both credit frames with `consume_frames_locked_`. `audio_test` covers it on a host without sound.
- `AAudioStream_write` honours its timeout like AOSP: it waits for ring space with `pump_mu` released and returns the frames
  written; a stopped or paused stream returns what fit, because nothing drains it.
- A stream joining or leaving the mix fades over 3 ms (`StreamDeclick.hpp`, `make test-audio-declick`), and a data-callback
  AAudio stream restarted after a stop drops its old queued audio. Pause keeps it, and write-mode streams may prefill while stopped.
- `AudioThunk/AudioThunk.cpp` owns export registration; the Android adapter builder
  reads its AAudio export list. Keep that tooling in sync when moving registration.
  AAudio validation, defaults and state rules follow AOSP libaaudio
  `android-15.0.0_r1`, and its exports match that release's
  `src/libaaudio.map.txt`; strings returned to the guest live in guest memory.
  A buffer capacity request sizes the stream ring (`Audio::stream_open`
  `min_frames`) rather than failing: FMOD fails its output init on a refusal.
  `BIFROST_AUDIO_TRACE` (a `DebugFlags.hpp` switch) traces audio thunk calls.
- Native `AudioTrack` clients (Java AudioTrack, SoundPool, MediaPlayer) play through
  `Guest/Android/Adapters/audio_track.c`, the server side of AudioFlinger's
  control-block protocol (`createTrack` in `FrameworkAudioTracks`): one AAudio
  data-callback stream per track on the managed libaaudio adapter. Keep the
  cblk layout, obtain/release/flush/stop rules and futex wakes matched to
  AOSP `AudioTrackShared` `android-15.0.0_r1`; its `_Static_assert`s pin the
  offsets and `ManagedServiceBatch.audioTracks` checks a real client. `createTrack` refuses
  a nested request on its own thread: with Android's `libaaudio` in place of the adapter the
  stream open loops back into it (`ManagedServiceBatch` mode `no-audio-adapter` covers this).
- Managed OpenSL ES is the guest `Guest/Android/Adapters/opensles.c`, layered on
  the managed libaaudio adapter (its callback thread runs buffer-queue callbacks),
  not the host `AudioThunk/OpenSL.cpp` vtables. Keep its result codes and
  validation matched to AOSP wilhelm `android-15.0.0_r1`, and its exports to
  that release's `libOpenSLES.map.txt`.
- Native thunk return polarity must preserve the SVC dispatch convention:
  a handled host return is not permission to run a second syscall dispatcher.
- Managed `isMusicActive` reflects real started host output streams. Keep audio
  service answers tied to supported state rather than unconditional success.
- Validate host callback stress/frame counters and the managed Bionic AAudio
  thread and OpenSL ES player tests in JIT/interpreter/Tier-2. Host tests alone
  do not validate Android TLS.
