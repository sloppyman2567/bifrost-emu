// yggdrasil/devfs.cpp — DevFS: /dev/* device nodes.
//
// Most device paths are passed through to the host (which already has
// them). /dev/fb0 is special — it returns a memfd backed by the
// GraphicsBackend's framebuffer memory.
//
// /dev/stdin, /dev/stdout, /dev/stderr are dup()'d from the host's
// 0/1/2 so close(guest_fd) doesn't close the host's stdio.
//
// v1.4.5-alpha improvements:
//   - /dev is now a DirNode, so `ls /dev` works (previously returned
//     nothing).
//   - /dev/random vs /dev/urandom distinction: real Linux has /dev/random
//     block when the entropy pool is low and /dev/urandom never block.
//     We can't easily simulate blocking, but we CAN make them return
//     different byte sequences (previously both mapped to the same
//     host fd via openat, which gave identical results — fine for
//     most guests but incorrect for crypto tests that check the
//     distinction). Now /dev/random uses getrandom(GRND_RANDOM) and
//     /dev/urandom uses getrandom(GRND_NONBLOCK) — both non-blocking
//     on modern Linux, but the underlying pool selection differs.
#include "yggdrasil/yggdrasil.hpp"
#include "yggdrasil/host_node.hpp"
#include "yggdrasil/memfd_node.hpp"
#include "yggdrasil/fb_node.hpp"
#include "yggdrasil/audio_node.hpp"
#include "yggdrasil/dir_node.hpp"
#include "audio/audio.h"
#include "frost/graphics.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

// /dev directory listing. Only the entries we actually handle.
static std::vector<DirNode::Entry> dev_entries() {
    return {
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
        {"ptmx",    0x2 /*DT_CHR*/},
        {"pts",     0x4 /*DT_DIR*/},
    };
}

// Helper: open /dev/random or /dev/urandom via getrandom(). We can't
// create a HostNode from getrandom (it's a syscall, not an fd), so we
// use a memfd seeded with random bytes. For /dev/random we use
// GRND_RANDOM (draws from the blocking pool — non-blocking on modern
// Linux because the pool is always initialized); for /dev/urandom we
// use the default (GRND_NONBLOCK equivalent — urandom pool, never
// blocks). Both return 256 bytes per read by default; the guest can
// read less if it wants. We refresh on seek-to-0 so re-reads give
// fresh random bytes (matching real /dev/urandom behavior).
static std::unique_ptr<Node> open_random_node(bool blocking_pool, int flags) {
    auto regen = [blocking_pool]() -> std::string {
        // 256 bytes per call — matches the typical /dev/urandom read
        // size for seed material. The guest can read less; reads
        // beyond 256 return 0 (EOF) until seek-to-0 refreshes.
        char buf[256];
        int flags_get = blocking_pool ? GRND_RANDOM : 0;
        ssize_t n = ::getrandom(buf, sizeof(buf), flags_get);
        if (n < 0) {
            // Fall back to /dev/urandom read if getrandom fails.
            int fd = ::openat(AT_FDCWD, "/dev/urandom", O_RDONLY);
            if (fd < 0) return std::string();
            n = ::read(fd, buf, sizeof(buf));
            ::close(fd);
            if (n < 0) return std::string();
        }
        return std::string(buf, static_cast<size_t>(n));
    };
    return MemfdNode::create_lazy(
        blocking_pool ? "bifrost-dev-random" : "bifrost-dev-urandom",
        regen, flags);
}

std::unique_ptr<Node> Yggdrasil::open_devfs(const std::string& path,
                                            int flags, mode_t mode, int* err_out) {
    // ── /dev directory listing ─────────────────────────────────────
    if (path == "/dev" || path == "/dev/") {
        return std::make_unique<DirNode>("/dev", dev_entries(), flags);
    }

    // /dev/null, /dev/zero → host passthrough
    if (path == "/dev/null" || path == "/dev/zero") {
        int fd = ::openat(AT_FDCWD, path.c_str(), flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }

    // /dev/random vs /dev/urandom — distinct pools (v1.4.5-alpha).
    if (path == "/dev/random")  return open_random_node(/*blocking=*/true,  flags);
    if (path == "/dev/urandom") return open_random_node(/*blocking=*/false, flags);

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

    // /dev/snd, /dev/dsp, /dev/audio → audio backend (PCM buffer + WAV dump)
    if (path == "/dev/snd" || path == "/dev/dsp" || path == "/dev/audio") {
        if (audio_) {
            // Open the audio backend with default params (44100 Hz, stereo, 16-bit).
            if (!audio_->ready()) {
                audio_->open(44100, 2, 2);
            }
            return std::make_unique<AudioNode>(audio_, flags);
        }
        // No audio backend — fall back to /dev/null so writes succeed.
        int fd = ::openat(AT_FDCWD, "/dev/null", flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
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
