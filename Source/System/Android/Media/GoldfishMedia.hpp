#pragma once
#include "System/VFS/Nodes/Node.hpp"
namespace arm64emu::yggdrasil {
// The media-only Goldfish address-space ABI used by the pinned Codec2 modules.
// No host graphics/address-space services are exposed through this device.
std::unique_ptr<Node> open_goldfish_media(int flags, int* error);
}
