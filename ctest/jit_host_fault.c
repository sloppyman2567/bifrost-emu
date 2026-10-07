// Repeated faults at warmed native sites on four guest CPUs. Fixed register,
// NZCV and vector values are checked in/after the handler. QEMU is an oracle.
// Optional "bus" checks a shared file page beyond EOF (native JIT only).
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

static __thread uintptr_t expected_pc, expected_addr;
static __thread int expected_signal;
static __thread volatile sig_atomic_t faults, bad;
static long page_size;

static void handler(int signo, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (!expected_pc || signo != expected_signal) _exit(90);
    if ((uintptr_t)info->si_addr < expected_addr ||
        (uintptr_t)info->si_addr >= expected_addr + (uintptr_t)page_size ||
        info->si_code != (signo == SIGBUS ? BUS_ADRERR : SEGV_ACCERR) ||
        uc->uc_mcontext.pc != expected_pc ||
        uc->uc_mcontext.regs[19] != 0x1357 ||
        uc->uc_mcontext.regs[20] != 0x2468 ||
        uc->uc_mcontext.regs[11] != 0x999 ||
        (uc->uc_mcontext.pstate & 0xf0000000) != 0x80000000)
        ++bad;
    ++faults;
    uc->uc_mcontext.pc += 4; // following native IR must not execute handler state
}

#define PROBE(instruction, address) do { \
    uint64_t vector = 0, result = 0; \
    int before = faults; \
    __asm__ volatile( \
        "adr x9, 1f\n\tstr x9, [%1]\n\t" \
        "mov x19, #0x1357\n\tmov x20, #0x2468\n\t" \
        "fmov d8, x19\n\tmov x11, #0x999\n\tcmp x19, x20\n\t" \
        "1: " instruction "\n\t" \
        "fmov x9, d8\n\tstr x9, [%2]\n\tstr x11, [%3]" \
        : : "r"((uintptr_t)(address)), "r"(&expected_pc), \
            "r"(&vector), "r"(&result) \
        : "x9", "x11", "x19", "x20", "v0", "v8", "cc", "memory"); \
    if (faults != before + 1 || vector != 0x1357 || result != 0x999) ++bad; \
} while (0)

static void *worker(void *unused) {
    (void)unused;
    unsigned char *p = mmap(0, 2 * page_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || mprotect(p + page_size, page_size, PROT_NONE)) _exit(91);
    expected_signal = SIGSEGV;
    expected_addr = (uintptr_t)p + page_size;
    int lse = (getauxval(AT_HWCAP) & (1UL << 8)) != 0;
    for (int i = 0; i < 128; ++i) {
        PROBE("ldr x11, [%0]", p + page_size);
        PROBE("str x11, [%0]", p + page_size);
        PROBE("ldr q0, [%0]", p + page_size - 8);
        PROBE("str q0, [%0]", p + page_size - 8);
        if (lse) {
            PROBE(".arch_extension lse\n\tldadd x20, x11, [%0]", p + page_size);
            PROBE(".arch_extension lse\n\tldset x20, x11, [%0]", p + page_size);
            PROBE(".arch_extension lse\n\tcas x11, x20, [%0]", p + page_size);
        }
    }
    if (lse) {
        // The CAS-loop's first native read succeeds; its later cmpxchg
        // must recover a write fault without publishing the loaded old value.
        if (mprotect(p, page_size, PROT_READ)) _exit(92);
        expected_addr = (uintptr_t)p;
        PROBE(".arch_extension lse\n\tldset x20, x11, [%0]", p);
    }
    if (munmap(p, 2 * page_size)) ++bad;
    return (void *)(uintptr_t)(bad != 0);
}

static int bus_probe(void) {
    char path[] = "/tmp/bifrost-host-fault-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || unlink(path) || ftruncate(fd, page_size)) return 1;
    void *p = mmap(0, 2 * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return 1;
    close(fd);
    expected_signal = SIGBUS;
    expected_addr = (uintptr_t)p + page_size;
    PROBE("ldr x11, [%0]", (char *)p + page_size);
    PROBE("str x11, [%0]", (char *)p + page_size);
    PROBE("ldr q0, [%0]", (char *)p + page_size);
    PROBE("str q0, [%0]", (char *)p + page_size);
    int failed = bad || faults != 4;
    munmap(p, 2 * page_size);
    return failed;
}

int main(int argc, char **argv) {
    page_size = sysconf(_SC_PAGESIZE);
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    if (page_size <= 0 || sigaction(SIGSEGV, &sa, 0) || sigaction(SIGBUS, &sa, 0)) return 1;
    if (argc > 1 && !strcmp(argv[1], "bus")) {
        int failed = bus_probe();
        printf("jit_host_fault bus: %s\n", failed ? "FAIL" : "ALL PASS");
        return failed;
    }
    if (argc > 1 && !strcmp(argv[1], "single")) {
        int failed = worker(0) != 0;
        printf("jit_host_fault single CPU: %s\n", failed ? "FAIL" : "ALL PASS");
        return failed;
    }
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], 0, worker, 0)) return 1;
    int failed = 0;
    for (int i = 0; i < 4; ++i) {
        void *result;
        if (pthread_join(threads[i], &result) || result) failed = 1;
    }
    printf("jit_host_fault: 4 CPUs, 2048+ faults: %s\n", failed ? "FAIL" : "ALL PASS");
    return failed;
}
