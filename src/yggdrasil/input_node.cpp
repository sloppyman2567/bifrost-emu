// yggdrasil/input_node.cpp — InputNode implementation.
//
// Reads return event records from the FrostInput ring buffer. The format
// depends on the InputDevice type passed to the constructor:
//   - InputDevice::Event: 24-byte input_event records
//   - InputDevice::Js:    8-byte js_event records
//   - InputDevice::Mouse: not yet implemented (returns -ENOSYS)
//
// When the queue is empty, reads return 0 (EOF).
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
    // Use queue_size() (events currently in the ring buffer) rather than
    // event_count() (total events ever pushed) so FIONREAD reports the
    // correct number of bytes available to read.
    size_t count = input->queue_size();
    switch (dev) {
        case InputDevice::Event: return static_cast<ssize_t>(count * sizeof(struct ::input_event));
        case InputDevice::Js:    return static_cast<ssize_t>(count * sizeof(struct ::js_event));
        case InputDevice::Mouse: return 0; // not implemented yet
    }
    return 0;
}
} // unnamed namespace
ssize_t InputNode::read(uint64_t /*off*/, void* buf, size_t n) {
    if (!input_) return -ENODEV;
    if (n == 0) return 0;
    return input_->read(static_cast<uint8_t*>(buf), n, dev_, /*blocking=*/false);
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
    if (argp == 0 && request != ioctl_num::REQ_FIONREAD) return -EFAULT;
    switch (request) {
        case ioctl_num::REQ_FIONREAD: {
            // FIONREAD: write bytes-available to int*.
            int n = static_cast<int>(pending_bytes(input_, dev_));
            mem.store<int32_t>(argp, n);
            return 0;
        }
#if defined(__linux__)
        case EVIOCGVERSION: {
            // Return driver version (int).
            mem.store<int32_t>(argp, 0x010000); // EV_VERSION = 0x010000
            return 0;
        }
        case EVIOCGID: {
            // Return struct input_id (bustype, vendor, product, version).
            struct input_id id;
            memset(&id, 0, sizeof(id));
            // Provide sensible defaults so guests can identify the device.
            id.bustype = BUS_USB;
            id.vendor  = 0x1af4; // QEMU virt vendor
            id.product = 0x0001;
            id.version = 0x0100;
            mem.write(argp, &id, sizeof(id));
            return 0;
        }
        case EVIOCGNAME(0): {
            // EVIOCGNAME(len): if len==0, just return the needed size.
            // We report a fixed name; caller provides a buffer.
            const char* name = "bifrost-emu virtual input";
            size_t len = strlen(name) + 1;
            mem.write(argp, name, len);
            return static_cast<int>(len);
        }
        case EVIOCGPHYS(0): {
            const char* phys = "input0";
            size_t len = strlen(phys) + 1;
            mem.write(argp, phys, len);
            return static_cast<int>(len);
        }
        case EVIOCGUNIQ(0): {
            const char* uniq = "";
            mem.write(argp, uniq, 1);
            return 1;
        }
        case EVIOCGKEY(0): {
            // Return key state bitmap (KEY_MAX / 8 bytes).
            // All zeroes: no keys pressed.
            int nbytes = KEY_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGLED(0): {
            // LED state bitmap (LED_MAX / 8 bytes).
            int nbytes = LED_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGSND(0): {
            // Sound state bitmap (SND_MAX / 8 bytes).
            int nbytes = SND_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGSW(0): {
            // Switch state bitmap (SW_MAX / 8 bytes).
            int nbytes = SW_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGBIT(EV_KEY, 0): {
            // Return supported key codes bitmap.
            int nbytes = KEY_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGBIT(EV_REL, 0): {
            // Return supported relative axes bitmap.
            int nbytes = REL_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGBIT(EV_ABS, 0): {
            // Return supported absolute axes bitmap.
            int nbytes = ABS_MAX / 8 + 1;
            uint8_t zero = 0;
            for (int i = 0; i < nbytes; ++i) mem.store<uint8_t>(argp + i, zero);
            return nbytes;
        }
        case EVIOCGABS(ABS_X):
        case EVIOCGABS(ABS_Y):
        case EVIOCGABS(ABS_Z):
        case EVIOCGABS(ABS_RX):
        case EVIOCGABS(ABS_RY):
        case EVIOCGABS(ABS_RZ): {
            // Return struct input_absinfo for the requested axis.
            struct input_absinfo ai;
            memset(&ai, 0, sizeof(ai));
            ai.value = 0;
            ai.minimum = 0;
            ai.maximum = 255;
            ai.fuzz = 0;
            ai.flat = 0;
            ai.resolution = 0;
            mem.write(argp, &ai, sizeof(ai));
            return 0;
        }
#endif // __linux__
        default:
            return IOCTL_NOT_HANDLED;
    }
}
} // namespace arm64emu::yggdrasil
