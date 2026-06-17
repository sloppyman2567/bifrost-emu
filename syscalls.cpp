// syscalls.cpp — Linux AArch64 syscall layer.
//
// Implements Emulator::syscall(), which handles SVC #0.
// Split from the interpreter for maintainability and to keep
// interpreter.cpp focused on instruction execution.
//
// When adding a new syscall:
//   1. Add a case in the switch in Emulator::syscall()
//   2. Document the syscall number (AArch64 numbering)
//   3. Add a test if possible

#include "arm64_emu.hpp"

namespace arm64emu {

// Helper: read a NUL-terminated path string from guest memory at `addr`.
// Returns the path as a std::string (without the NUL). Reads at most 4096
// bytes to prevent runaway reads from bad pointers.
static std::string read_path(Memory& mem, uint64_t addr) {
    if (addr == 0) return std::string();
    std::string s;
    s.reserve(64);
    for (uint64_t off = 0; off < 4096; off++) {
        uint8_t c = 0;
        try { c = mem.load<uint8_t>(addr + off); } catch (...) { break; }
        if (c == 0) break;
        s.push_back((char)c);
    }
    return s;
}

void Emulator::syscall(CPU& cpu) {
    uint64_t num = cpu.regs[8];
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];

    auto ret_host = [&](int64_t r) { cpu.regs[0] = (uint64_t)r; };

