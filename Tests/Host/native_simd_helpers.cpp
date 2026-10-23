// Native SIMD helper bodies against the QEMU hashes in native_simd_expected.txt.
//
// The JIT inlines most native kinds, so the generated guest fixture
// (Tests/Unit/jit_native_simd_gen.c) mostly exercises the inline emitters. The
// run<KIND> helpers still serve what an emitter declines (scalar forms,
// 64-bit signed shifts, hosts without SSE4.1), so this test calls every helper
// directly over the fixture's inputs. fill() and the hash must match that
// fixture (Tools/CodeGen/nativegen.py, TEST template).
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include "Generated/NativeSimdTable.hpp"
#include "System/Core/CPU.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace arm64emu;

namespace {
struct V { uint8_t b[16]; };
const uint32_t f32s[] = {
    0x00000000, 0x80000000, 0x3fc00000, 0x40200000, 0xc0200000, 0x3f000000,
    0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001, 0xffc00002, 0x7f7fffff,
    0x00000001, 0x501502f9, 0x4b000001, 0xcb7fffff,
};
const uint64_t f64s[] = {
    0, 0x8000000000000000ull, 0x3ff8000000000000ull, 0xc004000000000000ull,
    0x3fe0000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff8000000000001ull, 0x7ff0000000000001ull, 0xfff8000000000002ull,
    0x7fefffffffffffffull, 0x0000000000000001ull, 0x4330000000000001ull,
    0x433fffffffffffffull, 0xc3e0000000000000ull, 0x43e0000000000000ull,
};
const uint16_t f16s[] = {
    0x0000, 0x8000, 0x3c00, 0x3e00, 0xc100, 0x7c00, 0xfc00, 0x7e01, 0x7c01,
    0xfe02, 0x7bff, 0x0001, 0x83ff, 0x0400, 0x3555, 0xd2a3,
};

void fill(V* v, unsigned k) {
    for (unsigned lane = 0; lane < 16; ++lane) {
        const unsigned sel = k * 7 + lane * (k % 5 + 1) + k / 3;
        switch (k % 4) {
            case 0: v->b[lane] = static_cast<uint8_t>(sel * 37 + k * 11 + (k & 1 ? 0x80 : 0)); break;
            case 1: if (lane < 8) std::memcpy(v->b + 2 * lane, &f16s[sel % 16], 2); break;
            case 2: if (lane < 4) std::memcpy(v->b + 4 * lane, &f32s[sel % 16], 4); break;
            default: if (lane < 2) std::memcpy(v->b + 8 * lane, &f64s[sel % 16], 8); break;
        }
    }
}

uint32_t helper_hash(NativeSimdFn fn, uint32_t word, CPU* cpu) {
    uint32_t h = 2166136261u;
    for (unsigned dn = 0; dn < 2; ++dn) {
        cpu->fpcr = dn ? 1u << 25 : 0;
        for (unsigned k = 0; k < 64; ++k) {
            V n, m, d, out;
            fill(&n, k);
            fill(&m, k + 17);
            for (int i = 0; i < 16; ++i) d.b[i] = static_cast<uint8_t>(0xa5 ^ (i * 29 + k));
            std::memcpy(&cpu->v_lo[0], n.b, 8); std::memcpy(&cpu->v_hi[0], n.b + 8, 8);
            std::memcpy(&cpu->v_lo[1], m.b, 8); std::memcpy(&cpu->v_hi[1], m.b + 8, 8);
            std::memcpy(&cpu->v_lo[2], d.b, 8); std::memcpy(&cpu->v_hi[2], d.b + 8, 8);
            std::memcpy(&cpu->regs[0], n.b, 8);
            fn(cpu, word);
            std::memcpy(out.b, &cpu->v_lo[2], 8); std::memcpy(out.b + 8, &cpu->v_hi[2], 8);
            for (int i = 0; i < 16; ++i) h = (h ^ out.b[i]) * 16777619u;
        }
    }
    return h ? h : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s native_simd_expected.txt\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "r");
    if (!f) { std::perror(argv[1]); return 2; }
    auto cpu = std::make_unique<CPU>();
    unsigned checked = 0, fails = 0;
    unsigned word, hash;
    while (std::fscanf(f, "%x %x", &word, &hash) == 2) {
        const NativeSimdKind kind = classify_native_simd(word);
        if (kind == NativeSimdKind::None) continue;
        NativeSimdFn fn = native_simd_helper(kind);
        if (!fn) { std::printf("FAIL %08x: no helper\n", word); ++fails; continue; }
        const uint32_t got = helper_hash(fn, word, cpu.get());
        ++checked;
        if (got != hash) {
            if (++fails <= 20) std::printf("FAIL %08x helper=%08x qemu=%08x\n", word, got, hash);
        }
    }
    std::fclose(f);
    if (fails) { std::printf("native_simd_helpers: %u of %u helper encodings FAILED\n", fails, checked); return 1; }
    std::printf("native_simd_helpers: %u helper encodings passed\n", checked);
    return 0;
}
