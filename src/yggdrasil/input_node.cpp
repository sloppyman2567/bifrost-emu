// yggdrasil/input_node.cpp — InputNode implementation.
//
// Reads return event records from the FrostInput ring buffer. The format
// depends on the InputDevice type passed to the constructor:
//   - InputDevice::Event: 24-byte input_event records
//   - InputDevice::Js:    8-byte js_event records
//   - InputDevice::Mouse: not yet implemented (returns -ENOSYS)
//
// When the queue is empty, reads return -EAGAIN (not 0/EOF).
//
// ioctl() supports:
//   - FIONREAD:  bytes available to read
//   - EVIOCG*:   device state queries (version, id, key state, abs info)
#include "yggdrasil/input_node.hpp"
#include "yggdrasil/terminal_ioctls.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/input.h>
#include <linux/joystick.h>
#endif
namespace arm64emu::yggdrasil {
namespace {
 // Helper: return bytes pending in the relevant queue for `dev`.
ssize_t pending_bytes(FrostInput* input, InputDevice dev) {
    if (!input) return 0;
    switch (dev) {
        case InputDevice::Event:
            return static_cast<ssize_t>(input->queue_size() *
                                        sizeof(struct ::input_event));
        case InputDevice::Js:
            return static_cast<ssize_t>(input->js_queue_size() *
                                        sizeof(struct ::js_event));
        case InputDevice::Mouse: return 0; // not implemented yet
    }
    return 0;
}
#if defined(__linux__)
// evdev ioctls encode len in _IOC_SIZE, so EVIOCGNAME(256) !=
// EVIOCGNAME(0). match on type+nr only.
inline uint32_t ev_type(uint32_t r) { return (r >> 8) & 0xFFu; }
inline uint32_t ev_nr(uint32_t r) { return r & 0xFFu; }
inline uint32_t ev_len(uint32_t r) { return (r >> 16) & 0x3FFFu; }
inline void write_bitmap(Memory& mem, uint64_t argp, uint32_t len,
                         int max, const uint16_t* codes, size_t n) {
    uint32_t need = static_cast<uint32_t>(max / 8 + 1);
    uint32_t out = (len == 0 || len > need) ? need : len;
    for (uint32_t i = 0; i < out; ++i) mem.store<uint8_t>(argp + i, 0);
    if (codes) {
        for (size_t k = 0; k < n; ++k) {
            uint32_t byte = codes[k] / 8u;
            if (byte >= out) continue;
            uint8_t cur = 0;
            mem.read(argp + byte, &cur, 1);
            cur |= static_cast<uint8_t>(1u << (codes[k] % 8u));
            mem.store<uint8_t>(argp + byte, cur);
        }
    }
}
#endif
} // unnamed namespace
ssize_t InputNode::read(uint64_t /*off*/, void* buf, size_t n) {
    if (!input_) return -ENODEV;
    if (n == 0) return 0;
    bool blocking = (flags_ & O_NONBLOCK) == 0;
    return input_->read(static_cast<uint8_t*>(buf), n, dev_, blocking);
}
ssize_t InputNode::write(uint64_t /*off*/, const void* /*buf*/, size_t /*n*/) {
    return -EACCES;  // input devices are read-only
}
ssize_t InputNode::lseek(int64_t /*off*/, int /*whence*/) {
    return -ESPIPE;  // not seekable
}
int InputNode::fstat(struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0400;  // char device, read-only
    st->st_size = 0;
    st->st_nlink = 1;
    return 0;
}
int InputNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    if (!input_) return -ENODEV;
    // Every handled request writes through argp (FIONREAD included), so a
    // NULL argument must be EFAULT — the old code exempted FIONREAD and
    // then stored to guest address 0.
    if (argp == 0) return -EFAULT;
    if (request == ioctl_num::REQ_FIONREAD) {
        int n = static_cast<int>(pending_bytes(input_, dev_));
        mem.store<int32_t>(argp, n);
        return 0;
    }
#if defined(__linux__)
    if (request == static_cast<uint32_t>(EVIOCGVERSION)) {
        mem.store<int32_t>(argp, 0x010000);
        return 0;
    }
    if (request == static_cast<uint32_t>(EVIOCGID)) {
        struct input_id id;
        memset(&id, 0, sizeof(id));
        id.bustype = BUS_USB;
        id.vendor  = 0x1af4;
        id.product = 0x0001;
        id.version = 0x0100;
        mem.write(argp, &id, sizeof(id));
        return 0;
    }
    // variable-len evdev queries: match type+nr, clamp to guest len.
    if (ev_type(request) == 'E') {
        uint32_t nr = ev_nr(request);
        uint32_t len = ev_len(request);
        if (nr == 0x06) {  // EVIOCGNAME
            const char* name = "bifrost-emu virtual input";
            size_t need = strlen(name) + 1;
            size_t out = (len == 0 || len > need) ? need : len;
            mem.write(argp, name, out);
            return static_cast<int>(need);
        }
        if (nr == 0x07) {  // EVIOCGPHYS
            const char* phys = "input0";
            size_t need = strlen(phys) + 1;
            size_t out = (len == 0 || len > need) ? need : len;
            mem.write(argp, phys, out);
            return static_cast<int>(need);
        }
        if (nr == 0x08) {  // EVIOCGUNIQ
            mem.store<uint8_t>(argp, 0);
            return 1;
        }
        if (nr == 0x18) {  // EVIOCGKEY: live pressed-key bitmap
            uint32_t need = KEY_MAX / 8 + 1;
            uint32_t out = (len == 0 || len > need) ? need : len;
            uint8_t bmp[96] = {};
            input_->key_bitmap(bmp, sizeof(bmp));
            for (uint32_t i = 0; i < out; ++i)
                mem.store<uint8_t>(argp + i, i < sizeof(bmp) ? bmp[i] : 0);
            return static_cast<int>(need);
        }
        if (nr == 0x19) {  // EVIOCGLED
            uint32_t need = LED_MAX / 8 + 1;
            uint32_t out = (len == 0 || len > need) ? need : len;
            for (uint32_t i = 0; i < out; ++i) mem.store<uint8_t>(argp + i, 0);
            return static_cast<int>(need);
        }
        if (nr == 0x1a) {  // EVIOCGSND
            uint32_t need = SND_MAX / 8 + 1;
            uint32_t out = (len == 0 || len > need) ? need : len;
            for (uint32_t i = 0; i < out; ++i) mem.store<uint8_t>(argp + i, 0);
            return static_cast<int>(need);
        }
        if (nr == 0x1b) {  // EVIOCGSW
            uint32_t need = SW_MAX / 8 + 1;
            uint32_t out = (len == 0 || len > need) ? need : len;
            for (uint32_t i = 0; i < out; ++i) mem.store<uint8_t>(argp + i, 0);
            return static_cast<int>(need);
        }
        if (nr >= 0x20 && nr <= 0x20 + EV_MAX) {  // EVIOCGBIT(ev,len)
            uint32_t ev = nr - 0x20;
            if (ev == EV_SYN) {
                static const uint16_t codes[] = {0};
                write_bitmap(mem, argp, len, 1, codes, 1);
                return 2;
            }
            if (ev == EV_KEY) {
                static const uint16_t codes[] = {
                    1, 14, 15, 28, 29, 42, 54, 56, 57, 58, 97, 100,
                    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                    16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                    26, 27, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
                    41, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
                    102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
                    0x110, 0x111, 0x112, 0x113, 0x114,
                    0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x138,
                    0x139, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e,
                    0x220, 0x221, 0x222, 0x223,
                };
                write_bitmap(mem, argp, len, KEY_MAX, codes,
                             sizeof(codes) / sizeof(codes[0]));
                return KEY_MAX / 8 + 1;
            }
            if (ev == EV_REL) {
                static const uint16_t codes[] = {0, 1, 8};
                write_bitmap(mem, argp, len, REL_MAX, codes, 3);
                return REL_MAX / 8 + 1;
            }
            if (ev == EV_ABS) {
                static const uint16_t codes[] = {0, 1, 2, 3, 4, 5, 0x10, 0x11};
                write_bitmap(mem, argp, len, ABS_MAX, codes, 8);
                return ABS_MAX / 8 + 1;
            }
            return IOCTL_NOT_HANDLED;
        }
        if (nr >= 0x40 && nr <= 0x40 + ABS_MAX) {  // EVIOCGABS(axis)
            struct input_absinfo ai;
            memset(&ai, 0, sizeof(ai));
            ai.minimum = -32768;
            ai.maximum = 32767;
            mem.write(argp, &ai, sizeof(ai));
            return 0;
        }
    }
#endif // __linux__
    return IOCTL_NOT_HANDLED;
}
} // namespace arm64emu::yggdrasil
