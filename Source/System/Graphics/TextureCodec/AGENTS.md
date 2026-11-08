# Texture codec

Pure CPU codecs that let a guest use texture formats the host GPU lacks. Today: ASTC LDR (2D) to BC7. No Vulkan or
guest-memory types here; the Vulkan thunk calls these.

## Ownership

- `AstcDecode.*`: one ASTC LDR block to RGBA8, from the Khronos Data Format Specification's ASTC chapter.
- `Bc7Encode.*`: RGBA8 4x4 block to BC7; `Bc7Tables.hpp` holds the partition and anchor tables.
- `AstcToBc7.*`: a whole image region, decoded and encoded in strips across threads.

## Local Contracts

- The decoder follows the specification, not another decoder's quirks: sRGB mode expands endpoints with `0x80`,
  results are the top 8 bits of the UNORM16 interpolation, HDR endpoint modes, HDR void extents and every illegal
  encoding give opaque magenta and a `false` return. It is LDR and 2D only; 3D and HDR formats are not offered.
- `Bc7Tables.hpp` is generated from the BPTC chapter's tables and not edited by hand. The specification's bold mark
  for two-subset partition 35 disagrees with its anchor table (pixel 2 against 8); the anchor table is right: a real
  GPU (RADV) decoded blocks of every mode and all 64 partitions, partition 35 included, exactly as an independent
  decoder does.
- The encoder produces modes 0, 1, 2, 3, 6 and 7 from one mode table and one fitter. An opaque block never gains
  transparency (mode 6 forces its p-bits). A flat colour is within one level per channel, not exact: a p-bit is shared
  by all channels of an endpoint. `Bc7Options::accept_error` trades speed for quality; the defaults were chosen from a
  sweep on game screenshots (about 4.5 to 5.5 Mpix/s per thread at 46 to 52 dB against the decoded source).
- `transcode_astc_to_bc7` works in strips of lcm(4, block height) rows so temporary memory is a few hundred kilobytes
  per thread; it repeats edge texels into partial BC7 blocks. All functions are reentrant (static tables only).

## Work Guidance

- Keep the encoder deterministic: output must not depend on thread count or optimisation level (the host test checks
  both).

## Verification

- `make test-astc-bc7-transcode`: hand-built ASTC blocks, trit/quint and unquantization properties, a fuzz, every BC7
  mode packed and decoded by an independent decoder, and the strip transcoder over all 14 footprints. It is not
  compared with ARM's reference decoder or a GPU's ASTC path; multi-partition and dual-plane blocks are checked only
  for self-consistency.

## Child DOX Index

None.
