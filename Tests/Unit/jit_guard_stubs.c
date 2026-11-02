// jit_guard_stubs.c — software page checks and fault exits with the gate closed.
//
// Tier-1 keeps one copy per JIT of the page checks and the recoverable fault
// exit. They run only while the bounds gate is closed, which a registered,
// missing userfaultfd page does for every access in the process. This drives
// each width and direction, in and out of a page crossing, with the address in
// different host registers (the live values before an access move it), and
// checks results, fault kinds and the state a guest handler sees.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { PAGE = 4096, LEVELS = 8, WIDTHS = 4, REPEATS = 8 };
static const int widths[WIDTHS] = {1, 2, 4, 8};
static const uint64_t PATTERN = 0x8877665544332211ull;

// Each kind of access at each pressure level is its own guest code, so each
// compiles its own guard. x10.. are live before the access; x9 is the base.
#define LIVE0 ""
#define LIVE1 LIVE0 "mov x10,#0x11\n eor x10,x10,x9\n"
#define LIVE2 LIVE1 "mov x11,#0x22\n eor x11,x11,x9\n mov x12,#0x33\n eor x12,x12,x9\n"
#define LIVE3 LIVE2 "mov x13,#0x44\n eor x13,x13,x9\n mov x14,#0x55\n eor x14,x14,x9\n"
#define LIVE4 LIVE3 "mov x15,#0x66\n eor x15,x15,x9\n mov x16,#0x77\n eor x16,x16,x9\n"
#define LIVE5 LIVE4 "mov x17,#0x88\n eor x17,x17,x9\n mov x19,#0x99\n eor x19,x19,x9\n"
#define LIVE6 LIVE5 "mov x20,#0xaa\n eor x20,x20,x9\n mov x21,#0xbb\n eor x21,x21,x9\n"
#define LIVE7 LIVE6 "mov x22,#0xcc\n eor x22,x22,x9\n mov x23,#0xdd\n eor x23,x23,x9\n"
#define CLOBBERS "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","x19","x20","x21","x22","x23","memory"

#define OP(live, insn) \
    __asm__ volatile("mov x9,%[a]\n mov x8,%[v]\n" live insn "\n mov %[r],x8\n" \
                     : [r] "=&r"(value) : [a] "r"(address), [v] "r"(value_in) : CLOBBERS)
#define LEVEL(live) \
    switch (kind) { \
    case 0: OP(live, "ldrb w8,[x9]"); break; \
    case 1: OP(live, "ldrh w8,[x9]"); break; \
    case 2: OP(live, "ldr w8,[x9]"); break; \
    case 3: OP(live, "ldr x8,[x9]"); break; \
    case 4: OP(live, "strb w8,[x9]"); break; \
    case 5: OP(live, "strh w8,[x9]"); break; \
    case 6: OP(live, "str w8,[x9]"); break; \
    case 7: OP(live, "str x8,[x9]"); break; \
    case 8: OP(live, "ldurb w8,[x9,#1]"); break; \
    case 9: OP(live, "ldurh w8,[x9,#1]"); break; \
    case 10: OP(live, "ldur w8,[x9,#1]"); break; \
    case 11: OP(live, "ldur x8,[x9,#1]"); break; \
    case 12: OP(live, "sturb w8,[x9,#1]"); break; \
    case 13: OP(live, "sturh w8,[x9,#1]"); break; \
    case 14: OP(live, "stur w8,[x9,#1]"); break; \
    case 15: OP(live, "stur x8,[x9,#1]"); break; \
    }

__attribute__((noinline)) static uint64_t access_at(int level, int kind, uint64_t address, uint64_t value_in) {
    uint64_t value = 0;
    switch (level) {
    case 0: LEVEL(LIVE0) break;
    case 1: LEVEL(LIVE1) break;
    case 2: LEVEL(LIVE2) break;
    case 3: LEVEL(LIVE3) break;
    case 4: LEVEL(LIVE4) break;
    case 5: LEVEL(LIVE5) break;
    case 6: LEVEL(LIVE6) break;
    case 7: LEVEL(LIVE7) break;
    }
    return value;
}

static sigjmp_buf resume;
static volatile int caught_signo, caught_code;
static volatile uint64_t caught_addr, caught_x9, caught_x10, caught_pc;

