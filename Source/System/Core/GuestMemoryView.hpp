// GuestMemoryView.hpp — the RAM figures a guest is shown (/proc/meminfo and sysinfo).
//
// They used to be a fixed 16 GiB total with 12 GiB available, whatever the host had, so a guest on a
// smaller machine never saw pressure: Unity and Chromium sized their caches for 16 GiB and Android's
// ActivityManager.MemoryInfo (computed from /proc/meminfo) could never report low memory. Now the
// figures are the host's real ones, with the total capped like a phone's RAM, so engines pick
// phone-sized budgets, and free/available follow the host's real availability.
#pragma once
#include <cstdint>

namespace arm64emu {

struct GuestMemoryInfo {
    uint64_t total = 0, free = 0, available = 0, buffers = 0, cached = 0, shmem = 0;  // bytes
};

// The host's figures before the guest policy; MemAvailable is estimated when the host has none.
struct HostMemoryInfo {
    uint64_t total = 0, free = 0, available = 0, buffers = 0, cached = 0, shmem = 0;  // bytes
};

// total = min(host total, cap), available = min(host available, total), free <= available,
// buffers + cached <= total - free. `cap` of 0 means no cap. Never returns a zero total.
GuestMemoryInfo apply_guest_memory_policy(const HostMemoryInfo& host, uint64_t cap);

// Cap in bytes: 8 GiB, or BIFROST_GUEST_MEMORY_MB megabytes (0 = no cap); read once.
uint64_t guest_memory_cap();

// Reads the host now (/proc/meminfo, or sysinfo when that is unreadable) and applies the policy.
GuestMemoryInfo guest_memory_info();

} // namespace arm64emu
