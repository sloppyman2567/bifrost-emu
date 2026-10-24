// ResourceLimits.cpp — the guest's getrlimit/setrlimit/prlimit64 state.
#include "System/Core/Emulator.hpp"
#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>
#include <climits>
namespace arm64emu {
namespace {
// The host's own limit for `resource`, or unlimited when it cannot be read. RLIM_INFINITY is the
// same all-ones value in the AArch64 guest ABI.
Emulator::Rlimit64 host_limit(int resource) {
    struct rlimit host{};
    if (::getrlimit(static_cast<__rlimit_resource_t>(resource), &host) != 0)
        return {RLIM_INFINITY, RLIM_INFINITY};
    return {host.rlim_cur, host.rlim_max};
}
}  // namespace

bool Emulator::get_rlimit(int resource, Rlimit64& out) {
    if (resource < 0 || resource >= GUEST_RLIM_NLIMITS) return false;
    if (!rlimits_seeded_.load(std::memory_order_acquire)) {
        // First use: take the host's limits. Racing threads store the same values.
        for (int r = 0; r < GUEST_RLIM_NLIMITS; ++r) {
            const Rlimit64 limit = host_limit(r);
            rlimit_cur_[r].store(limit.cur, std::memory_order_relaxed);
            rlimit_max_[r].store(limit.max, std::memory_order_relaxed);
        }
        rlimits_seeded_.store(true, std::memory_order_release);
    }
    out.cur = rlimit_cur_[resource].load(std::memory_order_relaxed);
    out.max = rlimit_max_[resource].load(std::memory_order_relaxed);
    return true;
}

int Emulator::set_rlimit(int resource, const Rlimit64& value) {
    Rlimit64 current;
    if (!get_rlimit(resource, current)) return -EINVAL;
    if (value.cur > value.max) return -EINVAL;
    // An unprivileged process may lower its hard limit but never raise it (setrlimit(2)).
    if (value.max > current.max && ::geteuid() != 0) return -EPERM;
    rlimit_cur_[resource].store(value.cur, std::memory_order_relaxed);
    rlimit_max_[resource].store(value.max, std::memory_order_relaxed);
    if (resource == RLIMIT_NOFILE)
        fds_.set_fd_limit(value.cur > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(value.cur));
    return 0;
}
}  // namespace arm64emu