static void on_fault(int signo, siginfo_t *info, void *raw) {
    const ucontext_t *uc = raw;
    caught_signo = signo;
    caught_code = info->si_code;
    caught_addr = (uint64_t)info->si_addr;
    caught_x9 = uc->uc_mcontext.regs[9];
    caught_x10 = uc->uc_mcontext.regs[10];
    caught_pc = uc->uc_mcontext.pc;
    siglongjmp(resume, 1);
}

static inline uint64_t low_bytes(uint64_t v, int width) {
    return width == 8 ? v : v & ((1ull << (width * 8)) - 1);
}

// A load returns the pattern and a store writes it and nothing next to it, for
// every level, width and direction, at offset 0 and 1 of the given address.
static void check_values(char *base, const char *what) {
    for (int level = 0; level < LEVELS; level++) {
        for (int w = 0; w < WIDTHS; w++) {
            const int width = widths[w];
            for (int offset = 0; offset < 2; offset++) {
                // The access starts `start` bytes before the second page, so widths
                // above one cross it; a byte access stays inside the first page.
                const size_t start = PAGE - (width > 1 ? width / 2 : 1);
                char *at = base + start - offset;
                const int load = w + (offset ? 8 : 0), store = load + 4;
                for (int repeat = 0; repeat < REPEATS; repeat++) {
                    memcpy(base + start, &PATTERN, (size_t)width);
                    uint64_t got = access_at(level, load, (uint64_t)at, 0);
                    if (got != low_bytes(PATTERN, width)) {
                        printf("%s: level %d width %d offset %d load %llx\n", what, level, width, offset,
                               (unsigned long long)got);
                        failures++;
                    }
                    memset(base + start - 1, 0xAA, (size_t)width + 2);
                    access_at(level, store, (uint64_t)at, PATTERN);
                    uint64_t stored = 0;
                    memcpy(&stored, base + start, (size_t)width);
                    if (stored != low_bytes(PATTERN, width) ||
                        (unsigned char)base[start - 1] != 0xAA ||
                        (unsigned char)base[start + width] != 0xAA) {
                        printf("%s: level %d width %d offset %d store %llx\n", what, level, width, offset,
                               (unsigned long long)stored);
                        failures++;
                    }
                    checks += 2;
                }
            }
        }
    }
}

// One access that must fault: the kind and the address, and a pre-instruction
// register state where the access left it. Levels with x10 live check its value.
static void expect_fault(int level, int kind, uint64_t address, int signo, int code, uint64_t fault_addr) {
    caught_signo = 0;
    if (sigsetjmp(resume, 1) == 0) {
        access_at(level, kind, address, PATTERN);
        printf("no fault: level %d kind %d address %llx\n", level, kind, (unsigned long long)address);
        failures++;
        return;
    }
    checks += 3;
    if (caught_signo != signo || caught_code != code || caught_addr != fault_addr) {
        printf("level %d kind %d address %llx: signal %d code %d addr %llx, expected %d %d %llx\n", level, kind,
               (unsigned long long)address, caught_signo, caught_code, (unsigned long long)caught_addr, signo, code,
               (unsigned long long)fault_addr);
        failures++;
    }
    if (caught_x9 != address) { printf("level %d kind %d: x9 %llx\n", level, kind, (unsigned long long)caught_x9); failures++; }
    if (level > 0 && caught_x10 != (0x11 ^ address)) {
        printf("level %d kind %d: x10 %llx\n", level, kind, (unsigned long long)caught_x10);
        failures++;
    }
    CHECK(caught_pc >= (uint64_t)access_at - 0x10000 && caught_pc < (uint64_t)access_at + 0x10000);
}

