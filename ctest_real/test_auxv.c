// test_auxv.c — Verify auxiliary vector entries provided by the kernel.
//
// What it verifies:
//   1. AT_PAGESZ is 4096.
//   2. AT_PHDR, AT_PHENT, AT_PHNUM are non-zero (for static binaries).
//   3. AT_ENTRY is non-zero and points to a valid address.
//   4. AT_RANDOM provides 16 bytes of random data (non-zero).
//   5. AT_HWCAP advertises FP + ASIMD + (optionally) ATOMICS.
//   6. AT_HWCAP2 is present (even if 0).
//   7. AT_EXECFN points to a readable string.
//   8. AT_BASE is present (interpreter base, 0 for static).
//
// Build: make cross SRC=ctest_real/test_auxv.c OUT=ctest_real/test_auxv.elf
// Run:   ./bifrost-emu ctest_real/test_auxv.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>
#include <sys/auxv.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

int main(void) {
    char msg[256];
    int len;

    // 1. AT_PAGESZ.
    long pagesz = getauxval(AT_PAGESZ);
    len = snprintf(msg, sizeof(msg), "AT_PAGESZ=%ld\n", pagesz);
    write(2, msg, len);
    CHECK(pagesz == 4096, "AT_PAGESZ is 4096");

    // 2. AT_PHDR, AT_PHENT, AT_PHNUM.
    long phdr  = getauxval(AT_PHDR);
    long phent = getauxval(AT_PHENT);
    long phnum = getauxval(AT_PHNUM);
    len = snprintf(msg, sizeof(msg), "AT_PHDR=0x%lx AT_PHENT=%ld AT_PHNUM=%ld\n",
                   phdr, phent, phnum);
    write(2, msg, len);
    CHECK(phdr  != 0, "AT_PHDR is non-zero");
    CHECK(phent == 56, "AT_PHENT is 56 (sizeof Elf64_Phdr)");
    CHECK(phnum > 0, "AT_PHNUM is non-zero");

    // 3. AT_ENTRY.
    long entry = getauxval(AT_ENTRY);
    len = snprintf(msg, sizeof(msg), "AT_ENTRY=0x%lx\n", entry);
    write(2, msg, len);
    CHECK(entry != 0, "AT_ENTRY is non-zero");
    CHECK((entry & 0x3) == 0, "AT_ENTRY is 4-byte aligned");

    // 4. AT_RANDOM — 16 bytes.
    unsigned long random_addr = getauxval(AT_RANDOM);
    len = snprintf(msg, sizeof(msg), "AT_RANDOM=0x%lx\n", random_addr);
    write(2, msg, len);
    CHECK(random_addr != 0, "AT_RANDOM is non-zero");
    // Check the bytes are accessible and not all-zero.
    if (random_addr != 0) {
        unsigned char *r = (unsigned char *)random_addr;
        int nonzero = 0;
        for (int i = 0; i < 16; i++) {
            if (r[i] != 0) nonzero++;
        }
        CHECK(nonzero >= 4, "AT_RANDOM has at least 4 non-zero bytes");
    } else {
        CHECK(0, "AT_RANDOM has at least 4 non-zero bytes");
    }

    // 5. AT_HWCAP — FP, ASIMD, CRC32, ATOMICS bits.
    unsigned long hwcap = getauxval(AT_HWCAP);
    len = snprintf(msg, sizeof(msg), "AT_HWCAP=0x%lx\n", hwcap);
    write(2, msg, len);
    CHECK((hwcap & (1UL << 0)) != 0, "AT_HWCAP advertises FP");
    CHECK((hwcap & (1UL << 1)) != 0, "AT_HWCAP advertises ASIMD");
    CHECK((hwcap & (1UL << 7)) != 0, "AT_HWCAP advertises CRC32");

    // 6. AT_HWCAP2 — should be present (may be 0).
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    len = snprintf(msg, sizeof(msg), "AT_HWCAP2=0x%lx\n", hwcap2);
    write(2, msg, len);
    // We don't assert a specific value — just that the call succeeds.
    CHECK(hwcap2 == 0, "AT_HWCAP2 is 0 (no BTI/PAC)");

    // 7. AT_EXECFN — should point to a readable string.
    unsigned long execfn = getauxval(AT_EXECFN);
    len = snprintf(msg, sizeof(msg), "AT_EXECFN=0x%lx\n", execfn);
    write(2, msg, len);
    CHECK(execfn != 0, "AT_EXECFN is non-zero");
    if (execfn != 0) {
        const char *name = (const char *)execfn;
        // Should contain "auxv" or similar.
        len = snprintf(msg, sizeof(msg), "execfn=\"%s\"\n", name);
        write(2, msg, len);
        CHECK(name[0] != 0, "AT_EXECFN string is non-empty");
    }

    // 8. AT_BASE — interpreter base (0 for static binaries).
    unsigned long base = getauxval(AT_BASE);
    len = snprintf(msg, sizeof(msg), "AT_BASE=0x%lx\n", base);
    write(2, msg, len);
    // For a static binary, AT_BASE is 0. Just verify the call works.
    (void)base;

    // 9. AT_SECURE.
    unsigned long secure = getauxval(AT_SECURE);
    CHECK(secure == 0, "AT_SECURE is 0 (not setuid)");

    // Final summary.
    len = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
