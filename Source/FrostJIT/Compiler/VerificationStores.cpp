// Resolve store footprints for differential replay without changing guest state.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <cstdio>
#include <cstdlib>
namespace arm64emu {
void FrostJIT::record_verification_stores(const IRBlock& ir_block, uint64_t start_pc, BlockEntry& entry) {
    // ── Verify-mode memory save/restore: populate store_infos ──────
    // Walk the IR and record every STORE_MEM or ATOMIC whose address operand can
    // be statically resolved to (saved_arm_reg + offset). At verify
    // time, we use this list to snapshot the original memory values
    // before the JIT runs and restore them before the interpreter
    // re-runs the block. This eliminates false-positive divergences
    // caused by the JIT's STORE_MEM being visible to the interpreter's
    // LOAD_MEM (read-then-write-same-address pattern).
    //
    // Address tracing rules (forward dataflow over the IR):
    //   LOAD_REG v ← arm_reg:           vreg_base[v] = arm_reg, vreg_off[v] = 0
    //   IMM      v ← imm:               vreg_base[v] = 0xFF, vreg_off[v] = imm
    //   ADD      v = v1 + v2 (one IMM): vreg_base[v] = vreg_base[v1], vreg_off[v] = vreg_off[v1] + imm
    //   anything else:                  vreg_base[v] = 0xFF (unknown)
    // STORE_MEM is recordable iff vreg_base[src1] is a valid ARM reg AND
    // that ARM reg is NOT written by any STORE_REG in this block (otherwise
    // the address would use the post-modification value, not the saved one).
    {
        // Forward dataflow: vreg → (base_arm_reg, accumulated_offset).
        // base_arm_reg = 0xFF means "unknown" (not a simple base+off).
        // We ALSO track which ARM regs have been modified SO FAR (up to
        // the current instruction). A STORE_MEM is only recordable if its
        // base ARM reg has NOT been modified before the STORE_MEM. This
        // handles the common pattern where a block loads from [Xn, #imm],
        // stores to [Xn, #imm], THEN does writeback (Xn = Xn + imm). The
        // store uses the ORIGINAL Xn value, which matches the saved CPU
        // state — so we can safely save/restore. The previous whole-block
        // check was too conservative and skipped these stores, leaving
        // false-positive divergences (e.g. fcvtzu_test's block at 0x1740
        // where LDR x2,[x1] ... STR x0,[x1] ... ADD x1,x1,#4).
        std::vector<uint8_t> vreg_base(4096, 0xFF);
        std::vector<int64_t> vreg_off(4096, 0);
        uint32_t modified_so_far = 0;  // bit i set if ARM reg i has been STORE_REG'd SO FAR
        // When a STORE_MEM's base reg was modified, we can still record it if we know
        // the new absolute value. This fixes the curl URL parse divergence where
        // x19 was set to a high address (0x571c67a000) via IMM, then used as a
        // STORE_MEM base. The old code skipped this store, causing the interpreter
        // to see the JIT's modification — a false-positive PC divergence.
        std::vector<bool> arm_reg_known(32, false);
        std::vector<uint64_t> arm_reg_val(32, 0);
        // MEMFULL cannot verify blocks that CALL the interpreter or dispatch a
        // callee mid-block: the interp re-run steps exactly instr_count
        // instructions, so a BL/BLR lands it mid-callee-prologue (LR pushed on
        // the stack) while the JIT's BL_CALL completed the whole call — a false
        // memory divergence. SVC mutates guest memory via syscalls. Skip flagging
        // such blocks for has_unresolved_store so MEMFULL only diff-matches
        // straight-line store blocks (the SIMD_ST16 copy loop is the target).
        bool has_call_like = false;
        for (const auto& inst : ir_block.insts) {
            if (inst.op == IROp::BL_CALL || inst.op == IROp::BLR_CALL ||
                inst.op == IROp::CALL_INTERP || inst.op == IROp::SVC) {
                has_call_like = true;
                break;
            }
        }
        for (auto& inst : ir_block.insts) {
            if (inst.op == IROp::LOAD_REG && inst.src1 <= 31) {
                if (inst.dest < 4096) {
                    vreg_base[inst.dest] = static_cast<uint8_t>(inst.src1);
                    vreg_off[inst.dest]  = 0;
                }
            } else if (inst.op == IROp::IMM) {
                if (inst.dest < 4096) {
                    vreg_base[inst.dest] = 0xFE;  // marker: this is an IMM
                    vreg_off[inst.dest]  = static_cast<int64_t>(inst.imm_value());
                }
            } else if (inst.op == IROp::ADD) {
                // ADD v, v1, v2 — try to fold if one operand is IMM
                uint8_t b1 = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                uint8_t b2 = (inst.src2 < 4096) ? vreg_base[inst.src2] : 0xFF;
                if (b1 <= 31 && b2 == 0xFE && inst.dest < 4096) {
                    // base + imm
                    vreg_base[inst.dest] = b1;
                    vreg_off[inst.dest]  = vreg_off[inst.src1] + vreg_off[inst.src2];
                } else if (b2 <= 31 && b1 == 0xFE && inst.dest < 4096) {
                    // imm + base (commutative)
                    vreg_base[inst.dest] = b2;
                    vreg_off[inst.dest]  = vreg_off[inst.src1] + vreg_off[inst.src2];
                } else {
                    if (inst.dest < 4096) vreg_base[inst.dest] = 0xFF;
                }
            } else if (inst.op == IROp::STORE_MEM || inst.op == IROp::ATOMIC) {
                // Record if src1 traces back to an ARM reg. An atomic's
                // read-modify-write must be undone too, or the replay
                // reads the value the JIT already wrote.
                const bool atomic = inst.op == IROp::ATOMIC;
                uint8_t b = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                if (b <= 31) {
                    BlockEntry::StoreInfo si;
                    si.arm_reg = b;
                    si.offset  = vreg_off[inst.src1] +
                        (atomic ? 0 : static_cast<int64_t>(inst.store_mem_params().offset));
                    si.width   = atomic ? inst.atomic_params().width : inst.store_mem_params().width;
                    si.use_absolute = false;
                    if (!(modified_so_far & (1u << b))) {
                        // Base reg NOT modified — use saved.regs[b] + offset (original path).
                        // Already set: si.use_absolute = false.
                    } else if (arm_reg_known[b]) {
                        // Compute the absolute address and use that for snapshot/restore.
                        si.use_absolute = true;
                        si.absolute_addr = arm_reg_val[b] + static_cast<uint64_t>(si.offset);
                    } else {
                        // Base reg was modified but we don't know the new value.
                        // Skip (accept false positive).
                        if (!has_call_like) entry.has_unresolved_store = true;
                        continue;
                    }
                    if (!entry.store_infos) {
                        entry.store_infos = std::make_shared<std::vector<BlockEntry::StoreInfo>>();
                    }
                    entry.store_infos->push_back(si);
                } else {
                    // Base could not be traced to an ARM reg at all (e.g. the
                    // address is loaded from memory). Invisible to the
                    // store_infos verifier — flag for the MEMFULL full-diff.
                    if (!has_call_like) entry.has_unresolved_store = true;
                }
            } else if (inst.op == IROp::SIMD_ST16) {
                // 16-byte vector store (LDR/STR Q, LDP/STP Q, LD1/ST1).
                // Track it like STORE_MEM so the verify save/restore +
                // VERIFY_MEM diff cover vector stores: a wrong-address or
                // wrong-data `stp q` corrupts the guest heap while leaving
                // every register intact — invisible to register verify
                // (the inflate copy loop's stp q29,q28 was exactly this).
                // One StoreInfo per 16-byte chunk (flags_op = nregs).
                uint8_t b = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                const St16Params sp = inst.st16_params();
                uint32_t nregs = sp.count ? sp.count : 1;
                bool resolvable = (b <= 31);
                if (resolvable && (modified_so_far & (1u << b)) &&
                    !arm_reg_known[b]) {
                    resolvable = false;
                }
                if (!resolvable) {
                    if (!has_call_like) entry.has_unresolved_store = true;
                    static bool trace_simdst16_ = (getenv("BIFROST_TRACE_SIMDST16") != nullptr);
                    if (trace_simdst16_) {
                        const St16Params sp = inst.st16_params();
                        fprintf(stderr, "[SIMDST16] block @ 0x%llx: UNTRACKED SIMD_ST16 src2=%d flags_op=%u imm=%lld\n",
                                static_cast<unsigned long long>(start_pc),
                                inst.src2, sp.count ? sp.count : 1,
                                static_cast<long long>(sp.offset));
                    }
                } else {
                    int64_t off = vreg_off[inst.src1] + static_cast<int64_t>(inst.st16_params().offset);
                    for (uint32_t ci = 0; ci < nregs; ci++) {
                        BlockEntry::StoreInfo si;
                        si.arm_reg = b;
                        si.offset  = off + 16 * ci;
                        si.width   = 16;
                        si.use_absolute = false;
                        if ((modified_so_far & (1u << b)) && arm_reg_known[b]) {
                            si.use_absolute = true;
                            si.absolute_addr = arm_reg_val[b] + static_cast<uint64_t>(si.offset);
                        }
                        if (!entry.store_infos) {
                            entry.store_infos = std::make_shared<std::vector<BlockEntry::StoreInfo>>();
                        }
                        entry.store_infos->push_back(si);
                    }
                }
            } else if (inst.op == IROp::STORE_REG) {
                // Mark the dest ARM reg as modified FROM THIS POINT ON.
                if (inst.dest <= 31) {
                    modified_so_far |= (1u << inst.dest);
                    // new absolute value of this ARM reg.
                    uint8_t src_base = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                    if (src_base == 0xFE) {
                        arm_reg_known[inst.dest] = true;
                        arm_reg_val[inst.dest] = static_cast<uint64_t>(vreg_off[inst.src1]);
                    } else {
                        arm_reg_known[inst.dest] = false;
                    }
                }
            } else {
                // Any other op destroys the base-tracking property for dest.
                if (inst.dest < 4096 && inst.op != IROp::STORE_REG) {
                    vreg_base[inst.dest] = 0xFF;
                }
            }
        }
    }
}
} // namespace arm64emu
