// Guest call probes live in executable ELF code, not an executable stack.
#include <stdint.h>

uint64_t capi_add(uint64_t a, uint64_t b) { return a + b; }
double capi_fadd(double a, double b) { return a + b; }

uint64_t capi_getpid(void) {
    register uint64_t result __asm__("x0");
    register uint64_t number __asm__("x8") = 172;
    __asm__ volatile("svc #0" : "=r"(result) : "r"(number) : "memory", "cc");
    return result;
}

// readlink must return an absolute executable path without a trailing NUL,
// including when the caller supplies only one byte of buffer space.
uint64_t capi_proc_exe(void) {
    char path[] = "/proc/self/exe";
    char buf[2] = {'?', '?'};
    register int64_t x0 __asm__("x0") = -100;
    register char* x1 __asm__("x1") = path;
    register char* x2 __asm__("x2") = buf;
    register uint64_t x3 __asm__("x3") = 1;
    register uint64_t x8 __asm__("x8") = 78;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0 == 1 && buf[0] == '/' && buf[1] == '?';
}
