// Permission faults, syscall EFAULT, and brk collision/regrowth regressions.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
static volatile sig_atomic_t faults, bad_signals;
static int expected_code, checks, failures;
static uintptr_t expected_addr;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void fault_handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    ++faults;
    if (sig != SIGSEGV || info->si_code != expected_code ||
        (uintptr_t)info->si_addr != expected_addr || uc->uc_mcontext.regs[10] != 123)
        ++bad_signals;
    // Repair the access and return: retry must resume at the faulting PC
    // with the interrupted register state, then complete the instruction.
    void *page = (void *)(expected_addr & ~(uintptr_t)4095);
    if (expected_code == SEGV_MAPERR) {
        if (mmap(page, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != page)
            _exit(2);
    } else if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) _exit(3);
}
#define PROBE(instruction, ptr, code, addr) do { \
    expected_code = (code); expected_addr = (uintptr_t)(addr); \
    void *page = (void *)(expected_addr & ~(uintptr_t)4095); \
    if ((code) == SEGV_MAPERR) CHECK(munmap(page, 4096) == 0); \
    else CHECK(mprotect(page, 4096, PROT_NONE) == 0); \
    int before = faults; \
    __asm__ volatile("mov x10, #123\n\t" instruction "\n\tmov x12, #456" \
        : : "r"((uintptr_t)(ptr)) : "x10", "x11", "x12", "v0", "memory"); \
    CHECK(faults == before + 1); CHECK(bad_signals == 0); \
} while (0)
int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, 0) == 0);
    char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return 1;
    p[0] = 42;
    CHECK(mprotect(p, 4096, PROT_NONE) == 0);
    PROBE("ldrb w11, [%0]", p, SEGV_ACCERR, p);
    PROBE("strb w11, [%0]", p, SEGV_ACCERR, p);
    PROBE("ldr q0, [%0]", p, SEGV_ACCERR, p);
    PROBE("str q0, [%0]", p, SEGV_ACCERR, p);
    PROBE("ldxr x11, [%0]", p, SEGV_ACCERR, p);
    PROBE("stlr x11, [%0]", p, SEGV_ACCERR, p);
    PROBE(".arch_extension lse\n\tldadd w11, w12, [%0]", p, SEGV_ACCERR, p);
    CHECK(mprotect(p, 4096, PROT_NONE) == 0);
    int before = faults;
    errno = 0;
    CHECK(syscall(SYS_write, 1, p, 1) == -1 && errno == EFAULT);
    CHECK(faults == before);
    CHECK(mprotect(p, 4096, PROT_READ | PROT_WRITE) == 0);
    p[0] = 42;
    CHECK(p[0] == 42);
    CHECK(mprotect(p + 4096, 4096, PROT_NONE) == 0);
    PROBE("ldr x11, [%0]", p + 4092, SEGV_ACCERR, p + 4096);
    PROBE("str x11, [%0]", p + 4092, SEGV_ACCERR, p + 4096);
    PROBE("ldr q0, [%0]", p + 4088, SEGV_ACCERR, p + 4096);
    PROBE("stp x11, x12, [%0]", p + 4088, SEGV_ACCERR, p + 4096);
    // LDP without writeback may legally overwrite its own address register.
    expected_code = SEGV_ACCERR; expected_addr = (uintptr_t)p + 4096;
    CHECK(mprotect(p + 4096, 4096, PROT_NONE) == 0);
    before = faults;
    register uintptr_t pair_addr __asm__("x13") = (uintptr_t)p + 4088;
    __asm__ volatile("mov x10, #123\n\tldp x13, x11, [x13]"
        : "+r"(pair_addr) : : "x10", "x11", "memory");
    CHECK(faults == before + 1 && bad_signals == 0);
    CHECK(munmap(p + 4096, 4096) == 0);
    PROBE("ldrb w11, [%0]", p + 4096, SEGV_MAPERR, p + 4096);
    PROBE("strb w11, [%0]", p + 4096, SEGV_MAPERR, p + 4096);
    CHECK(mprotect(p, 4096, PROT_READ) == 0);
    PROBE("strb w11, [%0]", p, SEGV_ACCERR, p);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        PROBE("strb w11, [%0]", p, SEGV_ACCERR, p);
        _exit(failures ? 1 : 0);
    }
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(munmap(p, 4096) == 0);
    uintptr_t base = syscall(SYS_brk, 0);
    CHECK((uintptr_t)syscall(SYS_brk, base + 16384) == base + 16384);
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    CHECK((uintptr_t)syscall(SYS_brk, base + 16384) == base + 16384);
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    void *middle = mmap((void *)(base + 8192), 4096, PROT_READ,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(middle == (void *)(base + 8192));
    CHECK((uintptr_t)syscall(SYS_brk, base + 16384) == base);
    PROBE("strb w11, [%0]", middle, SEGV_ACCERR, middle);
    CHECK(munmap(middle, 4096) == 0);
    CHECK((uintptr_t)syscall(SYS_brk, base + 16384) == base + 16384);
    CHECK((uintptr_t)syscall(SYS_brk, base) == base);
    printf("memory permissions: %d checks, %d failures\n%s\n", checks, failures,
           failures ? "FAIL" : "ALL PASS");
    return failures != 0;
}
