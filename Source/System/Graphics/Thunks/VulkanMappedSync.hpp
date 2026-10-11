#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arm64emu::vk_mapped {

// The baseline records the last synchronized value of each byte. A fence
// for one allocation must not erase CPU writes prepared in another one.
// Conversely, a submit must not overwrite GPU readback with an unchanged
// guest copy. Only changed guest bytes are published; readback merges into
// bytes the guest has not changed. Concurrent access to the same bytes still
// requires the application's normal Vulkan synchronization.
inline void mapped_push(uint8_t* host, const uint8_t* guest,
                        uint8_t* baseline, size_t size) {
    constexpr size_t chunk = 4096;
    for (size_t off = 0; off < size; off += chunk) {
        const size_t n = std::min(chunk, size - off);
        if (std::memcmp(guest + off, baseline + off, n) == 0) continue;
        for (size_t i = off; i < off + n; ++i) {
            if (guest[i] == baseline[i]) continue;
            host[i] = guest[i];
            baseline[i] = guest[i];
        }
    }
}

inline void mapped_pull(const uint8_t* host, uint8_t* guest,
                        uint8_t* baseline, size_t size) {
    constexpr size_t chunk = 4096;
    for (size_t off = 0; off < size; off += chunk) {
        const size_t n = std::min(chunk, size - off);
        if (std::memcmp(guest + off, baseline + off, n) == 0) {
            std::memcpy(guest + off, host + off, n);
        } else {
            for (size_t i = off; i < off + n; ++i) {
                if (guest[i] == baseline[i]) guest[i] = host[i];
            }
        }
        std::memcpy(baseline + off, host + off, n);
    }
}

} // namespace arm64emu::vk_mapped
