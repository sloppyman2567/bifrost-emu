#pragma once
#include "System/VFS/Nodes/Node.hpp"
namespace arm64emu::yggdrasil {
// Asgard's local broker shares one Binder domain among independent guest VMs.
int asgard_broker_main(const std::string& socket_path);
int report_broker_process_state(const std::string& socket_path, int32_t state);
std::unique_ptr<Node> open_broker_binder(const std::string& socket_path,
        const std::string& device, int flags, uint32_t uid, int pid, int* error);
}
