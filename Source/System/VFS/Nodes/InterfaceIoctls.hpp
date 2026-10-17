// InterfaceIoctls.hpp — network interface queries on a host socket.
//
// Bionic's getifaddrs (SIOCGIFFLAGS for apps, which may not send
// RTM_GETLINK), if_nametoindex (SIOCGIFINDEX) and if_indextoname
// (SIOCGIFNAME) query interfaces through struct ifreq ioctls on an
// AF_INET socket. Without them libcore's NetworkInterface drops every
// interface (index 0) and getNetworkInterfaces() returns null. The guest
// already sees the host's interfaces through netlink, so these answer from
// the host socket for the same interfaces.
//
// Only "get" requests whose argument is one self-contained struct ifreq are
// marshalled; struct ifreq is 40 bytes with the same layout on AArch64 and
// x86-64 Linux (LP64). Requests carrying a pointer (SIOCGIFCONF, SIOCETHTOOL,
// SIOCDEVPRIVATE) and every "set" request stay unhandled (-ENOTTY).
#pragma once
#include "System/Memory/Memory.hpp"
#include "System/VFS/Nodes/Node.hpp"
#include <cstdint>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
namespace arm64emu::yggdrasil {
namespace ioctl_num {
    // Linux UAPI include/uapi/linux/sockios.h
    constexpr uint32_t REQ_SIOCGIFNAME    = 0x8910;  // ifindex -> name
    constexpr uint32_t REQ_SIOCGIFFLAGS   = 0x8913;
    constexpr uint32_t REQ_SIOCGIFADDR    = 0x8915;
    constexpr uint32_t REQ_SIOCGIFDSTADDR = 0x8917;
    constexpr uint32_t REQ_SIOCGIFBRDADDR = 0x8919;
    constexpr uint32_t REQ_SIOCGIFNETMASK = 0x891B;
    constexpr uint32_t REQ_SIOCGIFMTU     = 0x8921;
    constexpr uint32_t REQ_SIOCGIFHWADDR  = 0x8927;
    constexpr uint32_t REQ_SIOCGIFINDEX   = 0x8933;  // name -> ifindex
    constexpr uint32_t REQ_SIOCGIFTXQLEN  = 0x8942;
}  // namespace ioctl_num

static_assert(sizeof(struct ifreq) == 40, "guest struct ifreq is 40 bytes (LP64)");

// Returns the host ioctl's result (0 or -errno), or Node::IOCTL_NOT_HANDLED.
inline int dispatch_interface_ioctl(int host_fd, uint32_t request, uint64_t argp, Memory& mem) {
    switch (request) {
        case ioctl_num::REQ_SIOCGIFNAME:
        case ioctl_num::REQ_SIOCGIFFLAGS:
        case ioctl_num::REQ_SIOCGIFADDR:
        case ioctl_num::REQ_SIOCGIFDSTADDR:
        case ioctl_num::REQ_SIOCGIFBRDADDR:
        case ioctl_num::REQ_SIOCGIFNETMASK:
        case ioctl_num::REQ_SIOCGIFMTU:
        case ioctl_num::REQ_SIOCGIFHWADDR:
        case ioctl_num::REQ_SIOCGIFINDEX:
        case ioctl_num::REQ_SIOCGIFTXQLEN:
            break;
        default:
            return Node::IOCTL_NOT_HANDLED;
    }
    if (argp == 0) return -EFAULT;
    struct ifreq ifr;
    mem.read(argp, &ifr, sizeof(ifr));
    if (::ioctl(host_fd, request, &ifr) < 0) return -errno;
    mem.write(argp, &ifr, sizeof(ifr));
    return 0;
}
}  // namespace arm64emu::yggdrasil
