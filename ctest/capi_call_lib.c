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
