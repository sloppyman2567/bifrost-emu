// The emitters' out-of-line call sequences, run on their own: emit_call_aligned keeps RFLAGS and aligns the
// call for every push count, emit_store_mem's slow path passes the value for any source register, and
// emit_load_mem's slow path sign-extends like its fast path. Each sequence is emitted into a FrostJIT code
// buffer, the call is retargeted at a recording helper, and the result is compared with what the fast path
// (or the plain x86 semantics) gives. Built with -fno-access-control: the emitters are private.
#include "System/Core/Emulator.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace arm64emu;

namespace arm64emu {
extern "C" uint64_t jit_load_mem_slow(Emulator*, CPU*, uint64_t, int);
extern "C" void jit_store_mem_slow(Emulator*, CPU*, uint64_t, uint64_t, int);
}
extern "C" {
uint64_t g_call_rsp, g_out[4], g_addr, g_val, g_load_val;
int g_w, g_calls;
void *g_emu, *g_cpu;
void fake_store(Emulator* e, CPU* c, uint64_t addr, uint64_t val, int w) {
    g_calls++; g_emu = e; g_cpu = c; g_addr = addr; g_val = val; g_w = w;
}
uint64_t fake_load(Emulator*, CPU*, uint64_t, int w) {   // zero-extended, like the real helper
    g_calls++;
    return w == 8 ? g_load_val : (g_load_val & ((1ULL << (8 * w)) - 1));
}
}
// Records RSP at the call instruction, then clobbers RFLAGS (xor), so only a restore can keep them.
asm(".text\n.globl probe_fn\nprobe_fn:\n lea 8(%rsp),%rax\n mov %rax,g_call_rsp(%rip)\n xor %eax,%eax\n ret\n");
extern "C" void probe_fn();

constexpr int RAX = FrostJIT::RAX, RCX = FrostJIT::RCX, RDX = FrostJIT::RDX, RBX = FrostJIT::RBX;
constexpr int RSI = 6, RDI = 7, R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL line %d: ", __LINE__); \
                                       std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Harness {
    FrostJIT j;
    size_t start = 0;
    // fn(cpu, window): RBX = cpu, R10 = window as in generated code. Four pushes keep RSP%16 == 8,
    // the body-entry state the emitters assume; R12/R13 are saved because the cases use them.
    void begin() {
        start = j.code_buf_used_;
        j.emit_push(RBX); j.emit_push(R10); j.emit_push(R12); j.emit_push(R13);
        j.emit_mov_reg(RBX, RDI);
        j.emit_mov_reg(R10, RSI);
    }
    void end_and_run(CPU* cpu, void* window) {
        j.emit_pop(R13); j.emit_pop(R12); j.emit_pop(R10); j.emit_pop(RBX);
        j.emit_ret();
        reinterpret_cast<void (*)(CPU*, void*)>(j.code_buf_ + start)(cpu, window);
    }
    void retarget(const void* from, const void* to) {
        const uint64_t a = reinterpret_cast<uint64_t>(from), b = reinterpret_cast<uint64_t>(to);
        int found = 0;
        for (size_t i = start; i + 8 <= j.code_buf_used_; ++i) {
            uint64_t v; std::memcpy(&v, j.code_buf_ + i, 8);
            if (v == a) { std::memcpy(j.code_buf_ + i, &b, 8); ++found; }
        }
        CHECK(found == 1, "helper call found %d times", found);
    }
    void record(int reg, int slot) { j.emit_store(R9, 8 * slot, reg); }   // after R9 holds &g_out
};

static void test_call_aligned(Harness& h) {
    struct Case { const char* name; std::vector<uint8_t> set; uint64_t zf, cf, sf; };
    const Case cases[] = {
        {"ZF", {0x31, 0xC0}, 1, 0, 0},                               // xor eax,eax
        {"CF+SF", {0xB8, 1, 0, 0, 0, 0x83, 0xF8, 2}, 0, 1, 1},       // mov eax,1; cmp eax,2
    };
    for (const Case& c : cases) for (int pushes = 0; pushes < 4; ++pushes) {
        h.begin();
        for (uint8_t b : c.set) h.j.emit_byte(b);
        for (int i = 0; i < pushes; ++i) h.j.emit_push(R11);
        h.j.emit_call_aligned(reinterpret_cast<void*>(&probe_fn), pushes);
        for (int i = 0; i < pushes; ++i) h.j.emit_pop(R11);
        h.j.emit_byte(0x9C); h.j.emit_pop(RAX);                      // pushfq; pop rax
        h.j.emit_mov_imm64(R9, reinterpret_cast<uint64_t>(&g_out[0]));
        h.record(RAX, 0);
        CPU cpu{};
        g_call_rsp = 1; g_out[0] = 0;
        h.end_and_run(&cpu, nullptr);
        const uint64_t f = g_out[0];
        CHECK(((f >> 6) & 1) == c.zf && (f & 1) == c.cf && ((f >> 7) & 1) == c.sf,
              "%s, %d pushes: RFLAGS 0x%llx not preserved", c.name, pushes, (unsigned long long)f);
        CHECK((g_call_rsp & 15) == 0, "%s, %d pushes: RSP %% 16 = %llu at the call", c.name, pushes,
              (unsigned long long)(g_call_rsp & 15));
    }
}

