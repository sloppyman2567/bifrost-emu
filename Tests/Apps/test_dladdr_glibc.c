/* test_dladdr_glibc.c — dladdr() via the real glibc symbol.
 *
 * Unlike test_dladdr.c (which uses the internal syscall 0x1005
 * directly), this is a DYNAMICALLY-LINKED glibc binary that calls
 * dladdr() from <dlfcn.h>. This verifies that the dladdr symbol
 * override in the dynamic linker routes user dladdr() calls through
 * our implementation (stub at OFF_DLADDR → syscall 0x1005).
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o test_dladdr_glibc.elf test_dladdr_glibc.c -ldl
 *   (must be built with the glibc toolchain, dynamically linked)
 * Run:   BIFROST_ROOT=rootfs ./bifrost-emu test_dladdr_glibc.elf
 * Pass:  exit 0.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* Open libm and resolve sqrt. */
    void* hlib = dlopen("/lib/libm.so.6", RTLD_NOW);
    if (!hlib) {
        printf("SKIP: dlopen libm failed: %s\n", dlerror());
        return 77;
    }
    printf("dlopen libm.so.6 -> %p\n", hlib);

    /* Use dlsym to get sqrt's address. */
    dlerror();  /* clear */
    void* sqrt_fn = dlsym(hlib, "sqrt");
    if (!sqrt_fn) {
        printf("SKIP: dlsym sqrt failed: %s\n", dlerror());
        return 77;
    }
    printf("dlsym sqrt -> %p\n", sqrt_fn);

    /* Call the REAL glibc dladdr() — this exercises the symbol
     * override (dladdr@GLIBC_2.34 → our OFF_DLADDR stub → syscall
     * 0x1005 → DynamicLinker::dladdr). */
    Dl_info info;
    memset(&info, 0, sizeof(info));
    int found = dladdr(sqrt_fn, &info);
    printf("dladdr(sqrt) -> found=%d\n", found);

    CK(found != 0, "glibc dladdr() finds the containing DSO for sqrt");
    printf("  dli_fname = %s\n", info.dli_fname ? info.dli_fname : "(null)");
    printf("  dli_fbase = %p\n", info.dli_fbase);
    printf("  dli_sname = %s\n", info.dli_sname ? info.dli_sname : "(null)");
    printf("  dli_saddr = %p\n", info.dli_saddr);

    CK(info.dli_fbase != NULL, "dladdr returns non-zero dli_fbase");
    CK(info.dli_fname != NULL, "dladdr returns non-zero dli_fname");
    if (info.dli_fname) {
        CK(strstr(info.dli_fname, "libm") != NULL,
           "dli_fname contains 'libm'");
    }
    CK(info.dli_sname != NULL, "dladdr returns non-zero dli_sname");
    if (info.dli_sname) {
        CK(strstr(info.dli_sname, "sqrt") != NULL,
           "dli_sname is a sqrt-family symbol");
    }
    CK(info.dli_saddr == sqrt_fn, "dli_saddr == sqrt address");

    /* dladdr on an unmapped address should return 0. */
    Dl_info info2;
    memset(&info2, 0, sizeof(info2));
    int found2 = dladdr((void*)0xdeadbeefULL, &info2);
    printf("dladdr(0xdeadbeef) -> found=%d (expect 0)\n", found2);
    CK(found2 == 0, "dladdr returns 0 for unmapped address");

    /* dladdr on the main binary's main() should find the main binary. */
    Dl_info info3;
    memset(&info3, 0, sizeof(info3));
    int found3 = dladdr((void*)main, &info3);
    printf("dladdr(main) -> found=%d\n", found3);
    CK(found3 != 0, "dladdr finds the main binary for main()");

    dlclose(hlib);

    if (failures) {
        printf("test_dladdr_glibc: FAIL (%d checks failed)\n", failures);
        return 1;
    }
    printf("test_dladdr_glibc: ALL PASS\n");
    return 0;
}
