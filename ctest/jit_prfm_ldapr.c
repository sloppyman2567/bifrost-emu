// jit_prfm_ldapr.c — regression coverage for PRFM (prefetch) and
// LDAPR/LDAPRB/LDAPRH (ARMv8.3 RCpc acquire loads).
//
// Both decoded incorrectly before the fix:
//   - PRFM (all addressing forms) fell through to the normal load path.
//     The prfop field (bits[4:0]) is not a register, so the "load" wrote
//     into that GPR (e.g. `prfm pstl1keep, [x0, #8]` clobbered x16) and
//     could fault on an address the guest only intended to hint.
//   - LDAPR has the same bit[21]=1/mode=00 shape as the LSE atomics and
//     was decoded as atom_op=0xC (CAS), silently comparing/swapping.
//
// Runs under both JIT and --no-jit; "checks passed" is the pass marker.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

// ── PRFM is a NOP (must not clobber the prfop-numbered register) ───────
static void test_prfm_nop(void) {
    uint64_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0xA5A5A5A500000000ULL + (uint64_t)i;
    const uint64_t sentinel = 0x1122334455667788ULL;
    uint64_t base = (uint64_t)buf;
    // Fixed register variables: GCC on AArch64 does not accept GPR names
    // in the asm clobber list, so bind x16/x3 explicitly and treat them as
    // read-write operands.
    register uint64_t r16 __asm__("x16") = sentinel;
    register uint64_t r3  __asm__("x3")  = 24;
    __asm__ volatile (
        "prfm pldl1keep, [%0]\n"              // Rt=0
        "prfm pstl1keep, [%0, #8]\n"          // Rt=16 -> would clobber x16
        "prfm pldl1keep, [%0, x3, lsl #3]\n"  // register offset form
        "prfm pldl1keep, [%0, #-8]\n"         // PRFUM (unscaled)
        "prfm pldl1keep, [%0, #32760]\n"      // far offset: a load would fault
        : "+r"(r16), "+r"(r3)
        : "r"(base)
        : "memory"
    );
    CHECK(r16 == sentinel, "prfm does not clobber the prfop-numbered register");
}

// ── LDAPR family (RCpc acquire load) ───────────────────────────────────
static void test_ldapr(void) {
    uint64_t q = 0x0123456789ABCDEFULL;
    uint16_t h = 0xBEEF;
    uint8_t  b = 0x5A;

    uint64_t qo = 0; uint64_t ho = 0; uint64_t bo = 0;
    __asm__ volatile (".arch armv8.3-a\n\tldapr %0, [%1]"  : "=r"(qo) : "r"(&q) : "memory");
    CHECK(qo == q, "ldapr x: loads the 64-bit word");

    uint64_t wo = 0;
    __asm__ volatile (".arch armv8.3-a\n\tldapr %w0, [%1]" : "=r"(wo) : "r"(&q) : "memory");
    CHECK((wo & 0xFFFFFFFFULL) == (q & 0xFFFFFFFFULL), "ldapr w: loads the low 32 bits");

    __asm__ volatile (".arch armv8.3-a\n\tldaprh %w0, [%1]" : "=r"(ho) : "r"(&h) : "memory");
    CHECK((ho & 0xFFFFULL) == h, "ldaprh: loads the 16-bit halfword");

    __asm__ volatile (".arch armv8.3-a\n\tldaprb %w0, [%1]" : "=r"(bo) : "r"(&b) : "memory");
    CHECK((bo & 0xFFULL) == b, "ldaprb: loads the byte");
}

int main(void) {
    test_prfm_nop();
    test_ldapr();
    if (failures == 0) {
        printf("jit_prfm_ldapr: checks passed (%d)\n", checks);
        return 0;
    }
    printf("jit_prfm_ldapr: %d/%d FAILED\n", failures, checks);
    return 1;
}
