// Classify hexadecimal AArch64 words from stdin with the real IR translator.
// TRANSLATED excludes explicit IR fallback, not conditional codegen fallback.
#include "decoder.hpp"
#include "ir/ir.hpp"
#include <iostream>
using namespace arm64emu;
int main() {
    uint32_t opcode;
    while (std::cin >> std::hex >> opcode) {
        DecodedInst decoded{};
        bool valid = decode(decoded, opcode);
        IRBlock block{};
        ir_reset_vreg_alloc();
        if (valid) translate_to_ir(block, decoded, 0x10000);
        bool fallback = false;
        for (const auto& inst : block.insts)
            if (inst.op == IROp::CALL_INTERP) fallback = true;
        std::cout << std::hex << opcode << '\t'
                  << (valid ? (fallback ? "IR_INTERPRETER" : "TRANSLATED")
                            : "DECODE_FAILED") << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
