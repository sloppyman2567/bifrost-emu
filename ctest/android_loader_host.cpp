#include "core/emulator.h"
#include "decoder.hpp"
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>

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
int main() {
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
    auto child = memory.clone_for_fork();
    assert(child->load<uint64_t>(0x0200000000008ffc) == value);
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
