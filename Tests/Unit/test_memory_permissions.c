// Linux AArch64 permission faults, syscall EFAULT, and brk regressions.
// These tests are ABI regressions, not full Arm architecture conformance tests.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
static volatile sig_atomic_t faults, bad_signals, probe_active;
static size_t page_size;
static int fault_limit;
static volatile uint64_t expected_pc, completed;
static int expected_code, checks, failures;
static uintptr_t expected_addr, expected_addr_end;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void fault_handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (!probe_active || ++faults > fault_limit) _exit(10);
    if (sig != SIGSEGV || info->si_code != expected_code ||
        (uintptr_t)info->si_addr < expected_addr ||
        (uintptr_t)info->si_addr >= expected_addr_end ||
        uc->uc_mcontext.pc != expected_pc || uc->uc_mcontext.regs[10] != 123)
        ++bad_signals;
    // Repair the access and return: retry must resume at the faulting PC
    // with the interrupted register state, then complete the instruction.
    void *page = (void *)(expected_addr & ~(uintptr_t)(page_size - 1));
    if (expected_code == SEGV_MAPERR) {
        if (mmap(page, page_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != page)
            _exit(2);
    } else if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0) _exit(3);
}
// The probe consumes the mapping as it stands: only the handler repairs it.
// Check the exact faulting PC and bound retries to avoid hanging on corruption.
// Cross-page FAR values are checked against the faulting page, rather than
// assuming how the CPU decomposes an unaligned access. No store atomicity
// is inferred from a faulting STP or unaligned STR.
#define PROBE_RANGE(instruction, ptr, code, addr, span) do { \
    expected_code = (code); expected_addr = (uintptr_t)(addr); \
    expected_addr_end = expected_addr + (span); \
    int before = faults; fault_limit = before + 1; \
    completed = 0; probe_active = 1; \
    __asm__ volatile("adr x9, 1f\n\tstr x9, [%1]\n\t" \
        "mov x10, #123\n\tmov x11, #90\n\tmov x12, #91\n\t" \
        "1: " instruction "\n\tmov x12, #456\n\tstr x12, [%2]" \
        : : "r"((uintptr_t)(ptr)), "r"(&expected_pc), "r"(&completed) \
        : "x9", "x10", "x11", "x12", "v0", "memory"); \
    probe_active = 0; \
    CHECK(faults == before + 1); CHECK(bad_signals == 0); CHECK(completed == 456); \
} while (0)
#define PROBE(instruction, ptr, code, addr) \
    PROBE_RANGE(instruction, ptr, code, addr, 1)
