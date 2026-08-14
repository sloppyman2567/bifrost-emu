// FCVT rounding-mode regression test (v1.5.2-alpha).
//
// The JIT's FP→int conversion (FP_F2I) only handled FCVTZS/FCVTZU
// (rmode=3, toward zero). floor() compiles to FCVTMS (rmode=2) and
// ceil() to FCVTPS (rmode=1), so chunk/mesh math fell back to
// CALL_INTERP — the interpreter. This test forces every rounding-mode
// variant (FCVTNS/FCVTMS/FCVTPS/FCVTZS, single + double, 32-bit and
// 64-bit GPR dest) via inline asm and checks the results against
// hand-rolled floor/ceil/nearest-even/trunc logic (no libm, so the
// binary builds under `make setup-tests`).
#include <stdio.h>
#include <stdint.h>

int fails = 0;
#define CK(cond, ...) do { if (!(cond)) { printf("FAIL %s: ", #cond); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ── libm-free reference rounding (pure C, matches AArch64 FCVT modes) ──
static long long ref_floor(double v) { long long t = (long long)v; return (v < (double)t) ? t - 1 : t; }
static long long ref_ceil(double v)  { long long t = (long long)v; return (v > (double)t) ? t + 1 : t; }
static long long ref_trunc(double v) { return (long long)v; }
static long long ref_neareven(double v) {
    long long n = (long long)v;
    double frac = v - (double)n;
    if (frac > 0.5) return n + 1;
    if (frac < -0.5) return n - 1;
    if (frac == 0.5) return (n & 1) ? n + 1 : n;
    if (frac == -0.5) return (n & 1) ? n - 1 : n;
    return n;
}

// ── Forced encodings (single-precision source, 32-bit GPR dest) ──
static inline int fcvtms_s(float v) { int r; __asm__("fcvtms %w0, %s1" : "=r"(r) : "w"(v)); return r; }
static inline int fcvtps_s(float v) { int r; __asm__("fcvtps %w0, %s1" : "=r"(r) : "w"(v)); return r; }
static inline int fcvtns_s(float v) { int r; __asm__("fcvtns %w0, %s1" : "=r"(r) : "w"(v)); return r; }
static inline int fcvtzs_s(float v) { int r; __asm__("fcvtzs %w0, %s1" : "=r"(r) : "w"(v)); return r; }
// ── Double-precision source, 64-bit GPR dest ──
static inline long long fcvtms_d(double v) { long long r; __asm__("fcvtms %x0, %d1" : "=r"(r) : "w"(v)); return r; }
static inline long long fcvtps_d(double v) { long long r; __asm__("fcvtps %x0, %d1" : "=r"(r) : "w"(v)); return r; }
static inline long long fcvtns_d(double v) { long long r; __asm__("fcvtns %x0, %d1" : "=r"(r) : "w"(v)); return r; }
static inline long long fcvtzs_d(double v) { long long r; __asm__("fcvtzs %x0, %d1" : "=r"(r) : "w"(v)); return r; }

double dv[] = { 2.7, -2.7, 3.0, -3.0, 0.5, -0.5, 0.0, 1.99, -1.99, 4.5, -4.5, 1e9, -1e9, 2.0000001, -2.0000001 };
float fv[] = { 2.7f, -2.7f, 3.0f, -3.0f, 0.5f, -0.5f, 0.0f, 1.99f, -1.99f, 4.5f, -4.5f, 1e9f, -1e9f, 2.0000001f, -2.0000001f };

int main(void) {
    for (int i = 0; i < (int)(sizeof(dv)/sizeof(dv[0])); i++) {
        double v = dv[i];
        CK(fcvtms_d(v) == ref_floor(v),     "d fcvtms v=%g got %lld exp %lld", v, fcvtms_d(v), ref_floor(v));
        CK(fcvtps_d(v) == ref_ceil(v),      "d fcvtps v=%g got %lld exp %lld", v, fcvtps_d(v), ref_ceil(v));
        CK(fcvtns_d(v) == ref_neareven(v),  "d fcvtns v=%g got %lld exp %lld", v, fcvtns_d(v), ref_neareven(v));
        CK(fcvtzs_d(v) == ref_trunc(v),     "d fcvtzs v=%g got %lld exp %lld", v, fcvtzs_d(v), ref_trunc(v));
    }
    for (int i = 0; i < (int)(sizeof(fv)/sizeof(fv[0])); i++) {
        float v = fv[i];
        CK(fcvtms_s(v) == (int)ref_floor(v),    "s fcvtms v=%g got %d exp %d", v, fcvtms_s(v), (int)ref_floor(v));
        CK(fcvtps_s(v) == (int)ref_ceil(v),     "s fcvtps v=%g got %d exp %d", v, fcvtps_s(v), (int)ref_ceil(v));
        CK(fcvtns_s(v) == (int)ref_neareven(v), "s fcvtns v=%g got %d exp %d", v, fcvtns_s(v), (int)ref_neareven(v));
        CK(fcvtzs_s(v) == (int)ref_trunc(v),    "s fcvtzs v=%g got %d exp %d", v, fcvtzs_s(v), (int)ref_trunc(v));
    }
    // Game pattern: a loop of consecutive FCVTMS (floor math) exercises the
    // previously-interp_only two-FCVMTS block shape (0x1e3000xx).
    volatile float acc = 0;
    for (int i = 0; i < 1000; i++) acc += fcvtms_s(2.7f + i * 0.01f);
    CK(acc > 0, "loop acc=%g", (double)acc);
    printf("%s (%d fails)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
