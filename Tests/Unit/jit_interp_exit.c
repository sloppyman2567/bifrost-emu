// jit_interp_exit.c — an interpreter-fallback instruction that changes the
// PC (here: SIGILL delivery) must leave its JIT block through a dispatcher
// return, not through the block's chain slot. The slot is patched to the
// STATIC successor once that block exists, so the old shared exit dropped
// the signal handler PC and ran the successor with the signal frame's
// registers; its `ret` then re-executed the faulting instruction forever.
//
// probe(0) translates the successor first, so probe(1)'s block is chained
// before its UNDEF runs. Plain C + inline asm: also runs under qemu-aarch64.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

uint64_t probe(uint64_t take_undef);
__asm__(".text\n .global probe\n .type probe,%function\n"
        "probe:\n"
        "  mov x1, #10\n"
        "  cbz x0, 2f\n"
        "  .inst 0x0e60fc02\n"   // FRECPS Q=0,D: reserved -> SIGILL
        "  add x1, x1, #1\n"
        "  b 3f\n"
        "2: add x1, x1, #100\n"
        "  b 3f\n"
        "3: add x0, x1, #5\n"
        "  ret\n"
        " .size probe, .-probe\n");

static volatile int ill_count;

static void on_ill(int sig, siginfo_t* si, void* ctx) {
    (void)sig; (void)si;
    ucontext_t* uc = (ucontext_t*)ctx;
    uc->uc_mcontext.pc += 4;  // skip the UNDEF, resume inside the block
    ill_count++;
}

static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "FAIL: timeout (handler PC dropped)\n";
    write(1, msg, sizeof msg - 1);
    _exit(1);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_ill;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    signal(SIGALRM, on_alarm);
    alarm(10);

    int checks = 0, failures = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t take = (uint64_t)(i & 1);
        uint64_t got = probe(take);
        uint64_t want = take ? 16 : 115;
        checks++;
        if (got != want && failures++ < 8)
            printf("FAIL iter %d: got %llu want %llu\n", i,
                   (unsigned long long)got, (unsigned long long)want);
    }
    checks++;
    if (ill_count != 1000 && failures++ < 8)
        printf("FAIL: %d SIGILLs, want 1000\n", ill_count);
    alarm(0);
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
