// CodeInvalidation.hpp — why guest code translations are being dropped.
//
// The reason selects the invalidation policy and its statistics bucket.
// Cache maintenance and mprotect leave guest bytes in place, so a
// translation whose fetched instruction words are unchanged (and still
// executable) remains valid. Every other reason replaces or removes the
// mapping and retires all overlapping translations.
#pragma once
#include <cstdint>
namespace arm64emu {
enum class CodeInvalidation : uint8_t {
    CacheMaintenance,  // DC/IC by VA: guest publishing written code
    Protect,           // mprotect / pkey_mprotect
    Unmap,             // munmap, brk shrink, failed mmap rollback
    Map,               // MAP_FIXED replacement
    Remap,             // mremap move/grow, MREMAP_DONTUNMAP
    Discard,           // MADV_DONTNEED / MADV_REMOVE
    Count
};
// Only these reasons may retain a translation whose bytes still match.
constexpr bool invalidation_keeps_unchanged(CodeInvalidation reason) {
    return reason == CodeInvalidation::CacheMaintenance || reason == CodeInvalidation::Protect;
}
constexpr const char* invalidation_name(CodeInvalidation reason) {
    switch (reason) {
        case CodeInvalidation::CacheMaintenance: return "cache";
        case CodeInvalidation::Protect: return "mprotect";
        case CodeInvalidation::Unmap: return "unmap";
        case CodeInvalidation::Map: return "map";
        case CodeInvalidation::Remap: return "remap";
        case CodeInvalidation::Discard: return "discard";
        default: return "other";
    }
}
} // namespace arm64emu
