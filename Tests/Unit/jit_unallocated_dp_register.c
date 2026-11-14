// jit_unallocated_dp_register.c — data processing (register) words outside the allocated encodings raise SIGILL.
//
// The decoder took any add/subtract-with-carry word as ADC/ADCS/SBC/SBCS and any conditional compare as CCMP/CCMN,
// whatever their fixed fields held (found by Scripts/Testing/sequence_fuzz.py: rmif x19, #18, #3 ran as an ADCS that
// wrote x3). Arm allocates ADC/SBC only with bits 15:10 = 0 (RMIF and SETF8/SETF16 are FEAT_FlagM, which Bifrost does
// not advertise) and CCMP/CCMN only with o2 (bit 10) and o3 (bit 4) clear. A FlagM word is checked only when
// AT_HWCAP lacks HWCAP_FLAGM, so the binary also passes under QEMU's max CPU.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

static sigjmp_buf trap;
static void on_sigill(int sig) { (void)sig; siglongjmp(trap, 1); }
static int checks, fails;

// A wrong decode as ADCS writes bits 4:0: x2 here, x13 for SETF8/SETF16 (fixed 01101).
#define TRAPS(word, what) do { \
        ++checks; \
        if (!sigsetjmp(trap, 1)) { \
            __asm__ volatile(".inst " #word ::: "x2", "x13", "cc", "memory"); \
            printf("NG %s (" #word ") executed instead of raising SIGILL\n", what); ++fails; \
        } \
    } while (0)

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, 0);

    TRAPS(0x9a020422, "adc x2, x1, x2 with bits 15:10 = 000001");
    TRAPS(0x3a02f822, "adcs w2, w1, w2 with bits 15:10 = 111110");
    TRAPS(0xfa401c02, "ccmp x0, #0, #2, ne with o2 = 1");
    TRAPS(0xfa401812, "ccmp x0, #0, #2, ne with o3 = 1");
    TRAPS(0xba421402, "ccmn x0, x2, #2, ne with o2 = 1");
    if (!(getauxval(AT_HWCAP) & (1ul << 27))) {  // HWCAP_FLAGM
        TRAPS(0xba090662, "rmif x19, #18, #2");
        TRAPS(0x3a00080d, "setf8 w0");
        TRAPS(0x3a00480d, "setf16 w0");
    }

    if (fails) { printf("jit_unallocated_dp_register: FAILED (%d of %d)\n", fails, checks); return 1; }
    printf("jit_unallocated_dp_register: ALL PASS (%d checks)\n", checks);
    return 0;
}
