// bifrost/emulator.hpp — public Emulator API.
//
// This header re-exports just the Emulator class definition from the
// private core/emulator.h. External consumers (libbifrost users, test
// harnesses, IDE plugins) include this header; they don't see the
// private members directly because they only call the public methods.
//
// For the basic run-an-ELF use case:
//
//     #include "bifrost/emulator.hpp"
//
//     arm64emu::Emulator emu;
//     std::vector<std::string> argv = {"hello.elf"};
//     emu.load_elf_file("hello.elf", argv);
//     int exit_code = emu.run();
#pragma once

#include "bifrost/types.hpp"
#include "bifrost/version.hpp"

// Pull in the full class definition. The private members are visible but
// external code should only use the public API (load_elf_file / run /
// step_public / set_* / accessors / enable_jit / jit_step).
#include "core/emulator.h"
