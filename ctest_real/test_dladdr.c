/* test_dladdr.c — dladdr() functional test for bifrost-emu.
 *
 * Verifies that dladdr() resolves an address inside a loaded shared
 * library to the containing DSO (dli_fname, dli_fbase) and the nearest
 * symbol (dli_sname, dli_saddr). Uses the bifrost internal syscall
 * 0x1005 (dladdr) directly so a static musl binary can exercise the
 * dynamic linker's dladdr() implementation.
 *
 * Build: make cross SRC=ctest_real/test_dladdr.c OUT=ctest_real/test_dladdr.elf
 * Run:   ./bifrost-emu ctest_real/test_dladdr.elf
 * Pass:  exit 0. Exit 77 = skip when libm/dladdr unavailable.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── Bifrost internal dlopen/dlsym/dladdr syscalls ──────────────────── */
static uint64_t bifrost_dlopen(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bifrost_dlsym(uint64_t handle, const char* name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static int bifrost_dladdr(const void* addr, void* info) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)addr;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)info;
    register uint64_t x8 __asm__("x8") = 0x1005;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return (int)(int64_t)x0;
}

/* Dl_info struct (matches glibc/musl layout). */
struct Dl_info {
    const char* dli_fname;
    void*       dli_fbase;
    const char* dli_sname;
    void*       dli_saddr;
};

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* Open libm.so.6 and resolve sqrt. */
    uint64_t hlib = bifrost_dlopen("/lib/libm.so.6", 2 /* RTLD_NOW */);
    if (!hlib) {
        printf("SKIP: could not dlopen /lib/libm.so.6 (no rootfs?)\n");
        return 77;
    }
    printf("dlopen libm.so.6 -> handle=0x%llx\n",
           (unsigned long long)hlib);

    void* sqrt_fn = (void*)(uintptr_t)bifrost_dlsym(hlib, "sqrt");
    if (!sqrt_fn) {
        printf("SKIP: could not dlsym sqrt\n");
        return 77;
    }
    printf("dlsym sqrt -> %p\n", sqrt_fn);

    /* dladdr() the sqrt address. */
    struct Dl_info info;
    memset(&info, 0, sizeof(info));
    int found = bifrost_dladdr(sqrt_fn, &info);
    printf("dladdr(sqrt) -> found=%d\n", found);

    CK(found == 1, "dladdr finds the containing DSO for sqrt");
    printf("  dli_fname = %s\n", info.dli_fname ? info.dli_fname : "(null)");
    printf("  dli_fbase = %p\n", info.dli_fbase);
    printf("  dli_sname = %s\n", info.dli_sname ? info.dli_sname : "(null)");
    printf("  dli_saddr = %p\n", info.dli_saddr);

    CK(info.dli_fbase != 0, "dladdr returns non-zero dli_fbase");
    CK(info.dli_fname != 0, "dladdr returns non-zero dli_fname (filename)");
    if (info.dli_fname) {
        CK(strstr(info.dli_fname, "libm") != 0,
           "dli_fname contains 'libm'");
    }
    CK(info.dli_sname != 0, "dladdr returns non-zero dli_sname (symbol)");
    if (info.dli_sname) {
        /* dladdr returns the nearest symbol; sqrt may be an alias
         * (sqrtf32x, __sqrt_finite) at the same address. Accept any
         * name whose address exactly matches the looked-up address. */
        printf("  (sname is an alias of sqrt at the same address)\n");
        CK(strcmp(info.dli_sname, "sqrt") == 0 ||
           strcmp(info.dli_sname, "sqrtf32x") == 0 ||
           strstr(info.dli_sname, "sqrt") != 0,
           "dli_sname is a sqrt-family symbol");
    }
    CK(info.dli_saddr == sqrt_fn, "dli_saddr == sqrt address");

    /* dladdr on an unmapped address should return 0. */
    struct Dl_info info2;
    memset(&info2, 0, sizeof(info2));
    int found2 = bifrost_dladdr((void*)0xdeadbeefULL, &info2);
    printf("dladdr(0xdeadbeef) -> found=%d (expect 0)\n", found2);
    CK(found2 == 0, "dladdr returns 0 for unmapped address");

    if (failures) {
        printf("test_dladdr: FAIL (%d checks failed)\n", failures);
        return 1;
    }
    printf("test_dladdr: ALL PASS\n");
    return 0;
}

