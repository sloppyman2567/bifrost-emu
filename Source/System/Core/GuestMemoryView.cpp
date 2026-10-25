// GuestMemoryView.cpp — see GuestMemoryView.hpp.
#include "System/Core/GuestMemoryView.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/sysinfo.h>

namespace arm64emu {
namespace {

constexpr uint64_t kDefaultCap = 8ull << 30;

bool read_host_meminfo(HostMemoryInfo& out) {
    FILE* file = std::fopen("/proc/meminfo", "r");
    if (!file) return false;
    char line[256];
    bool have_available = false;
    while (std::fgets(line, sizeof(line), file)) {
        char name[64];
        unsigned long long kb = 0;
        if (std::sscanf(line, "%63[^:]: %llu", name, &kb) != 2) continue;
        const uint64_t bytes = kb * 1024ull;
        if (!std::strcmp(name, "MemTotal")) out.total = bytes;
        else if (!std::strcmp(name, "MemFree")) out.free = bytes;
        else if (!std::strcmp(name, "MemAvailable")) { out.available = bytes; have_available = true; }
        else if (!std::strcmp(name, "Buffers")) out.buffers = bytes;
        else if (!std::strcmp(name, "Cached")) out.cached = bytes;
        else if (!std::strcmp(name, "Shmem")) out.shmem = bytes;
    }
    std::fclose(file);
    if (!have_available) out.available = out.free + out.buffers + out.cached;
    return out.total != 0;
}

void read_host_sysinfo(HostMemoryInfo& out) {
    struct sysinfo si;
    std::memset(&si, 0, sizeof(si));
    if (::sysinfo(&si) != 0) return;
    const uint64_t unit = si.mem_unit ? si.mem_unit : 1;
    out.total = si.totalram * unit;
    out.free = si.freeram * unit;
    out.buffers = si.bufferram * unit;
    out.shmem = si.sharedram * unit;
    out.available = out.free + out.buffers;
}

} // namespace

GuestMemoryInfo apply_guest_memory_policy(const HostMemoryInfo& host, uint64_t cap) {
    GuestMemoryInfo g;
    // A host that reports nothing still gets a guest that can run.
    const uint64_t host_total = host.total ? host.total : kDefaultCap;
    g.total = cap ? std::min(host_total, cap) : host_total;
    g.available = std::min(host.available, g.total);
    g.free = std::min(host.free, g.available);
    g.shmem = std::min(host.shmem, g.total);
    g.buffers = std::min(host.buffers, g.total - g.free);
    g.cached = std::min(host.cached, g.total - g.free - g.buffers);
    return g;
}

uint64_t guest_memory_cap() {
    static const uint64_t cap = [] {
        const char* value = std::getenv("BIFROST_GUEST_MEMORY_MB");
        if (!value || !*value) return kDefaultCap;
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(value, &end, 10);
        if (end == value || *end != '\0') return kDefaultCap;
        return static_cast<uint64_t>(mb) << 20;  // 0 = no cap
    }();
    return cap;
}

GuestMemoryInfo guest_memory_info() {
    HostMemoryInfo host;
    if (!read_host_meminfo(host)) read_host_sysinfo(host);
    return apply_guest_memory_policy(host, guest_memory_cap());
}

} // namespace arm64emu
