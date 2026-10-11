// Common.hpp — backward-compatibility umbrella header.
//
// Historical code referenced this file directly. After the v1.4.0-beta.1
// refactor, the public surface is split into:
//   - Include/Bifrost/Types.hpp      (forward decls, exceptions, utilities)
//   - Include/Bifrost/Version.hpp    (VERSION / CODENAME)
//   - Include/Bifrost/Emulator.hpp   (Emulator class — full definition)
//   - Source/Interpreter/Decoder/Decoder.hpp            (decoder + DecodedInst)
//   - Source/FrostJIT/IR/IR.hpp              (IR types + translate/optimize/dump)
//   - Source/FrostJIT/Compiler/FrostJIT.hpp       (FrostJIT class)
//   - Source/System/Graphics/Display/Graphics.hpp (GraphicsBackend)
//
// This umbrella pulls them all in so legacy `#include "System/Core/Common.hpp"`
// keeps working. New code should include only what it needs.
#pragma once
#include "Bifrost/Emulator.hpp"   // pulls in types, version and core interfaces
#include "Bifrost/Version.hpp"
#include "Interpreter/Decoder/Decoder.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Graphics/Display/Graphics.hpp"
#include "System/Core/Signals.hpp"
