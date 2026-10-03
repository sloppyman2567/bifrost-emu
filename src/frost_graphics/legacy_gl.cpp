#include "legacy_gl.hpp"
#include <ffi.h>
#include <cstring>
#include <vector>
#include <stdexcept>

namespace arm64emu {
namespace {
uint32_t evaluator_components(uint32_t target, bool two_dimensional) {
    const uint32_t base = two_dimensional ? 0x0DB0 : 0x0D90;
    static constexpr uint32_t components[] = {4, 1, 3, 1, 2, 3, 4, 3, 4};
    return target >= base && target < base + 9 ? components[target - base] : 0;
}
uint32_t color_table_pixel_bytes(uint32_t format, uint32_t type) {
    uint32_t components;
    switch (format) {
        case 0x1907: case 0x80E0: components = 3; break; // RGB/BGR
        case 0x1908: case 0x80E1: components = 4; break; // RGBA/BGRA
        case 0x190A: components = 2; break; // LUMINANCE_ALPHA
        case 0x1900: case 0x1903: case 0x1904: case 0x1905:
        case 0x1906: case 0x1909: components = 1; break;
        default: return 0;
    }
    switch (type) {
        case 0x1400: case 0x1401: return components;
        case 0x1402: case 0x1403: return components * 2;
        case 0x1404: case 0x1405: case 0x1406: return components * 4;
        case 0x8032: case 0x8362: return 1;
        case 0x8363: case 0x8364: case 0x8033: case 0x8365:
        case 0x8034: case 0x8366: return 2;
        case 0x8035: case 0x8367: case 0x8036: case 0x8368: return 4;
        default: return 0;
    }
}
}
void dispatch_legacy_gl(Memory& mem, CPU& cpu, const thunk::Spec& spec, void* host_fn) {
    union Value { uint64_t guest; uint32_t integer; float single; double dbl; void* ptr; };
    constexpr size_t kMaxArgs = 12, kMaxBytes = 16u << 20;
    Value values[kMaxArgs]{};
    ffi_type* types[kMaxArgs]{};
    void* arguments[kMaxArgs]{};
    uint64_t guest_pointers[kMaxArgs]{};
    std::vector<uint8_t> buffers[kMaxArgs];
    bool output[kMaxArgs]{};
    const size_t nargs = std::strlen(spec.args);
    if (nargs > kMaxArgs) throw std::runtime_error("legacy GL signature exceeds argument capacity");
    unsigned gpr = 0, fp = 0;
    for (size_t i = 0; i < nargs; ++i) {
        arguments[i] = &values[i];
        switch (spec.args[i]) {
            case 'f':
                if (fp >= 8) throw std::runtime_error("legacy GL FP stack arguments unsupported");
                std::memcpy(&values[i].single, &cpu.v_lo[fp++], sizeof(float));
                types[i] = &ffi_type_float;
                break;
            case 'd':
                if (fp >= 8) throw std::runtime_error("legacy GL FP stack arguments unsupported");
                std::memcpy(&values[i].dbl, &cpu.v_lo[fp++], sizeof(double));
                types[i] = &ffi_type_double;
                break;
            default: {
                uint64_t raw;
                if (gpr < 8) raw = cpu.regs[gpr];
                else mem.read(cpu.sp + (gpr - 8) * 8, &raw, sizeof(raw));
                ++gpr;
                if (spec.args[i] == 'p' || spec.args[i] == 'z') {
                    guest_pointers[i] = raw;
                    types[i] = &ffi_type_pointer;
                } else {
                    values[i].integer = static_cast<uint32_t>(raw);
                    types[i] = &ffi_type_uint32;
                }
                break;
            }
        }
    }
    auto integer = [&](size_t i) { return static_cast<int32_t>(values[i].integer); };
    auto domain = [&](size_t i) { return spec.args[i] == 'd' ? values[i].dbl : values[i].single; };
    for (size_t i = 0; i < nargs; ++i) {
        if (types[i] != &ffi_type_pointer) continue;
        uint64_t bytes = 0;
        const char* name = spec.name;
        if (!std::strcmp(name, "glMap1f") || !std::strcmp(name, "glMap1d")) {
            const uint32_t components = evaluator_components(values[0].integer, false);
            const int32_t stride = integer(3), order = integer(4);
            if (components && stride >= static_cast<int32_t>(components) && order > 0 && domain(1) != domain(2)) {
                const uint64_t elements = static_cast<uint64_t>(order - 1) * stride + components;
                const size_t element_bytes = spec.args[1] == 'd' ? sizeof(double) : sizeof(float);
                if (elements > kMaxBytes / element_bytes) { cpu.regs[0] = 0; return; }
                bytes = elements * element_bytes;
            }
        } else if (!std::strcmp(name, "glMap2f") || !std::strcmp(name, "glMap2d")) {
            const uint32_t components = evaluator_components(values[0].integer, true);
            const int32_t us = integer(3), uo = integer(4), vs = integer(7), vo = integer(8);
            if (components && us >= static_cast<int32_t>(components) && vs >= static_cast<int32_t>(components) &&
                uo > 0 && vo > 0 && domain(1) != domain(2) && domain(5) != domain(6)) {
                // Check the element bound before byte multiplication (two
                // INT_MAX products can otherwise overflow a uint64_t).
                const uint64_t elements = static_cast<uint64_t>(uo - 1) * us +
                                          static_cast<uint64_t>(vo - 1) * vs + components;
                if (elements > kMaxBytes / (spec.args[1] == 'd' ? 8 : 4)) { cpu.regs[0] = 0; return; }
                bytes = elements * (spec.args[1] == 'd' ? 8 : 4);
            }
        } else if (!std::strcmp(name, "glProgramStringARB")) {
            if (integer(2) > 0) bytes = static_cast<uint32_t>(integer(2));
        } else if (!std::strcmp(name, "glGenProgramsARB")) {
            if (integer(0) > 0) bytes = static_cast<uint64_t>(integer(0)) * sizeof(uint32_t);
            output[i] = true;
        } else if (!std::strcmp(name, "glProgramEnvParameter4fvARB") ||
                   !std::strcmp(name, "glProgramLocalParameter4fvARB")) {
            bytes = 4 * sizeof(float);
        } else if (!std::strcmp(name, "glColorTableEXT")) {
            if (integer(2) > 0)
                bytes = static_cast<uint64_t>(integer(2)) *
                        color_table_pixel_bytes(values[3].integer, values[4].integer);
        } else throw std::runtime_error("legacy GL pointer lacks an extent policy");
        if (bytes > kMaxBytes) { cpu.regs[0] = 0; return; }
        // Invalid GL parameters need no buffer access; the driver reports
        // their GL error. A zero length also uses nullptr, as native GL does.
        values[i].ptr = nullptr;
        if (bytes && guest_pointers[i]) {
            buffers[i].resize(static_cast<size_t>(bytes));
            if (!output[i]) mem.read(guest_pointers[i], buffers[i].data(), buffers[i].size());
            else mem.check_access(guest_pointers[i], buffers[i].size(), Memory::GUEST_PROT_WRITE);
            values[i].ptr = buffers[i].data();
        } else if (bytes) { cpu.regs[0] = 0; return; }
    }
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, static_cast<unsigned>(nargs), &ffi_type_void, types) != FFI_OK)
        throw std::runtime_error("legacy GL ffi signature rejected");
    ffi_call(&cif, FFI_FN(host_fn), nullptr, arguments);
    for (size_t i = 0; i < nargs; ++i)
        if (output[i] && !buffers[i].empty()) mem.write(guest_pointers[i], buffers[i].data(), buffers[i].size());
    cpu.regs[0] = 0;
}
}