static void test_store_slow(Harness& h) {
    // Any source register but R8/R9 (the scratch pair), including RDX and RCX, the argument registers.
    const int srcs[] = {RAX, RCX, RDX, R11, R12, R13};
    const int addrs[] = {RAX, RCX, RDX, R11, R12};
    constexpr uint64_t A = 0x5000, V = 0x1122334455667788ULL;
    for (int w : {1, 2, 4, 8}) for (int off : {0, 16}) for (int a : addrs) for (int s : srcs) {
        h.begin();
        h.j.emit_mov_imm64(a, A);
        if (s != a) h.j.emit_mov_imm64(s, V);
        h.j.emit_store_mem(a, off, s, w);
        h.j.emit_mov_imm64(R9, reinterpret_cast<uint64_t>(&g_out[0]));
        h.record(s, 0); h.record(R10, 1);
        h.retarget(reinterpret_cast<void*>(&arm64emu::jit_store_mem_slow), reinterpret_cast<void*>(&fake_store));
        CPU cpu{}; cpu.jit_emu = reinterpret_cast<Emulator*>(0x1234);
        g_calls = 0; g_addr = g_val = 0; g_w = 0; g_emu = g_cpu = nullptr;
        h.end_and_run(&cpu, reinterpret_cast<void*>(0x2222));
        const uint64_t want = (s == a) ? A : V;
        CHECK(g_calls == 1, "store addr=%d src=%d: %d helper calls", a, s, g_calls);
        CHECK(g_addr == A + off && g_w == w && g_emu == reinterpret_cast<void*>(0x1234) && g_cpu == &cpu,
              "store addr=%d src=%d off=%d w=%d: bad address or arguments", a, s, off, w);
        CHECK(g_val == want, "store addr=%d src=%d w=%d: value 0x%llx, want 0x%llx", a, s, w,
              (unsigned long long)g_val, (unsigned long long)want);
        if (s != RAX) CHECK(g_out[0] == want, "store addr=%d src=%d: source clobbered", a, s);
        CHECK(g_out[1] == 0x2222, "store addr=%d src=%d: R10 not restored", a, s);
    }
}

static void test_load_slow(Harness& h) {
    const int dsts[] = {RAX, RCX, RDX, R8, R11, R13};
    const int addrs[] = {RAX, R12};
    const uint64_t vals[] = {0xDEADBEEFCAFE8081ULL, 0x0123456789ABCDEFULL, 0xFFFFFFFF7FFF7F7FULL};
    alignas(16) uint8_t window[64];
    for (uint64_t lv : vals) for (int w : {1, 2, 4, 8}) for (bool sx : {false, true}) for (int a : addrs) for (int d : dsts) {
        g_load_val = lv;
        std::memset(window, 0, sizeof window); std::memcpy(window, &lv, 8);
        uint64_t got[2] = {0, 0};
        for (int slow = 0; slow < 2; ++slow) {      // address 0 is inside the (empty) test window: the fast path
            h.begin();
            h.j.emit_mov_imm64(a, slow ? 0x5000 : 0);
            if (d != a) h.j.emit_mov_imm64(d, 0xCCCCCCCCCCCCCCCCULL);
            h.j.emit_load_mem(d, a, 0, w, sx);
            h.j.emit_mov_imm64(R9, reinterpret_cast<uint64_t>(&g_out[0]));
            h.record(d, 0);
            h.retarget(reinterpret_cast<void*>(&arm64emu::jit_load_mem_slow), reinterpret_cast<void*>(&fake_load));
            CPU cpu{}; cpu.jit_emu = reinterpret_cast<Emulator*>(0x1234);
            g_calls = 0;
            h.end_and_run(&cpu, window);
            CHECK(g_calls == slow, "load slow=%d: %d helper calls", slow, g_calls);
            got[slow] = g_out[0];
        }
        CHECK(got[0] == got[1], "load w=%d sext=%d addr=%d dst=%d val=0x%llx: fast 0x%llx, slow 0x%llx", w, sx, a, d,
              (unsigned long long)lv, (unsigned long long)got[0], (unsigned long long)got[1]);
    }
}

int main() {
    Harness h;
    if (!h.j.code_buf_) { std::puts("jit_emitter_slow_paths: no code buffer"); return 2; }
    test_call_aligned(h);
    test_store_slow(h);
    test_load_slow(h);
    if (failures) { std::printf("jit_emitter_slow_paths: %d FAILED\n", failures); return 1; }
    std::puts("jit_emitter_slow_paths: ALL PASS");
    return 0;
}
