// SIMD multiple-structure memory ops (LD2-LD4, ST2-ST4), native and inside the block.
//
// SIMD_STRUCT_MEM runs in memory mode: the allocator publishes the vector registers it reads and any pinned
// vector living in an XMM it clobbers (see vec_inst_effects), so v_lo/v_hi are authoritative for it. The guard
// that precedes it (emit_memory_guard) checks the whole span and, on any failure, steps the guest instruction, so the loads
// below cannot fault and a vector register changes only after every byte has been read. The post-index base
// update is separate IR the translator emits after this op.
//
// Memory holds n interleaved structures: element i of register j sits at byte (i * n + j) * size. A load gathers
// n * 16 (or n * 8) bytes into n registers, a store scatters them.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/CodeGen/Native/NativeStructure.hpp"
#include "FrostJIT/IR/IR.hpp"
#include <cstdint>
#include <cstring>
namespace arm64emu {
namespace {
// pshufb control bytes: an index selects a source byte, 0x80 gives zero.
void clear_mask(uint8_t m[16]) { std::memset(m, 0x80, 16); }

// LD4/ST4 with 8- and 16-bit elements: within one register, move each structure field's elements together so
// that lane x (4 bytes) holds field x. A 32-bit 4x4 transpose then finishes the job. inverse undoes it.
void group4_mask(int e, bool inverse, uint8_t m[16]) {
    clear_mask(m);
    const int groups = 4 / e;  // structures per register
    for (int x = 0; x < 4; x++)
        for (int g = 0; g < groups; g++)
            for (int t = 0; t < e; t++) {
                const int from = (4 * g + x) * e + t, to = x * 4 + g * e + t;
                if (inverse) m[from] = static_cast<uint8_t>(to); else m[to] = static_cast<uint8_t>(from);
            }
}

// LD2 with 8- and 16-bit elements: even elements to the low half, odd to the high half.
void split2_mask(int e, uint8_t m[16]) {
    clear_mask(m);
    for (int i = 0; i < 8 / e; i++)
        for (int t = 0; t < e; t++) {
            m[i * e + t] = static_cast<uint8_t>(2 * i * e + t);
            m[8 + i * e + t] = static_cast<uint8_t>((2 * i + 1) * e + t);
        }
}

// Generic load: result register j gathers its bytes from memory chunk s. Returns false when it takes none.
bool gather_mask(int n, int e, bool q, int j, int s, uint8_t m[16]) {
    clear_mask(m);
    bool any = false;
    for (int lane = 0; lane < (q ? 16 : 8) / e; lane++)
        for (int t = 0; t < e; t++) {
            const int at = (lane * n + j) * e + t;
            if (at / 16 == s) { m[lane * e + t] = static_cast<uint8_t>(at % 16); any = true; }
        }
    return any;
}

// Generic store: memory chunk s takes its bytes from source register j. Returns false when it takes none.
bool scatter_mask(int n, int e, bool q, int s, int j, uint8_t m[16]) {
    clear_mask(m);
    bool any = false;
    const int total = n * (q ? 16 : 8);
    for (int k = 0; k < 16 && 16 * s + k < total; k++) {
        const int element = (16 * s + k) / e, t = (16 * s + k) % e;
        if (element % n != j) continue;
        m[k] = static_cast<uint8_t>(element / n * e + t);
        any = true;
    }
    return any;
}
}  // namespace

bool FrostJIT::compile_simd_structure_(const IRInst& inst) {
    if (inst.op != IROp::SIMD_STRUCT_MEM) return false;
    const NativeStructureParams p = inst.native_structure_params();
    const int n = p.count, e = p.size, first = p.first;
    const bool q = p.q, load = p.load;
    const int total = n * (q ? 16 : 8), chunks = (total + 15) / 16;
    // XMM0-2 and XMM12-15 only: vec_inst_effects tells the allocator this op clobbers exactly these (0xF007),
    // so a loop's pinned vectors, which fill XMM3 upwards, normally survive it. x0-x2 and x12 hold the memory
    // chunks (load) or source registers (store); x13-x15 are scratch, with x15 also the mask-build scratch.
    constexpr int SRC[4] = {0, 1, 2, 12};

    clobber_flags();
    constexpr uint16_t SCRATCH = (1u << RAX) | (1u << RCX) | (1u << RDX) | (1u << R8) | (1u << R9) | (1u << R11);
    flush_invalidate_host_regs(SCRATCH);
    load_vreg_to_reg(RAX, inst.src1);
    emit_untag_data_address(RAX);

    // [window + rax + disp], full 16 bytes or the low 8.
    auto memory16 = [&](int xmm, int32_t disp, bool store) {
        emit_byte(0x66); emit_byte(rex(false, xmm >= 8, false, true));
        emit_byte(0x0F); emit_byte(store ? 0x11 : 0x10);
        emit_modrm_sib_win(xmm, RAX, disp);
    };
    auto memory8 = [&](int xmm, int32_t disp, bool store) {
        emit_byte(store ? 0x66 : 0xF3); emit_byte(rex(false, xmm >= 8, false, true));
        emit_byte(0x0F); emit_byte(store ? 0xD6 : 0x7E);
        emit_modrm_sib_win(xmm, RAX, disp);
    };
    auto chunk_access = [&](int xmm, int s, bool store) {
        if (total - 16 * s >= 16) memory16(xmm, 16 * s, store); else memory8(xmm, 16 * s, store);
    };
    // Control bytes into `xmm`; `scratch` takes the high half.
    auto load_mask = [&](int xmm, int scratch, const uint8_t m[16]) {
        uint64_t lo, hi;
        std::memcpy(&lo, m, 8); std::memcpy(&hi, m + 8, 8);
        emit_mov_imm64(RDX, lo); emit_vmovq_gpr_to_xmm(xmm, RDX);
        emit_mov_imm64(RDX, hi); emit_vmovq_gpr_to_xmm(scratch, RDX);
        sse2_op(0x6C, xmm, scratch);  // punpcklqdq
    };
    auto shuffle = [&](int xmm, int mask_xmm) { sse2_38(0x00, xmm, mask_xmm); };  // pshufb
    auto punpckl = [&](int size, int dst, int src) {
        sse2_op(size == 1 ? 0x60 : size == 2 ? 0x61 : size == 4 ? 0x62 : 0x6C, dst, src);
    };
    auto punpckh = [&](int size, int dst, int src) {
        sse2_op(size == 1 ? 0x68 : size == 2 ? 0x69 : size == 4 ? 0x6A : 0x6D, dst, src);
    };
    auto source = [&](int xmm, int reg) {  // a 64-bit register keeps only its low half
        load_lo(xmm, V_LO_OFF + reg * 8);
        if (q) load_hi(xmm, V_HI_OFF + reg * 8);
    };
    auto result = [&](int xmm, int reg) {
        store_lo(xmm, V_LO_OFF + reg * 8);
        if (q) store_hi(xmm, V_HI_OFF + reg * 8);
        else {  // a 64-bit result clears the upper half
            sse2_op(0xEF, 15, 15);  // pxor xmm15, xmm15
            store_lo(15, V_HI_OFF + reg * 8);
        }
    };
    // 4x4 transpose of the 32-bit lanes of x0, x1, x2, x12. The rows come out in x15, x13, x1, x0.
    auto transpose4 = [&]() {
        movdqa(13, 0); punpckl(4, 13, 1);   // [r0.0 r1.0 r0.1 r1.1]
        punpckh(4, 0, 1);                   // [r0.2 r1.2 r0.3 r1.3]
        movdqa(14, 2); punpckl(4, 14, 12);  // [r2.0 r3.0 r2.1 r3.1]
        punpckh(4, 2, 12);                  // [r2.2 r3.2 r2.3 r3.3]
        movdqa(15, 13); punpckl(8, 15, 14); // column 0
        punpckh(8, 13, 14);                 // column 1
        movdqa(1, 0); punpckl(8, 1, 2);     // column 2
        punpckh(8, 0, 2);                   // column 3
    };
    uint8_t mask[16];

    if (load) {
        for (int s = 0; s < chunks; s++) chunk_access(SRC[s], s, false);
        if (n == 2 && q && e == 4) {
            movdqa(13, 0); sse_fp(0xC6, 13, 1); emit_byte(0x88);  // shufps: even elements of x0:x1
            sse_fp(0xC6, 0, 1); emit_byte(0xDD);                  // odd elements
            result(13, first); result(0, first + 1);
        } else if (n == 2) {
            if (e < 8) {
                split2_mask(e, mask); load_mask(13, 15, mask);
                shuffle(0, 13);
                if (q) shuffle(1, 13);
            }
            if (q) {
                movdqa(14, 0); punpckl(8, 14, 1);  // even elements of both chunks
                punpckh(8, 0, 1);                  // odd elements
                result(14, first); result(0, first + 1);
            } else {  // one chunk: evens in the low half, odds in the high half
                movdqa(14, 0); sse2_imm(0x73, 14, 3, 8);  // psrldq xmm14, 8
                result(0, first); result(14, first + 1);
            }
        } else if (n == 4 && q && e == 8) {
            movdqa(13, 0); punpckl(8, 13, 2); punpckh(8, 0, 2);   // columns 0 and 1
            movdqa(14, 1); punpckl(8, 14, 12); punpckh(8, 1, 12); // columns 2 and 3
            result(13, first); result(0, first + 1); result(14, first + 2); result(1, first + 3);
        } else if (n == 4 && q) {
            if (e < 4) {
                group4_mask(e, false, mask); load_mask(13, 15, mask);
                for (int s = 0; s < 4; s++) shuffle(SRC[s], 13);
            }
            transpose4();
            result(15, first); result(13, first + 1); result(1, first + 2); result(0, first + 3);
        } else if (n == 4) {  // 64-bit arrangements: two chunks, two 32-bit unpacks give all four results
            if (e < 4) {
                group4_mask(e, false, mask); load_mask(13, 15, mask);
                shuffle(0, 13); shuffle(1, 13);
            }
            movdqa(13, 0); punpckl(4, 13, 1);  // low half field 0, high half field 1
            punpckh(4, 0, 1);                  // low half field 2, high half field 3
            movdqa(14, 13); sse2_imm(0x73, 14, 3, 8);  // psrldq: field 1 to the low half
            movdqa(1, 0); sse2_imm(0x73, 1, 3, 8);     // field 3
            result(13, first); result(14, first + 1); result(0, first + 2); result(1, first + 3);
        } else {
            for (int j = 0; j < n; j++) {
                bool started = false;
                for (int s = 0; s < chunks; s++) {
                    if (!gather_mask(n, e, q, j, s, mask)) continue;
                    load_mask(14, 15, mask);
                    movdqa(13, SRC[s]); shuffle(13, 14);
                    if (started) sse2_op(0xEB, 12, 13); else movdqa(12, 13);  // por
                    started = true;
                }
                result(12, first + j);
            }
        }
    } else {
        for (int j = 0; j < n; j++) source(SRC[j], first + j);
        if (n == 2) {
            if (q) {
                movdqa(13, 0); punpckl(e, 13, 1); punpckh(e, 0, 1);
                chunk_access(13, 0, true); chunk_access(0, 1, true);
            } else {
                punpckl(e, 0, 1);
                chunk_access(0, 0, true);
            }
        } else if (n == 4 && q && e == 8) {
            movdqa(13, 0); punpckl(8, 13, 1); punpckh(8, 0, 1);   // structures 0 and 1
            movdqa(14, 2); punpckl(8, 14, 12); punpckh(8, 2, 12); // structures 2 and 3
            chunk_access(13, 0, true); chunk_access(14, 1, true);
            chunk_access(0, 2, true); chunk_access(2, 3, true);
        } else if (n == 4 && q) {
            transpose4();
            const int row[4] = {15, 13, 1, 0};
            if (e < 4) {
                group4_mask(e, true, mask); load_mask(2, 12, mask);
                for (int s = 0; s < 4; s++) shuffle(row[s], 2);
            }
            for (int s = 0; s < 4; s++) chunk_access(row[s], s, true);
        } else if (n == 4) {  // 64-bit arrangements: the low halves of the sources become two grouped chunks
            movdqa(13, 0); punpckl(4, 13, 1);   // [r0.0 r1.0 r0.1 r1.1]
            movdqa(14, 2); punpckl(4, 14, 12);  // [r2.0 r3.0 r2.1 r3.1]
            movdqa(15, 13); punpckl(8, 15, 14); // chunk 0: lane 0 of each register
            punpckh(8, 13, 14);                 // chunk 1: lane 1 of each register
            if (e < 4) {
                group4_mask(e, true, mask); load_mask(0, 1, mask);
                shuffle(15, 0); shuffle(13, 0);
            }
            chunk_access(15, 0, true); chunk_access(13, 1, true);
        } else {
            for (int s = 0; s < chunks; s++) {
                bool started = false;
                for (int j = 0; j < n; j++) {
                    if (!scatter_mask(n, e, q, s, j, mask)) continue;
                    load_mask(14, 15, mask);
                    movdqa(13, SRC[j]); shuffle(13, 14);
                    if (started) sse2_op(0xEB, 12, 13); else movdqa(12, 13);
                    started = true;
                }
                chunk_access(12, s, true);
            }
        }
    }
    return true;
}
}  // namespace arm64emu
