// Handler edits to the AArch64 signal frame must survive rt_sigreturn.
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
// Keep the fault and continuation in assembly so compiler allocation cannot
// hide an ignored context edit. Capture SP before restoring the function frame.
__asm__(
    ".text\n.align 2\n.global probe\n.type probe,%function\nprobe:\n"
    "stp x19, x30, [sp, #-16]!\nmov x19, x1\n"
    "mov x10, #123\nmov x11, #456\nmovi v31.16b, #0\n"
    "ldr x0, [x0]\n"
    "str x0, [x19]\nstr x10, [x19, #8]\nstr x11, [x19, #16]\n"
    "mrs x9, nzcv\nstr x9, [x19, #24]\n"
    "mov x9, sp\nstr x9, [x19, #32]\nstr q31, [x19, #48]\n"
    "mrs x9, fpcr\nstr x9, [x19, #64]\n"
    "mrs x9, fpsr\nstr x9, [x19, #72]\n"
    "sub sp, sp, #16\nldp x19, x30, [sp], #16\nret\n"
    ".size probe, .-probe\n");
struct fpsimd { uint32_t magic, size, fpsr, fpcr; uint64_t v[32][2]; };
static volatile sig_atomic_t faults, bad, nested;
static uint64_t saved_sp;
static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void nested_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc = context;
    stack_t st;
    if (sigaltstack(NULL, &st) || !(st.ss_flags & SS_ONSTACK)) ++bad;
    if (!(uc->uc_stack.ss_flags & SS_ONSTACK)) ++bad;
    ++nested;
}
static void handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    ucontext_t *uc = context;
    if (++faults != 1 || sig != SIGSEGV) _exit(10);
    if (uc->uc_mcontext.regs[10] != 123 || uc->uc_mcontext.regs[11] != 456)
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
    struct fpsimd *fp = (void *)uc->uc_mcontext.__reserved;
    if (fp->magic != 0x46508001 || fp->size != sizeof(*fp)) _exit(11);
    fp->v[31][0] = 0x1122334455667788ULL;
    fp->v[31][1] = 0x8877665544332211ULL;
    fp->fpcr = 1u << 22;
    fp->fpsr = 1u << 27;
    // Records are not required to start with FPSIMD. Insert a valid ESR
    // record before it to exercise the parser instead of fixed offsets.
    unsigned char *r = (void *)uc->uc_mcontext.__reserved;
    memmove(r + 16, r, sizeof(*fp));
    memset(r, 0, 16);
    uint32_t head[2] = {0x45535201, 16};
    memcpy(r, head, sizeof(head));
    memset(r + 16 + sizeof(*fp), 0, 16);
}
static void exit_segv(int sig) { _exit(sig == SIGSEGV ? 42 : 43); }
static void corrupt_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc = context;
    // A broken record must fault, rather than reverting to the private copy.
    uint32_t size = 8;
    memcpy((unsigned char *)uc->uc_mcontext.__reserved + 4, &size, sizeof(size));
    struct sigaction sa = {.sa_handler = exit_segv};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void *p = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *stack = mmap(NULL, 16384, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED && stack != MAP_FAILED);
    if (p == MAP_FAILED || stack == MAP_FAILED) return 1;
    stack_t alt = {.ss_sp = stack, .ss_size = 16384};
    CHECK(sigaltstack(&alt, NULL) == 0);
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    sa.sa_sigaction = nested_handler;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    uint64_t old_fpcr, old_fpsr;
    __asm__ volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(old_fpcr), "=r"(old_fpsr));
    struct result r = {0};
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
    CHECK(sigismember(&mask, SIGKILL) == 0 && sigismember(&mask, SIGSTOP) == 0);
    CHECK(sigismember(&mask, SIGSEGV) == 0 && sigismember(&mask, SIGUSR1) == 0);
    CHECK(sigaltstack(NULL, &alt) == 0 && !(alt.ss_flags & SS_ONSTACK));
    CHECK(alt.ss_sp == stack && alt.ss_size == 16384);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        sa.sa_sigaction = corrupt_handler;
        sigaction(SIGUSR1, &sa, NULL);
        raise(SIGUSR1);
        _exit(44);
    }
    if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    }
    alt.ss_flags = SS_DISABLE;
    CHECK(sigaltstack(&alt, NULL) == 0);
    CHECK(munmap(stack, 16384) == 0 && munmap(p, 4096) == 0);
    printf("sigreturn context: %d checks, %d failures\n", checks, failures);
    puts(failures ? "FAIL" : "ALL PASS");
    return failures != 0;
}
