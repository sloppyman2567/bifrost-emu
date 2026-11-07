#pragma once
#include "System/VFS/Nodes/Node.hpp"
namespace arm64emu::yggdrasil {
// Asgard's local broker shares one Binder domain among independent guest VMs.
// idle_seconds > 0: the broker ends after that long with no app attached; 0 keeps it until stopped.
int asgard_broker_main(const std::string& socket_path, unsigned idle_seconds = 0);
int report_broker_process_state(const std::string& socket_path, int32_t state);
std::unique_ptr<Node> open_broker_binder(const std::string& socket_path,
        const std::string& device, int flags, uint32_t uid, int pid, int* error);
}
