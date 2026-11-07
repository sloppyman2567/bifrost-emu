// Atomics.cpp — native LDXR/LDAXR/STXR/STLXR/STLR sequences.
//
// The exclusive monitor is a table of bucket words (System/Core/
// ExclusiveMonitor.hpp: lock bit + version). These sequences inline its
// protocol so that an LL/SC pair costs a few dozen instructions instead of
// two C calls:
//   LDXR  : read the bucket version, then the value, then tag the CPU
//           (excl_tag_*, excl_value, excl_ver). The version comes first.
//   STXR  : tag check; lock the bucket (`lock bts`); the version must still be
//           the LDXR's; `lock cmpxchg` of the loaded value; unlock, with a
//           version bump (`add [bucket], 1`) when the write happened.
//   STLR  : lock the bucket, store, unlock with a bump.
// The access sits at a registered fault site whose stub first releases the
// bucket (the lock is held there) and then continues to the generic recovery
// stub, which steps the interpreter over the instruction under the same
// protocol. A contended lock calls jit_excl_lock_slow.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/ExclusiveMonitor.hpp"
#include "FrostJIT/IR/IR.hpp"
#include <cstddef>
#include <cstdint>

namespace arm64emu {

namespace {
constexpr int32_t kTagValid = offsetof(CPU, excl_tag_valid);
constexpr int32_t kTagAddr  = offsetof(CPU, excl_tag_addr);
constexpr int32_t kTagSize  = offsetof(CPU, excl_tag_size);
constexpr int32_t kValue    = offsetof(CPU, excl_value);
constexpr int32_t kVersion  = offsetof(CPU, excl_ver);
} // namespace

bool FrostJIT::compile_llsc_native(const IRInst& inst) {
    // Needs the recoverable guard that emit_memory_guard set up: it bounds- and
    // permission-checked the access and left the cold exit to resume at.
    if (!memory_fault_stub_ || !window_base_ || !inst.arm_pc) return false;
    const int w = inst.llsc_width();
    if (w != 1 && w != 2 && w != 4 && w != 8) return false;
    const bool exclusive = inst.llsc_exclusive();
    constexpr uint16_t MEM_CLOBBER =
        (1u << RAX) | (1u << RCX) | (1u << RDX) |
        (1u << R8)  | (1u << R9)  | (1u << R11);
    const size_t generic_stub = memory_fault_stub_;
    clobber_flags();
    flush_invalidate_host_regs(MEM_CLOBBER);
    load_vreg_to_reg(RAX, inst.src1);  // guest address

    // R9 = &bucket(RAX); clobbers RDX.
    auto bucket_address = [&] {
        emit_byte(0x89); emit_byte(modrm(3, RAX, RDX));              // mov edx, eax
        emit_shift_imm8(RDX, 5, 3);                                  // shr rdx, 3
        emit_byte(0x69); emit_byte(modrm(3, RDX, RDX));              // imul edx, edx, kHashMul
        emit_u32(ExclusiveMonitor::kHashMul);
        emit_shift_imm8(RDX, 5, 32 - ExclusiveMonitor::kBucketBits); // shr rdx, 21
        emit_shift_imm8(RDX, 4, 6);                                  // shl rdx, 6
        emit_mov_imm64(R9, reinterpret_cast<uint64_t>(&g_exclusive_monitor.buckets[0]));
        note_host_reloc(HostReloc::Image);
        emit_add_reg(R9, RDX);
    };
    auto mov_byte_cpu = [&](int32_t off, uint8_t v) {                // mov byte [rbx+off], v
        emit_byte(0xC6); emit_modrm_disp(0, CPU_REG, off); emit_byte(v);
    };
    auto unlock_keep = [&] {                                         // and qword [r9], -2
        emit_byte(0x49); emit_byte(0x83); emit_byte(0x21); emit_byte(0xFE);
    };
    auto unlock_bump = [&] {                                         // add qword [r9], 1
        emit_byte(0x49); emit_byte(0x83); emit_byte(0x01); emit_byte(1);
    };
    // Lock the bucket in R9. The uncontended path is one locked instruction.
    // Cold stubs sit ahead of the code that needs them: the fault stub (which
    // the access below registers) and the contended path.
    size_t fault_stub = 0;
    auto take_lock = [&](bool with_fault_stub) {
        const size_t over = emit_jmp_rel32_placeholder();
        if (with_fault_stub) {
            fault_stub = code_buf_used_;
            unlock_keep();
            const size_t to_generic = emit_jmp_rel32_placeholder();
            patch_jmp_rel32(to_generic, static_cast<int32_t>(generic_stub) - static_cast<int32_t>(to_generic + 5));
        }
        const size_t contended = code_buf_used_;
        emit_push(WIN_REG); emit_push(RAX); emit_push(RCX); emit_push(R9);
        emit_mov_reg(RDI, R9);
        emit_call_aligned(&jit_excl_lock_slow, 4);
        emit_pop(R9); emit_pop(RCX); emit_pop(RAX); emit_pop(WIN_REG);
        const size_t locked = emit_jmp_rel32_placeholder();
        patch_jmp_rel32(over, static_cast<int32_t>(code_buf_used_ - (over + 5)));
        emit_byte(0xF0); emit_byte(0x49); emit_byte(0x0F); emit_byte(0xBA);  // lock bts qword [r9], 0
        emit_byte(0x29); emit_byte(0x00);
        const size_t jc = emit_jcc_rel32_placeholder(2);                       // jc contended
        patch_jcc_rel32(jc, static_cast<int32_t>(contended) - static_cast<int32_t>(jc + 6));
        patch_jmp_rel32(locked, static_cast<int32_t>(code_buf_used_ - (locked + 5)));
    };

    if (inst.op == IROp::LDXR_FAST) {
        if (exclusive) {
            bucket_address();
            emit_load(RDX, R9, 0);                                   // version first,
            emit_alu_imm(RDX, 4, -2);                                // without the lock bit
            emit_store(CPU_REG, kVersion, RDX);
        }
        emit_load_mem(RCX, RAX, 0, w, false);                        // then the value (fault site)
        if (exclusive) {
            mov_byte_cpu(kTagValid, 1);
            emit_store(CPU_REG, kTagAddr, RAX);
            emit_byte(0xC7); emit_modrm_disp(0, CPU_REG, kTagSize); emit_u32(w);
            emit_store(CPU_REG, kValue, RCX);
        }
        store_reg_to_vreg(inst.dest, RCX);
    } else if (inst.op == IROp::STLR_FAST) {
        load_vreg_to_reg(RCX, inst.src2);
        bucket_address();
        take_lock(true);
        memory_fault_stub_ = fault_stub;
        emit_store_mem(RAX, 0, RCX, w);                              // fault site
        memory_fault_stub_ = generic_stub;
        unlock_bump();
    } else {  // STXR_FAST
        load_vreg_to_reg(RCX, inst.src2);
        std::vector<size_t> fail, fail_unlock;
        emit_byte(0x80); emit_modrm_disp(7, CPU_REG, kTagValid); emit_byte(0);       // cmp byte [tag_valid], 0
        fail.push_back(emit_jcc_rel32_placeholder(4));                                // je fail
        emit_byte(rex(true, false, false, false)); emit_byte(0x3B);                  // cmp rax, [tag_addr]
        emit_modrm_disp(RAX, CPU_REG, kTagAddr);
        fail.push_back(emit_jcc_rel32_placeholder(5));                                // jne fail
        emit_byte(0x83); emit_modrm_disp(7, CPU_REG, kTagSize); emit_byte(w);        // cmp dword [tag_size], w
        fail.push_back(emit_jcc_rel32_placeholder(5));
        bucket_address();
        take_lock(true);
        emit_load(RDX, R9, 0);
        emit_alu_imm(RDX, 4, -2);
        emit_byte(rex(true, false, false, false)); emit_byte(0x3B);                  // cmp rdx, [excl_ver]
        emit_modrm_disp(RDX, CPU_REG, kVersion);
        fail_unlock.push_back(emit_jcc_rel32_placeholder(5));                         // jne fail_unlock
        emit_mov_reg(R11, RAX);
        emit_untag_data_address(R11);  // the reservation keeps the tagged address, as the interpreter does
        emit_load(RAX, CPU_REG, kValue);                                              // expected = loaded value
        memory_fault_stub_ = fault_stub;
        register_memory_fault_site();
        memory_fault_stub_ = generic_stub;
        emit_byte(0xF0);                                                              // lock cmpxchg [r10+r11], rcx
        if (w == 2) emit_byte(0x66);
        emit_byte(w == 8 ? 0x4B : 0x43);
        emit_byte(0x0F); emit_byte(w == 1 ? 0xB0 : 0xB1);
        emit_byte(0x0C); emit_byte(0x1A);
        fail_unlock.push_back(emit_jcc_rel32_placeholder(5));                         // jne fail_unlock
        unlock_bump();
        emit_mov_imm32_zext(RDX, 0);
        mov_byte_cpu(kTagValid, 0);
        const size_t done = emit_jmp_rel32_placeholder();
        const size_t unlock_at = code_buf_used_;
        for (size_t f : fail_unlock) patch_jcc_rel32(f, static_cast<int32_t>(unlock_at - (f + 6)));
        unlock_keep();
        const size_t fail_at = code_buf_used_;
        for (size_t f : fail) patch_jcc_rel32(f, static_cast<int32_t>(fail_at - (f + 6)));
        mov_byte_cpu(kTagValid, 0);
        emit_mov_imm32_zext(RDX, 1);
        patch_jmp_rel32(done, static_cast<int32_t>(code_buf_used_ - (done + 5)));
        store_reg_to_vreg(inst.dest, RDX);
    }
    invalidate_host_regs(MEM_CLOBBER);
    return true;
}

} // namespace arm64emu
