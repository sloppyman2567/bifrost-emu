// Handler edits to the AArch64 signal frame must survive rt_sigreturn.
// Layout and return semantics come from Linux's AArch64 UAPI/kernel:
// arch/arm64/include/uapi/asm/sigcontext.h and arch/arm64/kernel/signal.c.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

_Static_assert(offsetof(ucontext_t, uc_mcontext.regs) == 184, "GPR layout");
_Static_assert(offsetof(ucontext_t, uc_mcontext.pc) == 440, "PC layout");
_Static_assert(offsetof(ucontext_t, uc_mcontext.__reserved) == 464, "FP layout");
_Static_assert(sizeof(ucontext_t) == 4560, "ucontext layout");
struct result { uint64_t x0, x10, x11, nzcv, sp, pad, v_lo, v_hi, fpcr, fpsr; };
extern void probe(void *, struct result *);
extern char probe_fault[];
extern void bad_return(uintptr_t) __attribute__((noreturn));
// Keep the fault and continuation in assembly so compiler allocation cannot
// hide an ignored context edit. Capture SP before restoring the function frame.
__asm__(
    ".text\n.align 2\n.global probe\n.type probe,%function\nprobe:\n"
    "stp x19, x30, [sp, #-16]!\nmov x19, x1\n"
    "mov x10, #123\nmov x11, #456\nmovi v31.16b, #0\n"
    ".global probe_fault\nprobe_fault:\nldr x0, [x0]\n"
    "str x0, [x19]\nstr x10, [x19, #8]\nstr x11, [x19, #16]\n"
    "mrs x9, nzcv\nstr x9, [x19, #24]\n"
    "mov x9, sp\nstr x9, [x19, #32]\nstr q31, [x19, #48]\n"
    "mrs x9, fpcr\nstr x9, [x19, #64]\n"
    "mrs x9, fpsr\nstr x9, [x19, #72]\n"
    "sub sp, sp, #16\nldp x19, x30, [sp], #16\nret\n"
    ".size probe, .-probe\n");
__asm__(
    ".text\n.align 2\n.global bad_return\n.type bad_return,%function\n"
    "bad_return:\nmov sp, x0\nmov x8, #139\nsvc #0\nbrk #0\n"
    ".size bad_return, .-bad_return\n");
