// test_capi.c — Test the C API (libbifrost.a + bifrost.h).
// Verifies: create, load, run, register access, FP access, flags, memory,
// step_n, error reporting, version, JIT config/verify, breakpoints,
// bifrost_call, svc hook, symbol lookup. Compiles as pure C (host-side,
// links libbifrost.a).
#include "bifrost.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

// AArch64 machine-code stubs written into guest memory via bifrost_write_mem.
//   stub_add:  add x0, x0, x1;  ret
//   stub_fadd: fadd d0, d0, d1; ret
static const uint32_t stub_add[] = {0x8b010000, 0xd65f03c0};
static const uint32_t stub_fadd[] = {0x1e612800, 0xd65f03c0};
// getpid: mov x8, #172; svc #0; ret
static const uint32_t stub_getpid[] = {0xd2801588, 0xd4000001, 0xd65f03c0};

static void* svc_hook_ud = NULL;
static uint64_t svc_hook_calls = 0;
static uint64_t svc_hook_last_num = 0;
static int svc_hook_intercept = 0; // 1 = handle getpid(172) ourselves

static int svc_hook(void* ud, uint64_t num, const uint64_t* args,
                    uint64_t* result) {
    (void)args;
    svc_hook_ud = ud;
    svc_hook_calls++;
    svc_hook_last_num = num;
    if (svc_hook_intercept && num == 172) { // getpid
        *result = 0xCAFEBABE;
        return 1;
    }
    return 0; // let the emulator handle it
}

