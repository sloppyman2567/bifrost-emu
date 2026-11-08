// Bc7Encode.hpp — BC7 (BPTC) block encoder.
//
// Modes 0, 1, 2, 3, 6 and 7 (the ones without a separate alpha index set or rotation), all driven by one table of mode
// parameters and one endpoint fitter: principal-axis start, exhaustive nearest-index assignment and least-squares
// refinement of the endpoints, with the p-bits chosen per endpoint. Partition choices for the multi-subset modes are
// ranked by how well each subset fits a line and only the best few are fitted in full.
#pragma once
#include <cstdint>

namespace arm64emu::texcodec {

struct Bc7Options {
    // Partitions fitted in full per multi-subset mode, best first by the line-fit estimate (0 disables the mode).
    int partitions2 = 2;   // modes 1, 3 and 7
    int partitions3 = 0;   // modes 0 and 2
    // A block whose single-subset (mode 6) fit already has at most this mean squared error per channel is not tried
    // with other modes: nearly all smooth areas end here. 2.0 is about 45 dB, far below what an ASTC source's own
    // coding noise hides.
    float accept_error = 2.0f;
    int refine_rounds = 2;
};

// Encodes 16 RGBA8 texels (row-major 4x4, R first) to one 16-byte BC7 block.
void bc7_encode_block(const uint8_t* rgba, const Bc7Options& options, uint8_t* block);

}  // namespace arm64emu::texcodec