struct fpsimd { uint32_t magic, size, fpsr, fpcr; uint64_t v[32][2]; };
_Static_assert(sizeof(struct fpsimd) == 528, "FPSIMD record size");
_Static_assert(offsetof(struct fpsimd, v) == 16, "FPSIMD vector offset");
static volatile sig_atomic_t faults, bad, nested;
static uint64_t saved_sp;
static uintptr_t fault_address;
static int corruption;
static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
// Context record order is not fixed. Find FPSIMD instead of assuming the
// first reserved record is it (real Linux can also provide ESR/SVE records).
static struct fpsimd *find_fpsimd(ucontext_t *uc) {
    unsigned char *r = (void *)uc->uc_mcontext.__reserved;
    size_t limit = sizeof(uc->uc_mcontext.__reserved);
    struct fpsimd *fp = NULL;
    for (size_t off = 0; off + 8 <= limit;) {
        uint32_t h[2];
        memcpy(h, r + off, sizeof(h));
        if (h[0] == 0 && h[1] == 0) return fp;
        if (h[1] < 16 || (h[1] & 15) || h[1] > limit - off) _exit(12);
        if (h[0] == 0x46508001) {
            if (fp || h[1] != sizeof(*fp)) _exit(13);
            fp = (void *)(r + off);
        }
        off += h[1];
    }
    _exit(14);
}
static void nested_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc = context;
    stack_t st;
    if (sigaltstack(NULL, &st) || !(st.ss_flags & SS_ONSTACK)) ++bad;
    if (!(uc->uc_stack.ss_flags & SS_ONSTACK)) ++bad;
    ++nested;
}
static void handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (++faults != 1 || sig != SIGSEGV) _exit(10);
    if (info->si_code != SEGV_ACCERR || (uintptr_t)info->si_addr != fault_address ||
        uc->uc_mcontext.pc != (uintptr_t)probe_fault ||
        uc->uc_mcontext.regs[10] != 123 || uc->uc_mcontext.regs[11] != 456)
        ++bad;
    saved_sp = uc->uc_mcontext.sp;
    // A nested handler must restore the outer handler's alternate-stack state.
    if (raise(SIGUSR1)) ++bad;
    stack_t st;
    if (sigaltstack(NULL, &st) || !(st.ss_flags & SS_ONSTACK)) ++bad;
    uc->uc_mcontext.pc += 4; // skip the protected load
    uc->uc_mcontext.regs[0] = 0x123456789abcdef0ULL;
    uc->uc_mcontext.regs[10] = 789;
    uc->uc_mcontext.regs[11] = 987;
    uc->uc_mcontext.sp += 16;
    uc->uc_mcontext.pstate = 0xa0000000; // N and C
    sigaddset(&uc->uc_sigmask, SIGUSR2);
    sigaddset(&uc->uc_sigmask, SIGKILL); // cannot be blocked
    sigaddset(&uc->uc_sigmask, SIGSTOP);
    struct fpsimd *fp = find_fpsimd(uc);
    if (!fp) _exit(11);
    fp->v[31][0] = 0x1122334455667788ULL;
    fp->v[31][1] = 0x8877665544332211ULL;
    fp->fpcr = 1u << 22;
    fp->fpsr = 1u << 27;
    // Records are not required to start with FPSIMD. Insert a valid ESR
    // record before it to exercise the parser instead of fixed offsets.
    unsigned char *r = (void *)uc->uc_mcontext.__reserved;
    // Explicitly construct the supported baseline ESR + FPSIMD + terminator
    // sequence. It does not claim to preserve optional SVE/SME state.
    struct fpsimd saved = *fp;
    memset(r, 0, sizeof(uc->uc_mcontext.__reserved));
    memcpy(r + 16, &saved, sizeof(saved));
    uint32_t head[2] = {0x45535201, 16};
    memcpy(r, head, sizeof(head));
    memset(r + 16 + sizeof(*fp), 0, 16);
}
static void exit_segv(int sig) { _exit(sig == SIGSEGV ? 42 : 43); }
static void corrupt_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc = context;
    struct fpsimd *fp = find_fpsimd(uc);
    if (!fp) _exit(15);
    struct fpsimd saved = *fp;
    unsigned char *r = (void *)uc->uc_mcontext.__reserved;
    size_t limit = sizeof(uc->uc_mcontext.__reserved);
    memset(r, 0, limit);
    memcpy(r, &saved, sizeof(saved));
    uint32_t size;
    switch (corruption) {
        case 0: size = 8; memcpy(r + 4, &size, 4); break; // too short
        case 1: size = 512; memcpy(r + 4, &size, 4); break; // wrong FP size
        case 2: size = (uint32_t)limit + 16; memcpy(r + 4, &size, 4); break;
        case 3: memcpy(r + sizeof(saved), &saved, sizeof(saved)); break; // duplicate
        case 4: memset(r, 0, limit); break; // missing FPSIMD
        case 5: // valid records consume the whole area: no terminator
            for (size_t off = sizeof(saved); off < limit; off += 16) {
                uint32_t h[2] = {0x45535201, 16};
                memcpy(r + off, h, sizeof(h));
            }
            break;
        case 6: size = 0xdeadbeef; memcpy(r, &size, 4); break; // unknown magic
    }
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0 && (page_size & (page_size - 1)) == 0);
    if (page_size <= 0 || (page_size & (page_size - 1))) return 1;
    size_t stack_size = (size_t)SIGSTKSZ;
    if (stack_size < 32768) stack_size = 32768;
    stack_size = (stack_size + page_size - 1) & ~(size_t)(page_size - 1);
    void *p = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED && stack != MAP_FAILED);
    if (p == MAP_FAILED || stack == MAP_FAILED) return 1;
    stack_t alt = {.ss_sp = stack, .ss_size = stack_size};
    CHECK(sigaltstack(&alt, NULL) == 0);
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    sa.sa_sigaction = nested_handler;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    uint64_t old_fpcr, old_fpsr;
    __asm__ volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(old_fpcr), "=r"(old_fpsr));
    struct result r = {0};
    fault_address = (uintptr_t)p;
    probe(p, &r);
    CHECK(faults == 1 && bad == 0 && nested == 1);
    CHECK(r.x0 == 0x123456789abcdef0ULL);
    CHECK(r.x10 == 789 && r.x11 == 987);
    CHECK(r.sp == saved_sp + 16);
    CHECK(r.nzcv == 0xa0000000);
    CHECK(r.v_lo == 0x1122334455667788ULL && r.v_hi == 0x8877665544332211ULL);
    CHECK(r.fpcr == (1u << 22) && r.fpsr == (1u << 27));
    __asm__ volatile("msr fpcr, %0\nmsr fpsr, %1" : : "r"(old_fpcr), "r"(old_fpsr));
    sigset_t mask;
    CHECK(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    CHECK(sigismember(&mask, SIGUSR2) == 1);
    // Linux kernel/signal.c:set_current_blocked removes both bits on return.
    // Keep this strict; tools/qemu-reference fixes unpatched QEMU's mismatch.
    CHECK(sigismember(&mask, SIGKILL) == 0);
    CHECK(sigismember(&mask, SIGSTOP) == 0);
    CHECK(sigismember(&mask, SIGSEGV) == 0 && sigismember(&mask, SIGUSR1) == 0);
    CHECK(sigaltstack(NULL, &alt) == 0 && !(alt.ss_flags & SS_ONSTACK));
    CHECK(alt.ss_sp == stack && alt.ss_size == stack_size);
    // Each malformed frame gets its own process. Unexpected normal return,
    // SIGILL, or process death cannot masquerade as the expected SIGSEGV.
    static const char *const bad_frames[] = {
        "short header", "wrong FPSIMD size", "record exceeds reserved area",
        "duplicate FPSIMD", "missing FPSIMD", "missing terminator",
        "unknown record", "misaligned SP", "unreadable SP"
    };
    for (corruption = 0; corruption < (int)(sizeof(bad_frames) / sizeof(bad_frames[0]));
         ++corruption) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            sa.sa_handler = exit_segv;
            sa.sa_flags = SA_ONSTACK;
            if (sigaction(SIGSEGV, &sa, NULL)) _exit(45);
            if (corruption >= 7) {
                // Misaligned and unreadable frame SP, independently of an
                // internal signal-frame snapshot. Handler uses the altstack.
                bad_return((uintptr_t)p + (corruption == 7 ? 1 : 0));
            }
            sa.sa_sigaction = corrupt_handler;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
            if (sigaction(SIGUSR1, &sa, NULL)) _exit(46);
            if (raise(SIGUSR1)) _exit(47);
            _exit(44);
        }
        if (child > 0) {
            int status = 0;
            CHECK(waitpid(child, &status, 0) == child);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
                printf("bad frame '%s': unexpected wait status 0x%x\n",
                       bad_frames[corruption], status);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
        }
    }
    alt.ss_flags = SS_DISABLE;
    CHECK(sigaltstack(&alt, NULL) == 0);
    CHECK(munmap(stack, stack_size) == 0 && munmap(p, page_size) == 0);
    printf("sigreturn context: %d checks, %d failures\n", checks, failures);
    puts(failures ? "FAIL" : "ALL PASS");
    return failures != 0;
}
