// SocketPolicy.hpp — the offline network policy for guest sockets.
//
// With the policy on (Config::android_offline, BIFROST_ANDROID_OFFLINE=1) a guest cannot send
// IP traffic anywhere but the loopback interface: connect/sendto/sendmsg/sendmmsg to any other
// IPv4/IPv6 address fail with ENETUNREACH, as on a device with no route. Unix, netlink and other
// non-IP families, binding and listening are unaffected; inbound traffic to a socket the guest
// already listens on a wildcard address is not filtered.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

namespace arm64emu {

inline std::atomic<bool>& network_offline() {
    static std::atomic<bool> offline{false};
    return offline;
}

// True when the policy forbids sending to the sockaddr at `addr` (`len` bytes, from guest memory).
inline bool socket_egress_blocked(const void* addr, size_t len) {
    if (!addr || len < sizeof(sa_family_t) || !network_offline().load(std::memory_order_relaxed))
        return false;
    sa_family_t family;
    std::memcpy(&family, addr, sizeof family);
    // 127.0.0.0/8 and the unspecified address (which Linux routes to the local host) stay reachable.
    auto local_v4 = [](uint32_t host_order) { return (host_order >> 24) == 127 || host_order == 0; };
    if (family == AF_INET && len >= sizeof(sockaddr_in)) {
        sockaddr_in in;
        std::memcpy(&in, addr, sizeof in);
        return !local_v4(ntohl(in.sin_addr.s_addr));
    }
    if (family == AF_INET6 && len >= sizeof(sockaddr_in6)) {
        sockaddr_in6 in6;
        std::memcpy(&in6, addr, sizeof in6);
        if (IN6_IS_ADDR_LOOPBACK(&in6.sin6_addr) || IN6_IS_ADDR_UNSPECIFIED(&in6.sin6_addr)) return false;
        if (IN6_IS_ADDR_V4MAPPED(&in6.sin6_addr)) {
            const uint8_t* b = in6.sin6_addr.s6_addr;
            return !local_v4(uint32_t(b[12]) << 24 | uint32_t(b[13]) << 16 | uint32_t(b[14]) << 8 | b[15]);
        }
        return true;
    }
    return false;
}

}  // namespace arm64emu
