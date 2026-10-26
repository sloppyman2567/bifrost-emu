/*
 * jit_window_bounds.c — guest accesses beyond the emulator's address window.
 *
 * The JIT bounds-checks every native load/store against the window size for
 * its width (now read from the window's header page). Addresses above any
 * window (the top page of the address space, and negative offsets that wrap
 * past zero) must raise SIGSEGV with si_addr at the access and resume
 * through the guest handler, for every width and both directions. QEMU 11.1
 * reports these addresses the same way. (Lower out-of-window addresses such
 * as 2^47 are not used: QEMU reports si_addr 0 for them.)
 *
 * The checks run again while a userfaultfd-registered page is missing: then
 * the header-page gate reads 0, every access takes the checked path, and that
 * path must still compare with the real window bound.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static sigjmp_buf env;
static volatile uintptr_t fault_addr;
static void on_segv(int sig, siginfo_t* info, void* ctx) {
    (void)sig; (void)ctx;
    fault_addr = (uintptr_t)info->si_addr;
    siglongjmp(env, 1);
}

static int checks, fails;
static void expect_fault(const char* name, int faulted, uintptr_t want) {
    ++checks;
    if (!faulted || fault_addr != want) {
        printf("FAIL %s faulted=%d si_addr=%#lx want=%#lx\n", name, faulted,
               (unsigned long)fault_addr, (unsigned long)want);
        ++fails;
    }
}

#define LOAD(name, ins, base, want) do { \
    fault_addr = 0; uint64_t out = 0; const uint64_t b = (base); \
    int faulted = sigsetjmp(env, 1); \
    if (!faulted) __asm__ volatile(ins : "=&r"(out) : "r"(b) : "memory"); \
    expect_fault(name, faulted, (want)); (void)out; } while (0)
#define STORE(name, ins, base, want) do { \
    fault_addr = 0; const uint64_t b = (base), val = 0x1122334455667788ull; \
    int faulted = sigsetjmp(env, 1); \
    if (!faulted) __asm__ volatile(ins : : "r"(val), "r"(b) : "memory"); \
    expect_fault(name, faulted, (want)); } while (0)

static void run_checks(void) {
    const uint64_t bases[] = {0xfffffffffffff000ull, 0xffffffffffffff00ull};
    for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
        const uint64_t a = bases[i];
        for (int round = 0; round < 3; ++round) {  // warm the translated block
            LOAD("ldrb", "ldrb %w0, [%1]", a, a);
            LOAD("ldrh", "ldrh %w0, [%1]", a, a);
            LOAD("ldr w", "ldr %w0, [%1]", a, a);
            LOAD("ldr x", "ldr %0, [%1]", a, a);
            LOAD("ldrsh x off", "ldrsh %0, [%1, #6]", a, a + 6);
            LOAD("ldr x off", "ldr %0, [%1, #16]", a, a + 16);
            STORE("strb", "strb %w0, [%1]", a, a);
            STORE("strh", "strh %w0, [%1]", a, a);
            STORE("str w", "str %w0, [%1]", a, a);
            STORE("str x", "str %0, [%1]", a, a);
            STORE("str x off", "str %0, [%1, #24]", a, a + 24);
        }
    }
    /* A negative offset from zero wraps to the top of the address space. */
    for (int round = 0; round < 3; ++round) {
        LOAD("ldur x wrap", "ldur %0, [%1, #-8]", 0, 0xfffffffffffffff8ull);
        STORE("stur w wrap", "stur %w0, [%1, #-4]", 0, 0xfffffffffffffffcull);
    }
}

// A registered page left missing for the second pass; 0 when unavailable.
static int missing_page(void) {
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | 1);
    if (fd < 0) fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC);
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_SIGBUS};
    if (fd < 0 || ioctl(fd, UFFDIO_API, &api)) return 0;
    char* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || madvise(page, 4096, MADV_DONTNEED)) return 0;
    struct uffdio_register reg = {.range = {(uintptr_t)page, 4096}, .mode = UFFDIO_REGISTER_MODE_MISSING};
    return ioctl(fd, UFFDIO_REGISTER, &reg) == 0;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    run_checks();
    if (missing_page()) run_checks();
    else printf("jit_window_bounds: no userfaultfd, second pass skipped\n");
    if (fails) { printf("jit_window_bounds: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_window_bounds: %d checks passed\n", checks);
    return 0;
}