#define NONE_PROBE(instruction, ptr, addr, span) do { \
    CHECK(mprotect((void *)((uintptr_t)(addr) & ~(uintptr_t)(page_size - 1)), \
                   page_size, PROT_NONE) == 0); \
    PROBE_RANGE(instruction, ptr, SEGV_ACCERR, addr, span); \
} while (0)
int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    long size = sysconf(_SC_PAGESIZE);
    CHECK(size > 0 && (size & (size - 1)) == 0);
    if (size <= 0 || (size & (size - 1))) return 1;
    page_size = (size_t)size;
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, 0) == 0);
    char *p = mmap(0, (3 * page_size), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return 1;
    p[0] = 42;
    NONE_PROBE("ldrb w11, [%0]", p, p, 1);
    NONE_PROBE("strb w11, [%0]", p, p, 1);
    NONE_PROBE("ldr q0, [%0]", p, p, 1);
    NONE_PROBE("str q0, [%0]", p, p, 1);
    NONE_PROBE("ldxr x11, [%0]", p, p, 1);
    NONE_PROBE("stlr x11, [%0]", p, p, 1);
    if (getauxval(AT_HWCAP) & (1UL << 8)) { // HWCAP_ATOMICS / FEAT_LSE
        NONE_PROBE(".arch_extension lse\n\tldadd w11, w12, [%0]", p, p, 1);
    } else puts("LSE access: unavailable on this CPU");
    CHECK(mprotect(p, page_size, PROT_NONE) == 0);
    int before = faults;
    // A pipe must copy the user buffer; /dev/null need not access it at all.
    // Do not depend on whatever kind of descriptor stdout was redirected to.
    int fds[2];
    int pipe_result = pipe(fds);
    CHECK(pipe_result == 0);
    if (pipe_result == 0) {
        errno = 0;
        CHECK(syscall(SYS_write, fds[1], p, 1) == -1 && errno == EFAULT);
        CHECK(faults == before);
        CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
    }
    CHECK(mprotect(p, page_size, PROT_READ | PROT_WRITE) == 0);
    p[0] = 42;
    CHECK(p[0] == 42);
    NONE_PROBE("ldr x11, [%0]", p + page_size - 4, p + page_size, page_size);
    NONE_PROBE("str x11, [%0]", p + page_size - 4, p + page_size, page_size);
    NONE_PROBE("ldr q0, [%0]", p + page_size - 8, p + page_size, page_size);
    NONE_PROBE("stp x11, x12, [%0]", p + page_size - 8, p + page_size, page_size);
    // LDP without writeback may legally overwrite its own address register.
    expected_code = SEGV_ACCERR; expected_addr = (uintptr_t)p + page_size;
    CHECK(mprotect(p + page_size, page_size, PROT_NONE) == 0);
    before = faults; fault_limit = before + 1;
    expected_addr_end = expected_addr + page_size; probe_active = 1;
    register uintptr_t pair_addr __asm__("x13") = (uintptr_t)p + page_size - 8;
    __asm__ volatile("adr x9, 1f\n\tstr x9, [%1]\n\tmov x10, #123\n\t"
        "1: ldp x13, x11, [x13]"
        : "+r"(pair_addr) : "r"(&expected_pc) : "x9", "x10", "x11", "memory");
    probe_active = 0;
    CHECK(faults == before + 1 && bad_signals == 0);
    CHECK(munmap(p + page_size, page_size) == 0);
    PROBE("ldrb w11, [%0]", p + page_size, SEGV_MAPERR, p + page_size);
    CHECK(munmap(p + page_size, page_size) == 0);
    PROBE("strb w11, [%0]", p + page_size, SEGV_MAPERR, p + page_size);
    p[0] = 42;
    CHECK(mprotect(p, page_size, PROT_READ) == 0);
    CHECK(p[0] == 42);
    PROBE("strb w11, [%0]", p, SEGV_ACCERR, p);
    CHECK((unsigned char)p[0] == 90);
    // Prepare before fork. Child probes must not reapply protections, and
    // repairs/writes in the child must not alter the parent's VMAs or bytes.
    p[0] = 42;
    CHECK(mprotect(p, page_size, PROT_READ) == 0);
    CHECK(mprotect(p + page_size, page_size, PROT_NONE) == 0);
    CHECK(munmap(p + 2 * page_size, page_size) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(p[0] == 42);
        PROBE("strb w11, [%0]", p, SEGV_ACCERR, p);
        PROBE("ldrb w11, [%0]", p + page_size, SEGV_ACCERR, p + page_size);
        PROBE("ldrb w11, [%0]", p + 2 * page_size, SEGV_MAPERR, p + 2 * page_size);
        _exit(failures ? 1 : 0);
    }
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(p[0] == 42);
    PROBE("strb w11, [%0]", p, SEGV_ACCERR, p);
    PROBE("ldrb w11, [%0]", p + page_size, SEGV_ACCERR, p + page_size);
    PROBE("ldrb w11, [%0]", p + 2 * page_size, SEGV_MAPERR, p + 2 * page_size);
    CHECK(munmap(p, 3 * page_size) == 0);
    uintptr_t old_break = syscall(SYS_brk, 0);
    uintptr_t base = (old_break + page_size - 1) & ~(uintptr_t)(page_size - 1);
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    CHECK((uintptr_t)syscall(SYS_brk, base + (4 * page_size)) == base + (4 * page_size));
    *(volatile unsigned char *)(base + 2 * page_size) = 0x6a;
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    CHECK((uintptr_t)syscall(SYS_brk, base + (4 * page_size)) == base + (4 * page_size));
    CHECK(*(volatile unsigned char *)(base + 2 * page_size) == 0);
    *(volatile unsigned char *)(base + 2 * page_size) = 0x6b;
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    // Successful NOREPLACE mapping proves shrink released the middle page.
    void *middle = mmap((void *)(base + (2 * page_size)), page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(middle == (void *)(base + (2 * page_size)));
    if (middle != (void *)(base + (2 * page_size))) return 1;
    CHECK(*(volatile unsigned char *)middle == 0);
    *(volatile unsigned char *)middle = 0x3c;
    CHECK(mprotect(middle, page_size, PROT_READ) == 0);
    CHECK((uintptr_t)syscall(SYS_brk, base + (4 * page_size)) == base);
    CHECK((uintptr_t)syscall(SYS_brk, 0) == base);
    CHECK(*(volatile unsigned char *)middle == 0x3c);
    // Existing read-only protection must survive the rejected growth.
    PROBE("strb w11, [%0]", middle, SEGV_ACCERR, middle);
    CHECK(munmap(middle, page_size) == 0);
    CHECK((uintptr_t)syscall(SYS_brk, base + (4 * page_size)) == base + (4 * page_size));
    CHECK(*(volatile unsigned char *)(base + 2 * page_size) == 0);
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    printf("memory permissions: %d checks, %d failures\n%s\n", checks, failures,
           failures ? "FAIL" : "ALL PASS");
    return failures != 0;
}
