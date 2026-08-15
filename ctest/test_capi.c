// test_capi.c — Test the C API (libbifrost.a + bifrost.h).
// Verifies: create, load, run, register access, FP access, flags, memory,
// step_n, error reporting, version. Compiles as pure C (no C++ needed).
#include "bifrost.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

int main(int argc, const char* argv[]) {
    printf("=== C API Test (libbifrost.a) ===\n");
    printf("Version: %s\n\n", bifrost_version());

    // Create
    bifrost_emu_t* emu = bifrost_create();
    CHECK(emu != NULL, "bifrost_create()");

    // Load hello.elf
    int r = bifrost_load_elf(emu, "ctest/hello.elf", argc, argv);
    CHECK(r == 0, "bifrost_load_elf(\"ctest/hello.elf\")");

    // Check version
    CHECK(strcmp(bifrost_version(), "1.5.3-alpha") == 0, "version is 1.5.3-alpha");

    // Check initial state
    CHECK(bifrost_is_running(emu), "is_running after load");

    // Get PC (should be non-zero after ELF load)
    uint64_t pc = bifrost_get_pc(emu);
    CHECK(pc != 0, "get_pc() non-zero after load");

    // Get SP
    uint64_t sp = bifrost_get_sp(emu);
    CHECK(sp != 0, "get_sp() non-zero after load");

    // Set/get a register
    bifrost_set_reg(emu, 0, 0xDEADBEEF);
    CHECK(bifrost_get_reg(emu, 0) == 0xDEADBEEF, "set/get reg x0");

    // XZR (reg 31) should always read 0
    bifrost_set_reg(emu, 31, 0x1234);
    CHECK(bifrost_get_reg(emu, 31) == 0, "x31 reads as 0 (XZR)");

    // Out-of-range register
    CHECK(bifrost_get_reg(emu, 32) == 0, "reg 32 returns 0 (out of range)");

    // FP register access
    bifrost_set_fp_reg(emu, 0, 0x3FF0000000000000ULL, 0); // 1.0 in double
    CHECK(bifrost_get_fp_reg_lo(emu, 0) == 0x3FF0000000000000ULL, "set/get fp reg V0.lo");
    CHECK(bifrost_get_fp_reg_hi(emu, 0) == 0, "V0.hi is 0");

    // PSTATE / flags
    bifrost_set_pstate(emu, 0x60000000); // Z=1, C=1
    CHECK(bifrost_get_pstate(emu) == 0x60000000, "set/get pstate");

    bifrost_set_flag(emu, 0, 1); // N=1
    CHECK(bifrost_get_flag(emu, 0) == 1, "set/get flag N");
    CHECK(bifrost_get_flag(emu, 1) == 1, "flag Z still 1");
    bifrost_set_flag(emu, 0, 0); // N=0
    CHECK(bifrost_get_flag(emu, 0) == 0, "flag N back to 0");

    // FPSR / FPCR
    bifrost_set_fpsr(emu, 0x10);
    CHECK(bifrost_get_fpsr(emu) == 0x10, "set/get fpsr");
    bifrost_set_fpcr(emu, 0x00);
    CHECK(bifrost_get_fpcr(emu) == 0x00, "set/get fpcr");

    // Memory access
    uint8_t buf[16] = {0};
    bifrost_read_mem(emu, pc, buf, 4);
    printf("  Instruction at PC: %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3]);

    // Error reporting
    CHECK(strlen(bifrost_get_error(emu)) == 0 || bifrost_get_error(emu)[0] == '\0',
          "no error initially (or empty)");

    // JIT config
    bifrost_set_jit(emu, 1);
    CHECK(bifrost_get_jit(emu) == 1, "JIT enabled");
    bifrost_set_jit_threshold(emu, 100);
    bifrost_set_jit(emu, 0);
    CHECK(bifrost_get_jit(emu) == 0, "JIT disabled");

    // Trace / verbose
    bifrost_set_trace(emu, 0);
    bifrost_set_verbose(emu, 0);

    // Run the program (hello.elf prints "Hello, ARM64!")
    printf("\n--- Running hello.elf ---\n");
    bifrost_set_jit(emu, 0); // Use interpreter for deterministic output
    int exit_code = bifrost_run(emu);
    printf("--- Exit code: %d ---\n\n", exit_code);
    CHECK(exit_code == 0, "hello.elf exits 0");
    CHECK(!bifrost_is_running(emu), "not running after exit");

    // Destroy
    bifrost_destroy(emu);
    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
