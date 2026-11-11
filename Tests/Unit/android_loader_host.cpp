#include "System/Core/Emulator.hpp"
#include "Interpreter/Decoder/Decoder.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

using namespace arm64emu;
template<class T> static void put(std::vector<uint8_t>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
static std::vector<uint8_t> image(uint64_t alignment) {
    std::vector<uint8_t> bytes(4096, 0);
    std::memcpy(bytes.data(), "\177ELF\2\1\1", 7);
    put<uint16_t>(bytes,16,3); put<uint16_t>(bytes,18,183);
    put<uint32_t>(bytes,20,1); put<uint64_t>(bytes,24,256);
    put<uint64_t>(bytes,32,64); put<uint16_t>(bytes,52,64);
    put<uint16_t>(bytes,54,56); put<uint16_t>(bytes,56,1);
    put<uint32_t>(bytes,64,1); put<uint32_t>(bytes,68,5); // PT_LOAD, R+X
    put<uint64_t>(bytes,96,4096); put<uint64_t>(bytes,104,8192);
    put<uint64_t>(bytes,112,alignment);
    put<uint32_t>(bytes,256,0xd65f03c0); // ret
    return bytes;
}
static void rejects(const std::vector<uint8_t>& bytes) {
    Memory memory(false);
    bool failed = false;
    try { ElfLoader::load(memory, bytes); }
    catch (const EmuError&) { failed = true; }
    assert(failed && !memory.is_mapped(memory.pie_base(), 1));
}
// Tagged data accesses stay native under the JIT (a guard that compared the
// tagged address with the window bound stepped each one in the interpreter)
// and match the interpreter: loads, stores, pairs, Q stores with writeback,
// a page-crossing store, LSE and LL/SC. The base register keeps its tag.
struct TaggedRun { uint64_t regs[12]; uint64_t q1[2]; uint8_t data[8192]; uint64_t steps; };
static TaggedRun run_tagged(bool jit, uint64_t tag) {
    constexpr uint64_t code = 0x90000, stop = 0xa0000, data = 0x100000;
    static const uint32_t words[] = {
        0xf9000001, 0xf9000401, 0xf9400402, 0xa9010801, // str x1,[x0]; str x1,[x0,#8]; ldr x2,[x0,#8]; stp x1,x2,[x0,#16]
        0xa9411404, 0x39008001, 0x79404006, 0x3d800c00, // ldp x4,x5,[x0,#16]; strb w1,[x0,#32]; ldrh w6,[x0,#32]; str q0,[x0,#48]
        0xad820000, 0x3cdf0001, 0xf8210007, 0x913ff00a, // stp q0,q0,[x0,#64]!; ldur q1,[x0,#-16]; ldadd x1,x7,[x0]; add x10,x0,#0xffc
        0xf9000141, 0xb940014b, 0xc85f7c08, 0x91000508, // str x1,[x10] (crosses a page); ldr w11,[x10]; 1: ldxr x8,[x0]; add x8,x8,#1
        0xc8097c08, 0x35ffffc9, 0xd65f03c0};            // stxr w9,x8,[x0]; cbnz w9,1b; ret
    auto e = std::make_unique<Emulator>();
    e->mem().set_data_top_byte_ignore(true);
    assert(e->mem().map_range(code, 4096) && e->mem().map_range(data, 8192));
    e->mem().write(code, words, sizeof(words));
    assert(e->mem().mprotect_guest(code, 4096, 5));
    CPU cpu;
    cpu.pc = code; cpu.regs[0] = (tag << 56) | data; cpu.regs[1] = 0x1122334455667788ULL;
    cpu.v[0].lo = 0x0123456789abcdefULL; cpu.v[0].hi = 0xfedcba9876543210ULL;
    cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
    if (jit) e->enable_jit();
    for (unsigned n = 0; cpu.pc != stop && n < 1000; ++n) {
        if (jit) cpu.pc = e->jit()->run_block(cpu, *e);
        else e->step(cpu);
    }
    assert(cpu.pc == stop);
    TaggedRun r{};
    std::memcpy(r.regs, cpu.regs, sizeof(r.regs));
    r.q1[0] = cpu.v[1].lo; r.q1[1] = cpu.v[1].hi;
    e->mem().read(data, r.data, sizeof(r.data));
    r.steps = cpu.decode_cache_hits + cpu.decode_cache_misses;  // interpreter steps only
    return r;
}
static void check_tagged_jit() {
    for (uint64_t tag : {0ULL, 2ULL, 0xb4ULL, 0xffULL}) {
        const TaggedRun interp = run_tagged(false, tag), jit = run_tagged(true, tag);
        assert(!std::memcmp(interp.regs, jit.regs, sizeof(jit.regs)));
        assert(!std::memcmp(interp.q1, jit.q1, sizeof(jit.q1)));
        assert(!std::memcmp(interp.data, jit.data, sizeof(jit.data)));
        assert(jit.regs[0] >> 56 == tag);  // pre-index writeback keeps the tag
        assert(jit.q1[0] == 0x0123456789abcdefULL && jit.regs[11] == 0x55667788);
        if (jit.steps) {
            std::cerr << "tag " << tag << ": " << jit.steps << " interpreter steps under the JIT\n";
            assert(false);
        }
    }
}
int main() {
    check_tagged_jit();
    for (uint64_t alignment : {4096,16384,65536}) {
        Memory memory(false);
        auto bytes = image(alignment);
        auto loaded = ElfLoader::load(memory, bytes);
        assert(loaded.base_addr % alignment == 0);
        assert(loaded.phdr_addr == loaded.base_addr + 64);
        assert(memory.load<uint32_t>(loaded.entry) == 0xd65f03c0);
        assert(memory.load<uint64_t>(loaded.base_addr + 4096) == 0);
        ElfLoader::Options options;
        options.apply_relocations = false; options.reserve_image = true;
        auto interpreter = ElfLoader::load(memory, bytes, options);
        assert(interpreter.base_addr % alignment == 0);
        assert(interpreter.base_addr >= Memory::MMAP_BASE_MIN);
        assert(interpreter.base_addr + 8192 < Memory::STACK_TOP - Memory::STACK_SIZE);
        assert(memory.mmap_alloc(4096) != interpreter.base_addr);
    }
    auto bytes = image(4096);
    put<uint64_t>(bytes,96,8193); rejects(bytes); // filesz > memsz
    bytes = image(4096); put<uint64_t>(bytes,72,UINT64_MAX); rejects(bytes);
    bytes = image(4096); put<uint64_t>(bytes,80,UINT64_MAX-4095); rejects(bytes);
    bytes = image(4096); put<uint64_t>(bytes,112,12288); rejects(bytes);
    bytes = image(16384); put<uint64_t>(bytes,80,4096); rejects(bytes);
    bytes = image(4096); put<uint64_t>(bytes,32,UINT64_MAX); rejects(bytes);

    Memory memory(false);
    assert(memory.map_range(0x8000,8192));
    bool failed = false;
    try { memory.store<uint64_t>(0xab00000000008000,1); }
    catch (const UnmappedMemory&) { failed = true; }
    assert(failed); // Linux profile still rejects these addresses
    memory.set_data_top_byte_ignore(true);
    for (uint64_t tag : {0ULL,2ULL,0xabULL,0xffULL}) {
        uint64_t tagged = (tag << 56) | 0x8000;
        memory.store<uint64_t>(tagged,tag);
        assert(memory.load<uint64_t>(0x8000) == tag);
        assert(memory.guest_to_host_ptr(tagged) == memory.guest_to_host_ptr(0x8000));
        assert(memory.atomic_cas_64(tagged,tag,42));
        assert(memory.atomic_cas_32(tagged,42,43));
    }
    uint64_t value = 0xdeadbeef;
    memory.write(0x0200000000008ffc,&value,8); // page-crossing tagged access
    assert(memory.load<uint64_t>(0x8ffc) == value);
    assert(memory.mprotect_guest(0x9000,4096,Memory::GUEST_PROT_READ));
    failed = false;
    try { memory.write(0xff00000000008ffc,&value,8); }
    catch (const UnmappedMemory&) { failed = true; }
    assert(failed && memory.load<uint64_t>(0x8ffc) == value);
    // Guest fork is a host fork: the child reads the parent's tagged state.
    const pid_t child = fork();
    if (child == 0) _exit(memory.load<uint64_t>(0x0200000000008ffc) == value ? 0 : 1);
    int status = 0;
    assert(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    failed = false;
    try { memory.fetch_inst(0x0200000000008000); }
    catch (const UnmappedMemory&) { failed = true; }
    assert(failed); // instruction addresses are not silently untagged
    constexpr uint64_t high = 0x100004000ULL;
    assert(memory.map_range(high,4096));
    memory.store<uint64_t>(0x0200000000000000ULL | high,99);
    assert(memory.load<uint64_t>(high) == 99);
    assert(memory.atomic_cas_64(0xff00000000000000ULL | high,99,100));
    assert(memory.load<uint64_t>(high) == 100);

    for (unsigned opcode = 0x10; opcode <= 0x1f; ++opcode)
    for (unsigned sf = 0; sf < 2; ++sf) {
        DecodedInst decoded{};
        uint32_t word = 0x1ac10000 | (sf << 31) | (opcode << 10);
        const bool valid = opcode <= 0x17 && sf == ((opcode & 3) == 3);
        assert(decode(decoded,word) == valid);
        if (valid) {
            assert(decoded.cls == InstClass::CRC32);
            assert(decoded.imm == (opcode & 3));
            assert(decoded.is_sub == ((opcode >> 2) & 1));
        }
    }
    std::cout << "android_loader_host: ALL PASS\n";
}