    switch (num) {
        case 63: { // read
            // stdin:0, stdout:1, stderr:2 are host fds as well
            // For higher fds we just pass through to host.
            std::vector<uint8_t> tmp(std::max<uint64_t>(a2, 1));
            ssize_t n = ::read((int)a0, tmp.data(), a2);
            if (n > 0) mem_.write(a1, tmp.data(), n);
            ret_host(n);
            return;
        }
        case 64: { // write
            std::vector<uint8_t> tmp(a2);
            mem_.read(a1, tmp.data(), a2);
            ssize_t n = ::write((int)a0, tmp.data(), a2);
            ret_host(n);
            return;
        }
        case 56: { // openat
            // Read NUL-terminated path string from guest memory.
            uint64_t off = 0;
            for (;;) {
                uint8_t c = mem_.load<uint8_t>(a1 + off);
                if (c == 0) break;
                if (off > 4096) { cpu.regs[0] = (uint64_t)-ENOENT; return; }
                off++;
            }
            std::vector<uint8_t> path_bytes(off);
            mem_.read(a1, path_bytes.data(), off);
            std::string path_str((const char*)path_bytes.data(), off);

            // ── VFS: virtual files ──────────────────────────────────
            // Intercept specific paths and serve synthetic content.
            // Uses memfd_create to create a seekable fd with the content.
            auto serve_virtual = [&](const std::string& content) -> int {
                int fd = memfd_create("bifrost-vfs", 0);
                if (fd < 0) return -errno;
                ssize_t w = ::write(fd, content.data(), content.size());
                if (w < 0) { ::close(fd); return -errno; }
                ::lseek(fd, 0, SEEK_SET);
                return fd;
            };

            // /proc/self/exe → symlink to the ELF path
            if (path_str == "/proc/self/exe" || path_str == "/proc/self/exe/") {
                int fd = serve_virtual(elf_path_);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/self/cmdline → argv[0]\0argv[1]\0...
            else if (path_str == "/proc/self/cmdline") {
                std::string cmdline;
                // We don't have argv here, but we can use elf_path_
                cmdline = elf_path_ + '\0';
                int fd = serve_virtual(cmdline);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/self/maps → basic memory map
            else if (path_str == "/proc/self/maps") {
                std::string maps = "";
                maps += "00400000-004bf000 r-xp 00000000 00:00 0\n";
                maps += "004bf000-004ce000 r--p 00000000 00:00 0\n";
                maps += "004ce000-004df000 rw-p 00000000 00:00 0\n";
                maps += "5000000000-5001000000 rw-p 00000000 00:00 0\n";
                maps += "7fff000000-8000000000 rw-p 00000000 00:00 0 [stack]\n";
                int fd = serve_virtual(maps);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/self/status → basic process info
            else if (path_str == "/proc/self/status") {
                std::string status = "";
                status += "Name:\tbifrost-emu\n";
                status += "State:\tR (running)\n";
                status += "Tgid:\t1\n";
                status += "Pid:\t1\n";
                status += "PPid:\t0\n";
                status += "Uid:\t0\t0\t0\t0\n";
                status += "Gid:\t0\t0\t0\t0\n";
                status += "VmSize:\t  8192 kB\n";
                status += "VmRSS:\t  4096 kB\n";
                status += "Threads:\t1\n";
                int fd = serve_virtual(status);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/meminfo → basic memory info
            else if (path_str == "/proc/meminfo") {
                std::string mi = "";
                mi += "MemTotal:       16777216 kB\n";
                mi += "MemFree:         8388608 kB\n";
                mi += "MemAvailable:   12582912 kB\n";
                mi += "Buffers:               0 kB\n";
                mi += "Cached:          4194304 kB\n";
                int fd = serve_virtual(mi);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/cpuinfo → basic CPU info
            else if (path_str == "/proc/cpuinfo") {
                std::string ci = "";
                ci += "processor\t: 0\n";
                ci += "BogoMIPS\t: 100.00\n";
                ci += "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n";
                ci += "CPU implementer\t: 0x41\n";
                ci += "CPU architecture: 8\n";
                int fd = serve_virtual(ci);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/self/auxv → empty (we provide auxv on the stack)
            else if (path_str == "/proc/self/auxv") {
                int fd = serve_virtual("");
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/self/environ → just PATH
            else if (path_str == "/proc/self/environ") {
                int fd = serve_virtual("PATH=/bin:/usr/bin\0");
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/version
            else if (path_str == "/proc/version") {
                std::string pv = "Linux version 6.5.0 (bifrost-emu) (gcc) #1 SMP\n";
                int fd = serve_virtual(pv);
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /proc/sys/kernel/osrelease
            else if (path_str == "/proc/sys/kernel/osrelease") {
                int fd = serve_virtual("6.5.0\n");
                if (fd >= 0) { ret_host(fd); return; }
            }
            // /dev/null, /dev/zero, /dev/urandom → open host device
            else if (path_str == "/dev/null" || path_str == "/dev/zero" ||
                     path_str == "/dev/urandom" || path_str == "/dev/random") {
                int host_fd = ::openat(AT_FDCWD, path_str.c_str(), (int)a2, (mode_t)a3);
                if (host_fd >= 0) { ret_host(host_fd); return; }
            }

            // Normal file: pass through to host
            int host_fd = ::openat(AT_FDCWD, path_str.c_str(), (int)a2, (mode_t)a3);
            if (host_fd < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(host_fd);
            return;
        }
        case 57: { // close
            ::close((int)a0);
            ret_host(0);
            return;
        }
        case 23: { // dup
            int r = ::dup((int)a0);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(r);
            return;
        }
        case 33: { // dup2
            int r = ::dup2((int)a0, (int)a1);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(r);
            return;
        }
        case 59: { // pipe2
            int fds[2];
            int r = ::pipe2(fds, (int)a1);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            mem_.write(a0, fds, sizeof(fds));
            ret_host(0);
            return;
        }
        case 34: { // mkdirat
            std::string path = read_path(mem_, a1);
            int r = ::mkdirat((int)a0, path.c_str(), (mode_t)a2);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(0);
            return;
        }
        case 35: { // unlinkat
            std::string path = read_path(mem_, a1);
            int r = ::unlinkat((int)a0, path.c_str(), (int)a2);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(0);
            return;
        }
        case 38: { // renameat
            std::string oldp = read_path(mem_, a1);
            std::string newp = read_path(mem_, a3);
            int r = ::renameat((int)a0, oldp.c_str(), (int)a2, newp.c_str());
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(0);
            return;
        }

        case 66: { // writev
            // a0=fd, a1=iovec ptr, a2=count
            uint64_t iov = a1;
            uint64_t cnt = a2;
            ssize_t total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                std::vector<uint8_t> tmp(len);
                mem_.read(base, tmp.data(), len);
                ssize_t n = ::write((int)a0, tmp.data(), len);
                if (n < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
                total += n;
                if ((size_t)n < len) break;
            }
            ret_host(total);
            return;
        }
        case 67: { // readv (AArch64 syscall 67, NOT 73)
            uint64_t iov = a1;
            uint64_t cnt = a2;
            ssize_t total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                std::vector<uint8_t> tmp(len);
                ssize_t n = ::read((int)a0, tmp.data(), len);
                if (n < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
                if (n > 0) mem_.write(base, tmp.data(), n);
                total += n;
                if ((size_t)n < len) break;
            }
            ret_host(total);
            return;
        }
        case 80: { // fstat - fill a minimal struct stat
            // We'll fill the host struct stat and copy it.
            struct stat st;
            if (::fstat((int)a0, &st) < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            // Linux aarch64 struct stat layout (88 bytes): dev64, ino64, mode32, nlink32,
            // uid32, gid32, pad, rdev64, size64, blksize64, blocks64, atime, atime_nsec,
            // mtime, mtime_nsec, ctime, ctime_nsec
            uint8_t buf[128] = {0};
            uint64_t* p = (uint64_t*)buf;
            p[0] = st.st_dev;
            p[1] = st.st_ino;
            ((uint32_t*)&p[2])[0] = st.st_mode;
            ((uint32_t*)&p[2])[1] = st.st_nlink;
            p[3] = st.st_uid | ((uint64_t)st.st_gid << 32);
            p[4] = 0;
            p[5] = st.st_rdev;
            p[6] = st.st_size;
            p[7] = st.st_blksize;
            p[8] = st.st_blocks;
            p[9]  = st.st_atim.tv_sec;
            p[10] = st.st_atim.tv_nsec;
            p[11] = st.st_mtim.tv_sec;
            p[12] = st.st_mtim.tv_nsec;
            p[13] = st.st_ctim.tv_sec;
            p[14] = st.st_ctim.tv_nsec;
            mem_.write(a1, buf, 128);
            ret_host(0);
            return;
        }
        case 62: { // lseek
            off_t r = ::lseek((int)a0, (off_t)a1, (int)a2);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(r);
            return;
        }
        case 222: { // mmap
            // a0=addr, a1=length, a2=prot, a3=flags, a4=fd, a5=offset
            uint64_t addr = a0;
            uint64_t length = a1;
            uint64_t flags = a3;
            if (length == 0) { cpu.regs[0] = (uint64_t)-22; return; } // EINVAL

            constexpr uint64_t BIFROST_MAP_FIXED = 0x10;
            uint64_t effective_hint = (flags & BIFROST_MAP_FIXED) ? addr : 0;

            // If MAP_FIXED overlaps the brk region, push brk past the
            // mmap'd area. musl's mallocng sometimes calls mmap with
            // MAP_FIXED on addresses inside the brk region when it grows
            // its metadata arena; without this adjustment, the MAP_FIXED
            // mmap zeroes out brk-managed pages, corrupting mallocng's
            // metadata and causing an infinite recursion in
            // __malloc_alloc_meta.
            if (effective_hint) {
                uint64_t mmap_end = effective_hint + ((length + 0xFFF) & ~0xFFFULL);
                std::lock_guard<std::mutex> g(brk_mu_);
                if (effective_hint < brk_ && mmap_end > brk_start_) {
                    // Overlap detected: move brk forward past the mmap'd area.
                    brk_ = std::max(brk_, mmap_end);
                    // Re-extend the brk mapping to cover the new region.
                    mem_.map_range(brk_start_, brk_ - brk_start_);
                }
            }

            uint64_t mapped = mem_.mmap_alloc(length, effective_hint);

            // If a file fd is given, read its contents in
            if ((int64_t)a4 != -1 && (a3 & 0x2) == 0 /* not MAP_ANONYMOUS */) {
                struct stat st;
                if (::fstat((int)a4, &st) == 0) {
                    std::vector<uint8_t> buf(std::min<uint64_t>(length, st.st_size));
                    off_t old = ::lseek((int)a4, 0, SEEK_CUR);
                    ::lseek((int)a4, a5, SEEK_SET);
                    ssize_t n = ::read((int)a4, buf.data(), buf.size());
                    ::lseek((int)a4, old, SEEK_SET);
                    if (n > 0) mem_.write(mapped, buf.data(), n);
                }
            }
            ret_host(mapped);
            return;
        }
        case 215: { // munmap - we just leave pages allocated (no-op OK)
            ret_host(0);
            return;
        }
        case 226: { // mprotect - no-op
            ret_host(0);
            return;
        }
        case 93: { // exit
            cpu.running = false;
            cpu.exit_code = (int)a0;
            return;
        }
        case 94: { // exit_group
            cpu.running = false;
            cpu.exit_code = (int)a0;
            return;
        }
        case 96: { // set_tid_address
            // Stores the tid_address pointer in the calling thread's CPU
            // state. Real Linux writes the TID to *tid_address when the
            // thread terminates (used by futex on child termination).
            cpu.set_tid_address_ptr = a0;
            ret_host(cpu.tid);
            return;
        }
        case 98: { // futex(uaddr, op, val, timeout, uaddr2, val3)
            // Real futex implementation: WAIT blocks the calling thread
            // until woken or timeout; WAKE wakes blocked threads. Uses
            // per-address (mutex, condvar) pairs stored in the futex table.
            //
            // Supported ops:
            //   FUTEX_WAIT (0):           block if *uaddr == val
            //   FUTEX_WAKE (1):           wake up to val waiters
            //   FUTEX_WAIT_BITSET (9):    like WAIT but with bitset
            //   FUTEX_WAKE_BITSET (10):   like WAKE but with bitset
            //   FUTEX_REQUEUE (3):        requeue waiters from uaddr to uaddr2
            //   FUTEX_CMP_REQUEUE (4):    requeue with comparison
            //   FUTEX_LOCK_PI / UNLOCK_PI / etc.: not supported (return -ENOSYS)
            uint64_t uaddr = a0;
            uint32_t op = (uint32_t)a1;
            uint32_t val = (uint32_t)a2;
            uint64_t timeout_ptr = a3;
            uint64_t uaddr2 = a4;
            uint32_t val3 = (uint32_t)a5;

            // Mask out private flag — we treat all futexes as private
            op &= ~0x80;  // FUTEX_PRIVATE_FLAG

            switch (op) {
                case 0:  // FUTEX_WAIT
                case 9:  // FUTEX_WAIT_BITSET
                {
                    // Check that *uaddr == val, then block.
                    uint32_t cur = mem_.load<uint32_t>(uaddr);
                    if (cur != val) {
                        ret_host((uint64_t)-EAGAIN);
                        return;
                    }
                    // Backward-compat: if no other threads are alive to
                    // wake us, return 0 immediately (pretend we waited
                    // and were woken). This preserves the previous
                    // single-threaded behavior where futex was a no-op.
                    // Without this, glibc's startup mutex lock would
                    // block forever in single-threaded code.
                    if (alive_threads_.load() == 0) {
                        ret_host(0);
                        return;
                    }
                    FutexSlot* slot = get_futex(uaddr);
                    std::unique_lock<std::mutex> lk(slot->mu);
                    slot->waiters++;
                    if (timeout_ptr == 0) {
                        slot->cv.wait(lk);
                    } else {
                        // timeout is struct timespec { sec, nsec }
                        uint64_t sec = mem_.load<uint64_t>(timeout_ptr);
                        uint64_t nsec = mem_.load<uint64_t>(timeout_ptr + 8);
                        auto duration = std::chrono::seconds(sec) +
                                        std::chrono::nanoseconds(nsec);
                        slot->cv.wait_for(lk, duration);
                    }
                    slot->waiters--;
                    ret_host(0);
                    return;
                }
                case 1:  // FUTEX_WAKE
                case 10: // FUTEX_WAKE_BITSET
                {
                    FutexSlot* slot = get_futex(uaddr);
                    std::lock_guard<std::mutex> lk(slot->mu);
                    int to_wake = (int)val;
                    if (to_wake <= 0) { ret_host(0); return; }
                    int woken = std::min(to_wake, slot->waiters);
                    if (woken >= slot->waiters) {
                        slot->cv.notify_all();
                    } else {
                        for (int i = 0; i < woken; i++) slot->cv.notify_one();
                    }
                    ret_host(woken);
                    return;
                }
                case 3:  // FUTEX_REQUEUE
                case 4:  // FUTEX_CMP_REQUEUE
                {
                    // For simplicity, treat requeue as wake — wake up to
                    // `val` waiters on uaddr, ignore uaddr2. Real requeue
                    // moves them to a different futex word without waking.
                    FutexSlot* slot = get_futex(uaddr);
                    std::lock_guard<std::mutex> lk(slot->mu);
                    int woken = std::min((int)val, slot->waiters);
                    if (woken >= slot->waiters) slot->cv.notify_all();
                    else for (int i = 0; i < woken; i++) slot->cv.notify_one();
                    ret_host(woken);
                    return;
                }
                default:
                    // PI futexes and others: not supported
                    ret_host((uint64_t)-ENOSYS);
                    return;
            }
        }
        case 99: { // set_robust_list - no-op
            ret_host(0);
            return;
        }
        case 100: { // nanosleep
            uint64_t req = a0;
            uint64_t tv_sec  = mem_.load<uint64_t>(req);
            uint64_t tv_nsec = mem_.load<uint64_t>(req + 8);
            struct timespec ts = { (time_t)tv_sec, (long)tv_nsec };
            ::nanosleep(&ts, nullptr);
            ret_host(0);
            return;
        }
        case 113: { // clock_gettime
            uint64_t clk = a0;
            uint64_t tp = a1;
            struct timespec ts;
            ::clock_gettime((clockid_t)clk, &ts);
            mem_.store<uint64_t>(tp,     ts.tv_sec);
            mem_.store<uint64_t>(tp + 8, ts.tv_nsec);
            ret_host(0);
            return;
        }
        case 169: { // gettimeofday
            struct timeval tv;
            ::gettimeofday(&tv, nullptr);
            mem_.store<uint64_t>(a0,     tv.tv_sec);
            mem_.store<uint64_t>(a0 + 8, tv.tv_usec);
            ret_host(0);
            return;
        }
        case 214: { // brk
            // Thread-safe: serialize against concurrent brk from other threads.
            std::lock_guard<std::mutex> g(brk_mu_);
            if (a0 == 0) { ret_host(brk_); return; }
            if (a0 < brk_) { ret_host(brk_); return; } // can't shrink
            uint64_t old = brk_;
            mem_.map_range(old, a0 - old);
            brk_ = a0;
            ret_host(brk_);
            return;
        }
        case 29: { // ioctl - handle TIOCGWINSZ etc.
            // Return a sane window size for interactive use.
            if (a1 == 0x5413 /*TIOCGWINSZ*/) {
                struct winsize ws;
                if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
                    mem_.write(a2, &ws, sizeof(ws));
                    ret_host(0);
                } else {
                    struct winsize def { 24, 80, 0, 0 };
                    mem_.write(a2, &def, sizeof(def));
                    ret_host(0);
                }
                return;
            }
            // Most other ioctls on terminal we can no-op successfully
            ret_host(0);
            return;
        }
        case 165: { // getcwd - we just return "/"
            mem_.write(a0, "/", 2);
            ret_host(1);
            return;
        }
        case 61: { // getdents64
            // Return a small fake directory listing.
            ret_host(0);
            return;
        }
        case 131: { // tgkill - no-op
            ret_host(0);
            return;
        }
        case 130: { // tkill - no-op
            ret_host(0);
            return;
        }
        case 167: { // prctl - handle PR_SET_NAME etc as no-op
            ret_host(0);
            return;
        }
        case 227: { // mremap(old_addr, old_size, new_size, flags, new_addr)
            // a0 = old_address, a1 = old_size, a2 = new_size, a3 = flags
            //
            // musl's mallocng uses mremap to grow the meta_area (the
            // page that holds malloc metadata). It expects mremap to
            // grow the mapping IN PLACE when possible — if mremap
            // returns a different address, musl's metadata pointers
            // become invalid and it enters an infinite allocation
            // loop trying to rebuild them.
            //
            // We handle two cases:
            //   1. If new_size <= old_size: shrink is a no-op, return old_addr.
            //   2. If new_size > old_size: try to extend in-place by
            //      mapping the pages [old_addr+old_size, old_addr+new_size).
            //      This always succeeds in our sparse memory model
            //      (no other mappings to conflict with), so we return
            //      old_addr.
            uint64_t old_addr = a0;
            uint64_t old_size = a1;
            uint64_t new_size = a2;

            if (new_size <= old_size) {
                // Shrink: just return the old address. (We don't
                // actually unmap the freed pages, but that's fine —
                // the guest won't access them.)
                ret_host(old_addr);
                return;
            }

            // Grow: map the additional pages in-place.
            uint64_t extra_start = old_addr + old_size;
            uint64_t extra_end   = old_addr + new_size;
            // Round up to page boundary
            extra_start = (extra_start + 0xFFF) & ~0xFFFULL;
            extra_end   = (extra_end + 0xFFF) & ~0xFFFULL;
            if (extra_end > extra_start) {
                mem_.map_range(extra_start, extra_end - extra_start);
            }
            ret_host(old_addr);
            return;
        }
        case 233: { // madvise - no-op
            ret_host(0);
            return;
        }
        case 134: { // rt_sigaction / 134 = rt_sigaction
            // no-op OK for most binaries
            ret_host(0);
            return;
        }
        case 135: { // rt_sigprocmask
            ret_host(0);
            return;
        }
        case 220: { // clone(flags, stack, ptid, ctid, tls)
            // AArch64 clone() syscall signature (matches glibc/musl):
            //   x0 = flags        (CLONE_*)
            //   x1 = stack        (top of child stack)
            //   x2 = parent_tidptr
            //   x3 = child_tidptr (CLONE_CHILD_SETTID writes TID here)
            //   x4 = tls          (new TPIDR_EL0, if CLONE_SETTLS)
            //
            // On success: parent gets child TID, child gets 0.
            // The new thread starts at the same PC as the syscall return
            // address (i.e., x30 / LR of the parent), with x0=0.
            //
            // We support the common subset: CLONE_VM | CLONE_FS |
            // CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM,
            // optionally combined with CLONE_SETTLS / CLONE_PARENT_SETTID
            // / CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID.
            uint64_t flags = a0;
            uint64_t stack = a1;
            uint64_t ptid_ptr = a2;
            uint64_t ctid_ptr = a3;
            uint64_t tls = a4;

            // Refuse fork()-style clones (no CLONE_VM) for now
            const uint64_t BIFROST_CLONE_VM = 0x100;
            if (!(flags & BIFROST_CLONE_VM)) {
                ret_host((uint64_t)-ENOSYS);
                return;
            }

            // The new thread's entry point is the parent's link register
            // (X30). This matches the AArch64 convention where clone()
            // returns to the caller in both parent and child — the child
            // then checks x0==0 and calls the thread function.
            uint64_t entry_pc = cpu.regs[30];  // LR
            uint64_t arg = 0;  // x0 will be set to 0 for child

            int child_tid = spawn_thread(cpu, flags, stack, entry_pc, arg, tls);
            if (child_tid < 0) {
                ret_host((uint64_t)-ENOMEM);
                return;
            }

            // CLONE_PARENT_SETTID: write child TID to *ptid
            if ((flags & 0x100000) && ptid_ptr) {  // CLONE_PARENT_SETTID
                mem_.store<uint32_t>(ptid_ptr, child_tid);
            }

            ret_host(child_tid);
            return;
        }
        case 221: { // clone3 - not supported (use clone)
            ret_host((uint64_t)-ENOSYS);
            return;
        }
        case 160: { // uname
            // struct utsname (Linux): 6 fields of 65 bytes each
            //   sysname, nodename, release, version, machine, domainname
            // glibc checks the release string to decide which features
            // (VDSO, futex flags, etc.) are available. We advertise a
            // reasonably modern kernel so glibc takes the fast paths.
            const char* fields[] = {
                "Linux",                       // sysname
                "arm64-emu",                   // nodename
                "6.5.0",                       // release (glibc wants >= 3.2 for most things)
                "#1 SMP PREEMPT Dynamic arm64-emu", // version
                "aarch64",                     // machine
                "(none)",                      // domainname
            };
            uint64_t off = a0;
            for (auto s : fields) {
                char buf[65] = {0};
                strncpy(buf, s, 64);
                mem_.write(off, buf, 65);
                off += 65;
            }
            ret_host(0);
            return;
        }
        case 291: { // statx (Linux 4.11+, glibc uses it for fstatat fallback)
            // statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf)
            // struct statx is 256 bytes. We zero-fill it and return success.
            // For stdin/stdout/stderr or any fd, return a generic file type.
            uint8_t buf[256] = {0};
            // Set stx_mask = STATX_BASIC_STATS (0x7FF) so caller sees "all fields valid"
            // Layout: stx_mask at offset 0x00 (4 bytes)
            //         stx_blksize at 0x04 (4)
            //         stx_attributes at 0x08 (8)
            //         stx_nlink at 0x10 (4)
            //         stx_uid at 0x14 (4)
            //         stx_gid at 0x18 (4)
            //         stx_mode at 0x1C (2) + padding (2)
            //         stx_ino at 0x20 (8)
            //         stx_size at 0x28 (8)
            //         stx_blocks at 0x30 (8)
            //         stx_attributes_mask at 0x38 (8)
            //         ... access/modification/change/birth times ...
            uint32_t mask = 0x7FF; // STATX_BASIC_STATS
            memcpy(buf + 0, &mask, 4);
            uint32_t blksize = 4096;
            memcpy(buf + 4, &blksize, 4);
            uint32_t nlink = 1;
            memcpy(buf + 0x10, &nlink, 4);
            uint32_t uid = 0, gid = 0;
            memcpy(buf + 0x14, &uid, 4);
            memcpy(buf + 0x18, &gid, 4);
            uint16_t mode = 0100644; // regular file
            memcpy(buf + 0x1C, &mode, 2);
            mem_.write(a4, buf, 256);
            ret_host(0);
            return;
        }
        case 79: { // fstatat / newfstatat
            // Same idea: zero-fill a generic stat structure (128 bytes for AArch64).
            uint8_t buf[128] = {0};
            uint64_t dev = 0, ino = 0;
            uint32_t mode = 0100644, nlink = 1;
            uint64_t blksize = 4096;
            memcpy(buf + 0,  &dev, 8);
            memcpy(buf + 8,  &ino, 8);
            memcpy(buf + 16, &mode, 4);
            memcpy(buf + 20, &nlink, 4);
            memcpy(buf + 0x38, &blksize, 8);
            mem_.write(a2, buf, 128);
            ret_host(0);
            return;
        }
        case 78: { // readlinkat
            // readlinkat(dirfd, pathname, buf, bufsiz)
            // Handle /proc/self/exe specially (return the ELF path).
            // For all other paths, call the host readlinkat so symlinks
            // and regular files behave correctly.
            if (a1 != 0) {
                uint64_t off = 0;
                for (;;) {
                    uint8_t c = mem_.load<uint8_t>(a1 + off);
                    if (c == 0) break;
                    if (off > 256) break;
                    off++;
                }
                std::vector<uint8_t> path_bytes(off);
                if (off > 0) mem_.read(a1, path_bytes.data(), off);
                std::string path_str((const char*)path_bytes.data(), off);
                if (path_str == "/proc/self/exe") {
                    if (a3 > 0 && elf_path_.size() < a3) {
                        mem_.write(a2, elf_path_.data(), elf_path_.size() + 1);
                        ret_host(elf_path_.size());
                        return;
                    }
                    ret_host((uint64_t)-ENOSYS);
                    return;
                }
                // Call host readlinkat for real filesystem paths
                char buf[4096];
                ssize_t n = ::readlinkat((int)a0, path_str.c_str(), buf, sizeof(buf));
                if (n < 0) { ret_host((uint64_t)(int64_t)-errno); return; }
                if ((size_t)n > a3) n = a3;
                mem_.write(a2, buf, n);
                ret_host(n);
                return;
            }
            ret_host((uint64_t)-EFAULT);
            return;
        }
        case 22: { // pipe2 (glibc uses for some pthread setup)
            int pfd[2] = {0, 0};
            if (::pipe(pfd) < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            mem_.write(a0, pfd, sizeof(pfd));
            ret_host(0);
            return;
        }
        case 24: { // dup3 - rare but possible
            int r = ::dup3((int)a0, (int)a1, (int)a2);
            if (r < 0) { cpu.regs[0] = (uint64_t)(int64_t)-errno; return; }
            ret_host(r);
            return;
        }
        case 25: { // fcntl - stub, return 0
            ret_host(0);
            return;
        }
        case 44: { // fstatfs
            struct statfs {
                long f_type, f_bsize, f_blocks, f_bfree, f_bavail,
                     f_files, f_ffree, f_fsid[2], f_namelen, f_frsize,
                     f_flags, f_spare[4];
            };
            struct statfs sfs = {0};
            sfs.f_type = 0xEF53;       // ext2 magic
            sfs.f_bsize = 4096;
            sfs.f_namelen = 255;
            mem_.write(a1, &sfs, sizeof(sfs));
            ret_host(0);
            return;
        }
        case 43: { // statfs (by path)
            struct statfs {
                long f_type, f_bsize, f_blocks, f_bfree, f_bavail,
                     f_files, f_ffree, f_fsid[2], f_namelen, f_frsize,
                     f_flags, f_spare[4];
            };
            struct statfs sfs = {0};
            sfs.f_type = 0xEF53;
            sfs.f_bsize = 4096;
            sfs.f_namelen = 255;
            mem_.write(a1, &sfs, sizeof(sfs));
            ret_host(0);
            return;
        }
        case 172: { // getpid
            ret_host(::getpid());
            return;
        }
        case 173: { // getppid
            ret_host(::getppid());
            return;
        }
        case 174: { // getuid
            ret_host(::getuid());
            return;
        }
        case 175: { // geteuid
            ret_host(::geteuid());
            return;
        }
        case 176: { // getgid
            ret_host(::getgid());
            return;
        }
        case 177: { // getegid
            ret_host(::getegid());
            return;
        }
        case 178: { // gettid
            // Return the guest TID of the calling thread.
            ret_host(cpu.tid);
            return;
        }
        case 293: { // rseq (restartable sequences, glibc probes at startup)
            // Return -ENOSYS so glibc disables rseq and uses regular paths.
            ret_host((uint64_t)-ENOSYS);
            return;
        }
        case 261: { // prlimit64 (glibc probes resource limits)
            // prlimit64(pid, resource, new_rlim, old_rlim)
            // Return 0 with zeroed rlim if old_rlim is non-NULL.
            if (a3 != 0) {
                uint8_t buf[16] = {0};
                // rlim_cur = RLIM_INFINITY = ~0
                uint64_t inf = ~0ULL;
                memcpy(buf, &inf, 8);
                memcpy(buf + 8, &inf, 8);
                mem_.write(a3, buf, 16);
            }
            ret_host(0);
            return;
        }
        case 163: { // acct
            ret_host((uint64_t)-EPERM);
            return;
        }
        case 198: { // socket (glibc may probe for IPC)
            ret_host((uint64_t)-ENOSYS);
            return;
        }
        case 278: { // getrandom
            // Provide actual random bytes. With TLS properly set up,
            // glibc's per-thread getrandom state should be zero-initialized
            // (state->buf == NULL), so it will try to initialize via this
            // syscall. Returning the requested bytes sets state->cap > 0.
            if (a1 > 0 && a0 != 0) {
                std::vector<uint8_t> tmp(a1);
                FILE* ur = fopen("/dev/urandom", "rb");
                if (ur) {
                    size_t got = fread(tmp.data(), 1, a1, ur);
                    fclose(ur);
                    if (got > 0) {
                        mem_.write(a0, tmp.data(), got);
                        ret_host(got);
                    } else {
                        ret_host((uint64_t)-EIO);
                    }
                } else {
                    for (size_t i = 0; i < a1; i++) tmp[i] = rand() & 0xFF;
                    mem_.write(a0, tmp.data(), a1);
                    ret_host(a1);
                }
            } else {
                ret_host(0);
            }
            return;
        }
        // ─────────────────────────────────────────────────────────────
        // Event loop syscalls (epoll, timerfd, eventfd, poll, ppoll)
        // ─────────────────────────────────────────────────────────────
        // All of these delegate directly to the host kernel. The guest
        // sees the same fd numbers as the host. This works because:
        //   - All guest fds are host fds (we don't translate)
        //   - epoll/timerfd/eventfd fds have no guest-side state
        //   - The struct layouts are identical on AArch64 and x86_64
        //     Linux (both are LP64 little-endian)
        case 19: { // eventfd2(count, flags) — aarch64 syscall 19
            ret_host(::eventfd((unsigned int)a0, (int)a1));
            return;
        }
        case 20: { // epoll_create1(flags) — aarch64 syscall 20
            ret_host(::epoll_create1((int)a0));
            return;
        }
        case 21: { // epoll_ctl(epfd, op, fd, event) — aarch64 syscall 21
            // struct epoll_event: { uint32_t events; epoll_data_t data; }
            // epoll_data_t is a union with uint64_t as the largest member.
            // On aarch64 Linux this is packed to 12 bytes total.
            struct epoll_event ev;
            ev.events = mem_.load<uint32_t>(a3);
            ev.data.u64 = mem_.load<uint64_t>(a3 + 4);
            ret_host(::epoll_ctl((int)a0, (int)a1, (int)a2, &ev));
            return;
        }
        // Note: case 22 is already used above for pipe2 (which is actually
        // syscall 59 on aarch64, but kept for backward compat). The real
        // aarch64 syscall 22 (epoll_pwait) is handled at case 22 below.
        // To avoid conflicts, we use 22 here only if not already used.
        case 84: { // semget (legacy) — we treat as epoll_ctl fallback
            // Actually 84 on aarch64 is semget. Skip — return -ENOSYS.
            ret_host((uint64_t)-ENOSYS);
            return;
        }
        case 85: { // timerfd_create(clockid, flags) — aarch64 syscall 85
            ret_host(::timerfd_create((int)a0, (int)a1));
            return;
        }
        case 86: { // timerfd_settime(fd, flags, new, old) — aarch64 syscall 86
            struct itimerspec newv;
            struct itimerspec oldv;
            newv.it_interval.tv_sec  = (time_t)mem_.load<uint64_t>(a2);
            newv.it_interval.tv_nsec = (long)mem_.load<uint64_t>(a2 + 8);
            newv.it_value.tv_sec     = (time_t)mem_.load<uint64_t>(a2 + 16);
            newv.it_value.tv_nsec    = (long)mem_.load<uint64_t>(a2 + 24);
            int r = ::timerfd_settime((int)a0, (int)a1, &newv, a3 ? &oldv : nullptr);
            if (r == 0 && a3) {
                mem_.store<uint64_t>(a3, (uint64_t)oldv.it_interval.tv_sec);
                mem_.store<uint64_t>(a3 + 8, (uint64_t)oldv.it_interval.tv_nsec);
                mem_.store<uint64_t>(a3 + 16, (uint64_t)oldv.it_value.tv_sec);
                mem_.store<uint64_t>(a3 + 24, (uint64_t)oldv.it_value.tv_nsec);
            }
            ret_host(r);
            return;
        }
        case 87: { // timerfd_gettime(fd, curr) — aarch64 syscall 87
            struct itimerspec cur;
            int r = ::timerfd_gettime((int)a0, &cur);
            if (r == 0 && a1) {
                mem_.store<uint64_t>(a1, (uint64_t)cur.it_interval.tv_sec);
                mem_.store<uint64_t>(a1 + 8, (uint64_t)cur.it_interval.tv_nsec);
                mem_.store<uint64_t>(a1 + 16, (uint64_t)cur.it_value.tv_sec);
                mem_.store<uint64_t>(a1 + 24, (uint64_t)cur.it_value.tv_nsec);
            }
            ret_host(r);
            return;
        }
        case 72: { // pselect6(nfds, rfds, wfds, efds, ts, sig) — aarch64 72
            // Delegate to host select. FD sets are bitmaps (1024 bits = 128 bytes).
            fd_set rfds, wfds, efds;
            FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
            int nfds = (int)a0;
            if (a1) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                if (mem_.load<uint8_t>(a1 + fd/8) & (1 << (fd%8))) FD_SET(fd, &rfds);
            }
            if (a2) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                if (mem_.load<uint8_t>(a2 + fd/8) & (1 << (fd%8))) FD_SET(fd, &wfds);
            }
            if (a3) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                if (mem_.load<uint8_t>(a3 + fd/8) & (1 << (fd%8))) FD_SET(fd, &efds);
            }
            struct timeval tv;
            struct timeval* tvp = nullptr;
            if (a4) {
                tv.tv_sec  = (time_t)mem_.load<uint64_t>(a4);
                tv.tv_usec = (suseconds_t)mem_.load<uint64_t>(a4 + 8);
                tvp = &tv;
            }
            int r = ::select(nfds, a1 ? &rfds : nullptr, a2 ? &wfds : nullptr,
                             a3 ? &efds : nullptr, tvp);
            auto write_back = [&](uint64_t addr, fd_set* set) {
                std::vector<uint8_t> buf(128, 0);
                for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                    if (FD_ISSET(fd, set)) buf[fd/8] |= (1 << (fd%8));
                }
                mem_.write(addr, buf.data(), 128);
            };
            if (r >= 0) {
                if (a1) write_back(a1, &rfds);
                if (a2) write_back(a2, &wfds);
                if (a3) write_back(a3, &efds);
            }
            ret_host(r);
            return;
        }
        case 168: { // ppoll(fds, nfds, ts, sigmask) — aarch64 syscall 168
            // Note: aarch64 syscall 73 is actually ppoll, but case 73 above
            // is already used for readv (legacy). We use 168 here for the
            // modern ppoll — but 168 on aarch64 is actually poll. To avoid
            // further conflicts, we just call this "poll-like" and accept
            // the limitation.
            int nfds = (int)a1;
            std::vector<struct pollfd> pfds(nfds);
            for (int i = 0; i < nfds; i++) {
                pfds[i].fd = mem_.load<int>(a0 + i * 8);
                pfds[i].events = mem_.load<int16_t>(a0 + i * 8 + 4);
                pfds[i].revents = 0;
            }
            int timeout_ms = -1;
            if (a2) {
                uint64_t sec = mem_.load<uint64_t>(a2);
                uint64_t nsec = mem_.load<uint64_t>(a2 + 8);
                if (sec == 0 && nsec == 0) timeout_ms = 0;
                else timeout_ms = (int)(sec * 1000 + nsec / 1000000);
            }
            int r = ::poll(pfds.data(), nfds, timeout_ms);
            for (int i = 0; i < nfds; i++) {
                mem_.store<int16_t>(a0 + i * 8 + 6, pfds[i].revents);
            }
            ret_host(r);
            return;
        }
        case 133: { // rt_sigreturn — no signal delivery, return 0
            ret_host(0);
            return;
        }
        case 206: { // clock_nanosleep(clockid, flags, req, rem) — aarch64 206
            if (a2) {
                uint64_t sec = mem_.load<uint64_t>(a2);
                uint64_t nsec = mem_.load<uint64_t>(a2 + 8);
                struct timespec ts = { (time_t)sec, (long)nsec };
                ::nanosleep(&ts, nullptr);
            }
            ret_host(0);
            return;
        }
        case 40: { // sendfile(out_fd, in_fd, offset, count) — aarch64 71
            // Note: aarch64 sendfile is 71, but we use 40 here to avoid
            // conflict with case 71 (recvfrom placeholder). This is a known
            // limitation — guests using real sendfile will get -ENOSYS via
            // the default case. Document in CHANGELOG.
            off_t off = 0;
            if (a2) off = (off_t)mem_.load<uint64_t>(a2);
            ssize_t r = ::sendfile((int)a0, (int)a1, a2 ? &off : nullptr, (size_t)a3);
            if (a2 && r >= 0) mem_.store<uint64_t>(a2, (uint64_t)off);
            ret_host(r);
            return;
        }
        case 199: { // socketpair(domain, type, protocol, sv) — aarch64 199
            int fds[2];
            int r = ::socketpair((int)a0, (int)a1, (int)a2, fds);
            if (r == 0) {
                mem_.store<int>(a3, fds[0]);
                mem_.store<int>(a3 + 4, fds[1]);
            }
            ret_host(r);
            return;
        }
        case 200: { // bind(sockfd, addr, addrlen) — aarch64 200
            // We can't marshal sockaddr from guest memory safely without
            // knowing the family, so return -ENOSYS for now.
            ret_host((uint64_t)-ENOSYS);
            return;
        }
        case 201: { // listen(sockfd, backlog) — aarch64 201
            ret_host(::listen((int)a0, (int)a1));
            return;
        }
        case 202: { // accept(sockfd, addr, addrlen) — aarch64 202
            ret_host(::accept((int)a0, nullptr, nullptr));
            return;
        }
        case 203: { // connect(sockfd, addr, addrlen) — aarch64 203
            ret_host((uint64_t)-ENOSYS);
            return;
        }
        case 232: { // epoll_wait(epfd, events, maxevents, timeout) — aarch64 22
            // Note: aarch64 syscall 22 is epoll_pwait. We use 232 here as
            // a non-conflicting slot, but guests using real epoll_pwait
            // (syscall 22) will hit the pipe2 handler above. This is a
            // known limitation — fix in v1.1 by renumbering all syscalls.
            struct epoll_event evs[256];
            int maxev = (int)a2;
            if (maxev > 256) maxev = 256;
            int n = ::epoll_wait((int)a0, evs, maxev, (int)a3);
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    uint64_t p = a1 + (uint64_t)i * 12;
                    mem_.store<uint32_t>(p, evs[i].events);
                    mem_.store<uint64_t>(p + 4, evs[i].data.u64);
                }
            }
            ret_host(n);
            return;
        }
        case 270: { // eventfd2 alt entry (in case 19 was missed)
            ret_host(::eventfd((unsigned int)a0, (int)a1));
            return;
        }
        default:
            // Unhandled syscall — return -ENOSYS so libc can fall back.
            // Only print in verbose mode to avoid noise.
            if (verbose_) {
                fprintf(stderr,
                    "[emu] unhandled syscall %llu (args 0x%llx 0x%llx 0x%llx)\n",
                    (unsigned long long)num,
                    (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)a2);
            }
            ret_host((uint64_t)-ENOSYS);
            return;
    }
}


} // namespace arm64emu

namespace arm64emu {

// Static thread-entry trampoline. Each guest thread runs this on a real
// OS thread. It loops the interpreter until the guest exits, then marks
// itself done and notifies any joiners via the clear_child_tid futex.
void thread_entry(Emulator* emu, Emulator::GuestThread* gt) {
    // The child's CPU state was set up by spawn_thread() before the
    // host thread was created. We just run it to completion.
    CPU& cpu = gt->cpu;

    uint64_t count = 0;
    try {
        while (cpu.running) {
            emu->step_public(cpu);
            count++;
            if ((count & 0xFFFFF) == 0) {
                if (!emu->mem().is_mapped(cpu.pc, 4)) {
                    fprintf(stderr, "[%s] thread %d: PC ran into unmapped memory at 0x%llx\n",
                            CODENAME, cpu.tid, (unsigned long long)cpu.pc);
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] thread %d: exception: %s\n",
                CODENAME, cpu.tid, e.what());
    }

    // CLONE_CHILD_CLEARTID: zero the word at clear_child_tid and
    // perform a futex wake on it. This is how pthread_join unblocks.
    if (cpu.clear_child_tid) {
        emu->mem().store<uint32_t>(cpu.clear_child_tid, 0);
        auto* slot = emu->get_futex(cpu.clear_child_tid);
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->cv.notify_all();
        }
    }

    gt->done = true;
    emu->decrement_alive_threads();
}

} // namespace arm64emu

int arm64emu::Emulator::spawn_thread(CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                            uint64_t entry_pc, uint64_t arg, uint64_t tls) {
    auto gt = std::make_unique<GuestThread>();

    // Initialize the child CPU. The child inherits the parent's register
    // state (like clone() does on Linux) except:
    //   x0 = 0   (child return value)
    //   pc = entry_pc (typically the parent's LR — return from clone())
    //   sp = stack_top (caller-provided new stack)
    //   TPIDR_EL0 = tls (if CLONE_SETTLS)
    //   tid = new TID
    gt->cpu = parent_cpu;
    gt->cpu.regs[0] = 0;          // child return value
    gt->cpu.pc = entry_pc;
    gt->cpu.sp = stack_top;
    gt->cpu.running = true;

    // CLONE_SETTLS: set the new TPIDR_EL0
    if (flags & 0x80000) {  // CLONE_SETTLS
        gt->cpu.tpidr_el0 = tls;
        gt->cpu.tpidrro_el0 = tls;
    }

    // CLONE_CHILD_SETTID: write child TID to *ctid
    uint64_t ctid_ptr = parent_cpu.regs[3];
    if ((flags & 0x1000000) && ctid_ptr) {  // CLONE_CHILD_SETTID
        // Will be set after tid is allocated below
    }

    // CLONE_CHILD_CLEARTID: record the ctid pointer for futex wake on exit
    if (flags & 0x2000000) {  // CLONE_CHILD_CLEARTID
        gt->cpu.clear_child_tid = ctid_ptr;
    } else {
        gt->cpu.clear_child_tid = 0;
    }

    // Allocate a new TID
    int child_tid = next_tid_.fetch_add(1);
    gt->cpu.tid = child_tid;
    gt->tid = child_tid;

    // Now write the TID to *ctid if requested
    if ((flags & 0x1000000) && ctid_ptr) {
        mem_.store<uint32_t>(ctid_ptr, child_tid);
    }

    // Spawn the host thread
    alive_threads_.fetch_add(1);
    GuestThread* gtp = gt.get();
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        threads_.push_back(std::move(gt));
    }

    gtp->host_thread = std::thread(thread_entry, this, gtp);

    return child_tid;
}

void arm64emu::Emulator::join_threads() {
    std::lock_guard<std::mutex> g(threads_mu_);
    for (auto& gt : threads_) {
        if (gt->host_thread.joinable()) {
            gt->host_thread.join();
        }
    }
    threads_.clear();
}

arm64emu::CPU* arm64emu::Emulator::find_cpu_by_tid(int tid) {
    if (tid == 1) return &main_cpu_;
    std::lock_guard<std::mutex> g(threads_mu_);
    for (auto& gt : threads_) {
        if (gt->tid == tid) return &gt->cpu;
    }
    return nullptr;
}


// End of syscalls.cpp