int main(int argc, const char* argv[]) {
    printf("=== C API Test (libbifrost.a) ===\n");
    printf("Version: %s\n\n", bifrost_version());

    // Create
    bifrost_emu_t* emu = bifrost_create();
    CHECK(emu != NULL, "bifrost_create()");

    // JIT is ON by default (matches bifrost.h docs + main.cpp)
    CHECK(bifrost_get_jit(emu) == 1, "JIT enabled by default");

    // Load hello.elf
    int r = bifrost_load_elf(emu, "ctest/hello.elf", argc, argv);
    CHECK(r == 0, "bifrost_load_elf(\"ctest/hello.elf\")");

    // Check version (non-empty, "major.minor-tag" shaped — don't hardcode
    // the number here or every version bump breaks this test)
    const char* ver = bifrost_version();
    CHECK(ver && ver[0] && strchr(ver, '.') != NULL, "version string sane");

    // Check initial state
    CHECK(bifrost_is_running(emu), "is_running after load");

    // Get PC (should be non-zero after ELF load)
    uint64_t pc = bifrost_get_pc(emu);
    CHECK(pc != 0, "get_pc() non-zero after load");
    uint64_t entry_pc = pc; // remember the entry point for the final run

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

    // JIT verify (sets/clears BIFROST_JIT_VERIFY env before first dispatch)
    bifrost_set_jit_verify(emu, 1);
    CHECK(getenv("BIFROST_JIT_VERIFY") != NULL, "set_jit_verify(1) sets env");
    bifrost_set_jit_verify(emu, 0);
    CHECK(getenv("BIFROST_JIT_VERIFY") == NULL, "set_jit_verify(0) clears env");

    // Trace / verbose
    bifrost_set_trace(emu, 0);
    bifrost_set_verbose(emu, 0);

    // ── Breakpoints ──────────────────────────────────────────────────────
    // Set a breakpoint at the current PC: step should stop immediately
    // (return 1) BEFORE executing the instruction there.
    CHECK(bifrost_set_breakpoint(emu, pc) == 0, "set_breakpoint(pc)");
    uint64_t before = bifrost_get_pc(emu);
    int sr = bifrost_step(emu);
    CHECK(sr == 1, "step() returns 1 at breakpoint");
    CHECK(bifrost_get_pc(emu) == before, "PC unchanged at breakpoint hit");

    // Removing it lets stepping proceed normally.
    CHECK(bifrost_remove_breakpoint(emu, pc) == 0, "remove_breakpoint(pc)");
    sr = bifrost_step(emu);
    CHECK(sr == 0, "step() returns 0 after breakpoint removed");
    CHECK(bifrost_get_pc(emu) == before + 4, "PC advanced past breakpoint");
    CHECK(bifrost_set_breakpoint(emu, 0xFFFFFFFFFFFFFFFFULL) == 0,
          "set_breakpoint(unreachable addr) ok");
    CHECK(bifrost_remove_breakpoint(emu, 0xFFFFFFFFFFFFFFFFULL) == 0,
          "remove_breakpoint(unreachable addr) ok");
    CHECK(bifrost_remove_breakpoint(NULL, pc) == -1, "remove_breakpoint(NULL) fails");
    CHECK(bifrost_set_breakpoint(NULL, pc) == -1, "set_breakpoint(NULL) fails");

    // ── bifrost_lookup_symbol ────────────────────────────────────────────
    CHECK(bifrost_lookup_symbol(emu, NULL) == 0, "lookup_symbol(NULL) -> 0");
    CHECK(bifrost_lookup_symbol(emu, "") == 0, "lookup_symbol(\"\") -> 0");
    CHECK(bifrost_lookup_symbol(emu, "definitely_not_a_symbol_xyz") == 0,
          "lookup_symbol(unknown) -> 0 (no crash)");
    CHECK(bifrost_lookup_symbol(NULL, "foo") == 0, "lookup_symbol(NULL emu) -> 0");

    // ── bifrost_call (guest function invocation) ─────────────────────────
    // Find a writable guest address inside the stack (sp - 4096). Write the
    // two stubs there and invoke them via the borrow-CPU path.
    uint64_t stub_addr = sp - 4096;
    CHECK(bifrost_write_mem(emu, stub_addr, stub_add, sizeof(stub_add)) == 0,
          "write_mem(add stub)");
    CHECK(bifrost_write_mem(emu, stub_addr + 8, stub_fadd, sizeof(stub_fadd)) == 0,
          "write_mem(fadd stub)");
    CHECK(bifrost_write_mem(emu, stub_addr + 16, stub_getpid, sizeof(stub_getpid)) == 0,
          "write_mem(getpid stub)");

    // Integer call: add x0,x0,x1 with args {7, 35} -> 42
    int64_t iargs[2] = {7, 35};
    uint64_t res = bifrost_call(emu, stub_addr, iargs, 2, NULL, 0);
    CHECK(res == 42, "bifrost_call(add, {7,35}) == 42");

    // FP call: fadd d0,d0,d1 with {1.5, 2.25} -> 3.75
    double fargs[2] = {1.5, 2.25};
    double fres = bifrost_call_f(emu, stub_addr + 8, NULL, 0, fargs, 2);
    CHECK(fres == 3.75, "bifrost_call_f(fadd, {1.5,2.25}) == 3.75");

    // syscall stub: getpid() through the emulator (no hook yet)
    res = bifrost_call(emu, stub_addr + 16, NULL, 0, NULL, 0);
    CHECK(res > 0, "bifrost_call(getpid) returns a positive pid");

    // ── SVC hook ─────────────────────────────────────────────────────────
    CHECK(bifrost_set_svc_hook(NULL, svc_hook, &svc_hook_ud) == -1,
          "set_svc_hook(NULL) fails");
    bifrost_set_svc_hook(emu, svc_hook, &svc_hook_ud);
    // Intercept getpid(172): the emulator must call our hook, which returns
    // 0xCAFEBABE without dispatching to the guest syscall layer.
    svc_hook_intercept = 1;
    res = bifrost_call(emu, stub_addr + 16, NULL, 0, NULL, 0);
    CHECK(res == 0xCAFEBABE, "svc hook intercepts getpid -> 0xCAFEBABE");
    CHECK(svc_hook_calls >= 1, "svc hook was called");
    CHECK(svc_hook_last_num == 172, "svc hook saw syscall 172");
    CHECK(svc_hook_ud == &svc_hook_ud, "svc hook received userdata");
    // Let the emulator handle a syscall normally (write syscall via interp).
    svc_hook_intercept = 0;
    uint64_t calls_before = svc_hook_calls;
    res = bifrost_call(emu, stub_addr + 16, NULL, 0, NULL, 0);
    CHECK(res > 0, "svc hook passthrough still returns a pid");
    CHECK(svc_hook_calls == calls_before + 1, "svc hook observed passthrough syscall");
    // Clear the hook.
    bifrost_set_svc_hook(emu, NULL, NULL);
    res = bifrost_call(emu, stub_addr + 16, NULL, 0, NULL, 0);
    CHECK(res > 0, "cleared svc hook -> emulator handles getpid");

    // ── Guest function call state restoration ────────────────────────────
    // After bifrost_call, PC/SP/regs must be restored to pre-call values.
    CHECK(bifrost_get_pc(emu) == before + 4, "PC restored after bifrost_call");

    // Reset PC to the real entry point: the breakpoint tests above stepped
    // one instruction past _start.
    bifrost_set_pc(emu, entry_pc);

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
