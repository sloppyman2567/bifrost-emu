#pragma once
#include <memory>
#include <string>
namespace arm64emu::yggdrasil {
struct BinderDomain;
// Owned by the realm broker, independently of any app's PID, UID or rootfs.
class CodecServices {
public:
    CodecServices(std::string socket, std::string root, unsigned mask,
                  std::shared_ptr<BinderDomain> domain);
    ~CodecServices();
    CodecServices(const CodecServices&) = delete;
    CodecServices& operator=(const CodecServices&) = delete;
    bool ready();
    std::string status();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