// A compare, CSEL's scratch result forwarded past enough live values to evict
// it from its register, then used: the value must survive in its spill slot.
__attribute__((noinline)) static uint64_t csel_under_pressure(uint64_t a, uint64_t b, uint64_t seed) {
    uint64_t r;
    __asm__ volatile(
        "cmp %[a],%[b]\n"
        "csel x0,%[a],%[b],lo\n"
        "eor x1,%[s],#1\n eor x2,x1,#2\n eor x3,x2,#4\n eor x4,x3,#8\n eor x5,x4,#16\n eor x6,x5,#32\n"
        "eor x7,x6,#64\n eor x8,x7,#128\n eor x9,x8,#256\n eor x10,x9,#512\n eor x11,x10,#1024\n"
        "add x12,x1,x2\n add x12,x12,x3\n add x12,x12,x4\n add x12,x12,x5\n add x12,x12,x6\n"
        "add x12,x12,x7\n add x12,x12,x8\n add x12,x12,x9\n add x12,x12,x10\n add x12,x12,x11\n"
        "add %[r],x12,x0\n"
        : [r] "=&r"(r) : [a] "r"(a), [b] "r"(b), [s] "r"(seed)
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","cc");
    return r;
}

static uint64_t csel_expected(uint64_t a, uint64_t b, uint64_t s) {
    uint64_t v = s ^ 1, sum = v;
    for (int bit = 1; bit <= 10; bit++) { v ^= 1ull << bit; sum += v; }
    return sum + (a < b ? a : b);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    // [RW][RW][none][read-only][unmapped]
    char *base = mmap(NULL, 5 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED);
    memset(base, 0, 4 * PAGE);
    CHECK(mprotect(base + 2 * PAGE, PAGE, PROT_NONE) == 0);
    CHECK(mprotect(base + 3 * PAGE, PAGE, PROT_READ) == 0);
    CHECK(munmap(base + 4 * PAGE, PAGE) == 0);

    check_values(base, "gate open");

    // A registered page that stays missing closes the gate for the whole test.
    int uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
    if (uffd < 0 && errno == EINVAL) uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC);
    char *missing = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(missing != MAP_FAILED);
    // Discarded before registering, as ART does (a page never touched stays
    // missing under the kernel either way).
    CHECK(madvise(missing, 2 * PAGE, MADV_DONTNEED) == 0);
    int closed = 0;
    if (uffd >= 0) {
        struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_SIGBUS};
        struct uffdio_register reg = {.range = {.start = (uintptr_t)missing, .len = 2 * PAGE},
                                      .mode = UFFDIO_REGISTER_MODE_MISSING};
        closed = ioctl(uffd, UFFDIO_API, &api) == 0 && ioctl(uffd, UFFDIO_REGISTER, &reg) == 0;
    }
    if (!closed) printf("userfaultfd unavailable (%s): only the open gate ran\n", strerror(errno));

    check_values(base, closed ? "gate closed" : "gate open again");

    for (int level = 0; level < LEVELS; level++) {
        for (int w = 0; w < WIDTHS; w++) {
            const int width = widths[w];
            const uint64_t none = (uint64_t)base + 2 * PAGE, ro = (uint64_t)base + 3 * PAGE;
            const uint64_t unmapped = (uint64_t)base + 4 * PAGE;
            // No access, wrong direction, nothing mapped, a crossing into no access.
            expect_fault(level, w, none, SIGSEGV, SEGV_ACCERR, none);
            expect_fault(level, w + 4, none, SIGSEGV, SEGV_ACCERR, none);
            expect_fault(level, w + 4, ro, SIGSEGV, SEGV_ACCERR, ro);
            CHECK(access_at(level, w, ro, 0) == 0);
            CHECK(((unsigned char *)ro)[0] == 0);
            expect_fault(level, w, unmapped, SIGSEGV, SEGV_MAPERR, unmapped);
            expect_fault(level, w + 4, unmapped, SIGSEGV, SEGV_MAPERR, unmapped);
            if (width > 1) {
                const uint64_t crossing = none - (uint64_t)(width / 2);
                caught_signo = 0;
                if (sigsetjmp(resume, 1) == 0) {
                    access_at(level, w, crossing, 0);
                    printf("no fault: crossing load level %d width %d\n", level, width);
                    failures++;
                } else {
                    CHECK(caught_signo == SIGSEGV && caught_x9 == crossing);
                }
            }
            // Far outside the window: the real bound rejects it, not the page flags.
            const uint64_t far = 1ull << 50;
            expect_fault(level, w, far, SIGSEGV, SEGV_MAPERR, far);
            if (closed) expect_fault(level, w, (uint64_t)missing, SIGBUS, BUS_ADRERR, (uint64_t)missing);
        }
    }

    for (uint64_t i = 0; i < 64; i++) {
        const uint64_t a = i * 0x9E3779B97F4A7C15ull, b = (i ^ 21) * 0xC2B2AE3D27D4EB4Full, s = i * 77;
        CHECK(csel_under_pressure(a, b, s) == csel_expected(a, b, s));
    }

    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("jit_guard_stubs: %d checks, gate %s: ALL PASS\n", checks, closed ? "closed" : "open only");
    return 0;
}
