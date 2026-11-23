// GuestIdentity.hpp — the kernel identity the guest sees: `uname` and the /proc files that repeat it.
//
// One definition, so `uname`, /proc/sys/kernel/{hostname,osrelease} and /proc/version cannot disagree. The Android DNS service
// (Scripts/Android/android_managed.py, DnsProxy::GUEST_HOSTNAME) answers the nodename itself; a test compares the two.
#pragma once

namespace arm64emu {

inline constexpr const char* GUEST_NODENAME = "bifrost-emu";
inline constexpr const char* GUEST_RELEASE = "6.5.0";  // glibc wants >= 3.2 for most things; modern enough for its fast paths
inline constexpr const char* GUEST_KERNEL_VERSION = "#1 SMP PREEMPT Dynamic bifrost-emu";

}  // namespace arm64emu
