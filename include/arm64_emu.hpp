// arm64_emu.hpp — backward-compatibility umbrella header.
//
// Historical code referenced this file directly. After the v1.4.0-beta.1
// refactor, the public surface is split into:
//   - include/bifrost/types.hpp      (forward decls, exceptions, utilities)
//   - include/bifrost/version.hpp    (VERSION / CODENAME)
//   - include/bifrost/emulator.hpp   (Emulator class — full definition)
//   - include/decoder.hpp            (decoder + DecodedInst)
//   - include/ir/ir.hpp              (IR types + translate/optimize/dump)
//   - include/jit/frostjit.hpp       (FrostJIT class)
//   - include/graphics.hpp           (GraphicsBackend)
//
// This umbrella pulls them all in so legacy `#include "arm64_emu.hpp"`
// keeps working. New code should include only what it needs.
#pragma once
#include "bifrost/emulator.hpp"   // pulls in types, version, core/* via core/emulator.h
#include "bifrost/version.hpp"
#include "decoder.hpp"
#include "ir/ir.hpp"
#include "jit/frostjit.hpp"
#include "frost/graphics.hpp"
#include "core/signal.h"
