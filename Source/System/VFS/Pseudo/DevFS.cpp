// DevFS.cpp — DevFS: /dev/* device nodes.
//
// Most device paths are passed through to the host (which already has
// them). /dev/fb0 is special — it returns a memfd backed by the
// GraphicsBackend's framebuffer memory.
//
// /dev/stdin, /dev/stdout, /dev/stderr are dup()'d from the host's
// 0/1/2 so close(guest_fd) doesn't close the host's stdio.
//
// Behavior:
//   - /dev is now a DirNode, so `ls /dev` works (previously returned
//     nothing).
//   - /dev/random and /dev/urandom pass through to the host devices:
//     reads of any size return fresh bytes and never reach EOF, as in
//     drivers/char/random.c. (A memfd seeded with 256 bytes ran dry for
//     guests that keep the fd open, e.g. Teeworlds' secure_random_fill.)
#include "System/VFS/VFS.hpp"
#include "System/Android/Binder/BinderNode.hpp"
#include "System/Android/Binder/Broker.hpp"
#include "System/Android/Media/GoldfishMedia.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "System/VFS/Nodes/MemfdNode.hpp"
#include "System/VFS/Nodes/FramebufferNode.hpp"
#include "System/VFS/Nodes/AudioNode.hpp"
#include "System/VFS/Nodes/InputNode.hpp"
#include "System/VFS/Nodes/DirectoryNode.hpp"
#include "System/Android/Properties/Properties.hpp"
#include "System/Audio/Audio.hpp"
#include "System/Graphics/Display/Graphics.hpp"
#include "System/Graphics/Input/Input.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
namespace arm64emu::yggdrasil {
// /dev directory listing. Only the entries we actually handle.
static std::vector<DirNode::Entry> dev_entries() {
    return {
        {"binder", 0x2}, {"hwbinder", 0x2}, {"vndbinder", 0x2},
        {"null",    0x2 /*DT_CHR*/},
        {"zero",    0x2 /*DT_CHR*/},
        {"urandom", 0x2 /*DT_CHR*/},
        {"random",  0x2 /*DT_CHR*/},
        {"tty",     0x2 /*DT_CHR*/},
        {"fb0",     0x2 /*DT_CHR*/},
        {"stdin",   0x2 /*DT_CHR*/},
        {"stdout",  0x2 /*DT_CHR*/},
        {"stderr",  0x2 /*DT_CHR*/},
        {"dsp",     0x2 /*DT_CHR*/},
        {"snd",     0x2 /*DT_CHR*/},
        {"audio",   0x2 /*DT_CHR*/},
        {"input",   0x4 /*DT_DIR*/},
        {"ptmx",    0x2 /*DT_CHR*/},
        {"pts",     0x4 /*DT_DIR*/},
    };
}
// /dev/input directory listing. We expose a single event device
// (event0) that aggregates keyboard + mouse events from the SDL2
// window. Real Linux has /dev/input/event0..eventN (one per device);
// we collapse them into one for simplicity.
static std::vector<DirNode::Entry> dev_input_entries() {
    return {
        {"event0", 0x2 /*DT_CHR*/},
        {"mice",   0x2 /*DT_CHR*/},
        {"mouse0", 0x2 /*DT_CHR*/},
        {"js0",    0x2 /*DT_CHR*/},
    };
}
std::unique_ptr<Node> Yggdrasil::open_devfs(const std::string& path,
                                            int flags, mode_t mode, int* err_out) {
    // ── /dev directory listing ─────────────────────────────────────
    if (path == "/dev" || path == "/dev/") {
        return std::make_unique<DirNode>("/dev", dev_entries(), flags);
    }
    if (path == "/dev/binder" || path == "/dev/hwbinder" || path == "/dev/vndbinder") {
        if ((flags & O_ACCMODE) != O_RDWR) { *err_out = -EINVAL; return nullptr; }
        if (!binder_broker_.empty())
            return open_broker_binder(binder_broker_, path, flags, process_uid_, ::getpid(), err_out);
        std::lock_guard<std::mutex> lock(binder_mu_);
        auto& domain = binder_domains_[path];
        if (!domain) {
            domain = std::make_shared<BinderDomain>();
            domain->sender_euid = process_uid_;
            domain->service_manager = android_services_ && path == "/dev/binder";
        }
        return std::make_unique<BinderNode>(domain, flags);
    }
    if (path == "/dev/goldfish_address_space" && android_properties_)
        return open_goldfish_media(flags, err_out);
    // Android system properties (Bionic "pre-split" single prop_area).
    if (path == "/dev/__properties__" && android_properties_) {
        if ((flags & O_ACCMODE) != O_RDONLY) { *err_out = -EACCES; return nullptr; }
        std::call_once(property_area_once_, [this] {
            size_t skipped = 0;
            auto properties = load_android_properties(sandbox_root_path());
            // Native libbinder waits for init's readiness property before
            // contacting handle 0. Our selected registry is ready immediately;
            // a root's build.prop must not advertise one when it is disabled.
            properties["servicemanager.ready"] = android_services_ ? "true" : "false";
            property_area_ = build_property_area(properties, &skipped);
            if (skipped)
                fprintf(stderr, "[android] %zu system properties did not fit\n", skipped);
        });
        auto node = MemfdNode::create("bifrost-properties", property_area_, flags);
        if (!node) { *err_out = -errno; return nullptr; }
        node->present_root_owned();
        return node;
    }
    // /dev/null, /dev/zero, /dev/random, /dev/urandom → host passthrough
    if (path == "/dev/null" || path == "/dev/zero" ||
        path == "/dev/random" || path == "/dev/urandom") {
        int fd = ::openat(AT_FDCWD, path.c_str(), flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }
    // /dev/fb0 → virtual framebuffer (memfd-backed via GraphicsBackend)
    if (path == "/dev/fb0") {
        if (!gfx_) { *err_out = -ENODEV; return nullptr; }
        int guest_fd = gfx_->open_dev_fb0();
        if (guest_fd < 0) { *err_out = -ENODEV; return nullptr; }
        return std::make_unique<FbNode>(guest_fd, flags, gfx_);
    }
    // /dev/tty → open the host's controlling terminal.
    if (path == "/dev/tty") {
        int fd = ::openat(AT_FDCWD, "/dev/tty", flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }
    // /dev/stdin, /dev/stdout, /dev/stderr → dup the host fd.
    if (path == "/dev/stdin") {
        int r = ::dup(0);
        if (r < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(r, flags);
    }
    if (path == "/dev/stdout") {
        int r = ::dup(1);
        if (r < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(r, flags);
    }
    if (path == "/dev/stderr") {
        int r = ::dup(2);
        if (r < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(r, flags);
    }
    // /dev/snd, /dev/dsp, /dev/audio → OSS playback streams.
    if (path == "/dev/snd" || path == "/dev/dsp" || path == "/dev/audio") {
        if (audio_) {
            // The node negotiates its own format before opening a logical
            // stream; opening a fixed-rate legacy device here loses OSS setup.
            return std::make_unique<AudioNode>(audio_, flags);
        }
        // No audio backend — fall back to /dev/null so writes succeed.
        int fd = ::openat(AT_FDCWD, "/dev/null", flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }
    // /dev/input → directory listing
    if (path == "/dev/input" || path == "/dev/input/") {
        return std::make_unique<DirNode>("/dev/input", dev_input_entries(), flags);
    }
    // /dev/input/eventX, /dev/input/mice, /dev/input/mouse0, /dev/input/js0
    // → input backend (FrostInput, owned by FrostGraphics).
    //   - eventX: 24-byte input_event records (EV_KEY/EV_REL/EV_ABS)
    //   - js0:    8-byte js_event records (JS_EVENT_BUTTON/AXIS)
    //   - mice/mouse0: ImPS/2 packets (not yet implemented)
    if (path.rfind("/dev/input/", 0) == 0) {
        std::string name = path.substr(strlen("/dev/input/"));
        ::arm64emu::InputDevice dev = ::arm64emu::InputDevice::Event;
        if (name == "event0") {
            dev = ::arm64emu::InputDevice::Event;
        } else if (name == "js0") {
            dev = ::arm64emu::InputDevice::Js;
        } else if (name == "mice" || name == "mouse0") {
            dev = ::arm64emu::InputDevice::Mouse;
        } else {
            // Unknown /dev/input/ path — fall through to host passthrough.
            *err_out = 0;
            return nullptr;
        }
        if (gfx_) {
            FrostInput* in = gfx_->input();
            if (in) {
                return std::make_unique<InputNode>(in, dev, flags);
            }
        }
        *err_out = -ENODEV;
        return nullptr;
    }
    // /dev/ptmx → pseudo-terminal master (passthrough for interactive apps)
    if (path == "/dev/ptmx") {
        int fd = ::openat(AT_FDCWD, "/dev/ptmx", flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }
    // /dev/pts/N → pseudo-terminal slaves (passthrough)
    if (path.rfind("/dev/pts/", 0) == 0) {
        int fd = ::openat(AT_FDCWD, path.c_str(), flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }
    *err_out = 0;
    return nullptr;  // not a /dev path we handle
}
} // namespace arm64emu::yggdrasil

namespace arm64emu::yggdrasil {
void Yggdrasil::enable_android_services() {
    std::lock_guard<std::mutex> lock(binder_mu_);
    if (!binder_domains_.empty())
        throw std::logic_error("Android services must be selected before opening Binder");
    android_services_ = true;
}
int Yggdrasil::report_android_process_state(int32_t state) {
    if (!binder_broker_.empty()) return report_broker_process_state(binder_broker_, state);
    std::lock_guard<std::mutex> lock(binder_mu_);
    auto domain = binder_domains_.find("/dev/binder");
    if (domain == binder_domains_.end()) return -ESRCH;
    return domain->second->report_process_state(process_ids().pid, state);
}
void Yggdrasil::stop_binder() {
    std::lock_guard<std::mutex> lock(binder_mu_);
    for (auto& entry : binder_domains_) entry.second->stop();
    binder_domains_.clear();
}
}

namespace arm64emu::yggdrasil {
bool Yggdrasil::binder_device_stat(const std::string& path, struct stat* st) {
    if (path != "/dev/binder" && path != "/dev/hwbinder" && path != "/dev/vndbinder") return false;
    memset(st, 0, sizeof(*st)); st->st_mode = S_IFCHR | 0666; st->st_nlink = 1;
    return true;
}
}
