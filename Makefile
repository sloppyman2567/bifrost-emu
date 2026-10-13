# bifrost-emu Makefile (2.0.0-alpha.1)
#
# Auto-discovers all .cpp files under Source/ and compiles them into the
# final bifrost-emu binary. Library builds (libbifrost.a) compile the
# same sources minus Source/Main.cpp.
#
# Build options (set on the make command line, e.g. `make`):
#
#   USE_SDL2=1     Enable SDL2 window backend for /dev/fb0.
#   USE_THUNK_GL=1 Enable host GL/EGL thunking (required for SDL2/GL apps).
#   Default: auto-detect (enabled when sdl2-config is available).
#
# Override examples:
#   make USE_SDL2=0    # force headless build even if SDL2 is installed
#   make USE_SDL2=1    # force SDL2 build (errors if not installed)
#
#   CXX            C++ compiler (default: g++)
#   CXXFLAGS       C++ compiler flags
#   LDFLAGS        Linker flags
#
# Directory layout (see Docs/ARCHITECTURE.md):
#
#   Source/FrostJIT/     — JIT: IR, Compiler, CodeGen, RegisterAlloc,
#                          Backend, Runtime, Tier2
#   Source/Interpreter/  — Decoder, Core (integer/branch), FloatSIMD
#   Source/System/       — Core, Memory, Loader, Syscalls, VFS, Android,
#                          Graphics, Audio, API
#   Include/             — public Bifrost/ API + Generated/ opgen tables
# Includes are spelled relative to Source/ or Include/.

CXX      ?= g++
INCDIR   := Include
CXXFLAGS ?= -O3 -std=c++17 -pthread -Wall -Wextra -I$(INCDIR) -ISource -MMD -MP
LDFLAGS  ?= -pthread

BUILD_PROFILE ?= release
USE_SDL2 ?= 1
USE_THUNK_GL ?= 1
# Android host window only; guest SDL2 thunk ABI remains unchanged.
USE_SDL3 ?= $(USE_SDL2)
BUILD_DIR ?= build/$(BUILD_PROFILE)-sdl$(USE_SDL2)-gl$(USE_THUNK_GL)
OBJDIR   := $(BUILD_DIR)
TARGET   ?= $(BUILD_DIR)/bifrost-emu
LIB      := $(OBJDIR)/libbifrost.a
CONFIG_STAMP := $(OBJDIR)/.build-config
SOURCE_STAMP := $(OBJDIR)/.source-list
MUSL_CC ?= Tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
GLIBC_CC ?= Tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc
CROSS_CC ?= $(MUSL_CC)

# Auto-discover all .cpp under Source/ except the C API (added to
# LIB_SOURCES below so libbifrost.a exposes Bifrost.h), plus Source/Main.cpp.
SOURCES  := $(shell find Source -name '*.cpp' -not -path 'Source/System/API/*' -not -path Source/Main.cpp | sort) Source/Main.cpp
OBJECTS  := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)   # auto-generated header dependency files

# Library objects (everything except Source/Main.cpp, PLUS the C API wrapper)
# and the native bridge adapter (if libffi is available, see below).
LIB_SOURCES := $(filter-out Source/Main.cpp,$(SOURCES)) Source/System/API/CAPI.cpp

# Native bridge adapter (Source/System/API/NativeBridge.cpp): exports the Android ART
# NativeBridgeItf table so libbifrost can serve as a native bridge
# (-XX:NativeBridge) for AArch64 apps on x86_64 hosts. Trampolines are
# libffi closures, so libffi is a hard dependency when enabled. Auto-
# disabled (with a warning) if libffi is unavailable.
NB_FFI_OK := $(shell echo 'int main(void){return 0;}' | $(CXX) -x c++ - -lffi -o /dev/null 2>/dev/null && echo yes)
ifeq ($(NB_FFI_OK),yes)
    LIB_SOURCES += Source/System/API/NativeBridge.cpp
    LDFLAGS += -lffi
else
    $(warning "libffi not found — native bridge adapter (Source/System/API/NativeBridge.cpp) disabled; install libffi-dev")
endif
LIB_OBJECTS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(LIB_SOURCES))
# Source/System/API/*.o files generate .d files via the pattern rule below, but DEPS above
# only covered OBJECTS (Source/* + main) — the api objects' deps were never
# included, so a header layout change left stale api objects in libbifrost.a
# (false test-capi segfault; always needed a clean rebuild). fold them in.
DEPS += $(LIB_OBJECTS:.o=.d)
# host test objects get their own .d files from the explicit recipes below.
DEPS += $(OBJDIR)/test_capi_host.d $(OBJDIR)/test_nb_host.d

HEADERS  := $(shell find Include Source -name '*.hpp' -o -name '*.h')

ifeq ($(USE_SDL2),1)
    SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
    SDL2_LIBS   ?= $(shell sdl2-config --libs   2>/dev/null)
    ifneq ($(SDL2_CFLAGS),)
        CXXFLAGS += $(SDL2_CFLAGS) -DBIFROST_USE_SDL2 -DBIFROST_THUNK_HAVE_SDL2
        LDFLAGS  += $(SDL2_LIBS)
    else
        $(warning "sdl2-config not found — building headless (install libsdl2-dev for SDL2)")
    endif
endif

ifeq ($(USE_SDL3),1)
    SDL3_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
    SDL3_AVAILABLE := $(shell pkg-config --exists 'sdl3 >= 3.2.0' 2>/dev/null && echo yes)
    ifeq ($(SDL3_AVAILABLE),yes)
        CXXFLAGS += $(SDL3_CFLAGS) $(shell pkg-config --cflags wayland-client x11) -DBIFROST_USE_SDL3
        LDFLAGS += $(shell pkg-config --libs wayland-client x11)
    else
        $(warning "SDL3 development headers unavailable; Android window backend returns ENOSYS")
    endif
endif

ifeq ($(USE_THUNK_GL),1)
    ifneq ($(SDL2_CFLAGS),)
        CXXFLAGS += -DBIFROST_THUNK_HAVE_GL -DBIFROST_THUNK_HAVE_EGL
        LDFLAGS  += -lGL -lEGL
    endif
endif

.PHONY: all opgen opgen-check nativegen nativegen-check nativegen-expect opgen-thunk opgen-thunk-check proxy-check wlgen wlgen-check wlviewporter wlviewporter-check vkmarshal vkmarshal-check vkxml-check glxml-check glcoverage-check egl-check opgen-fpfixed opgen-fpfixed-check test clean install uninstall lib debug setup setup-tests check-all test-capi test-nb generated-check ci FORCE

all: $(TARGET)
ifeq ($(BUILD_PROFILE),release)
ifeq ($(TARGET),$(BUILD_DIR)/bifrost-emu)
	cp $(TARGET) bifrost-emu
endif
endif

# Regenerate the opcode-decode tables (SIMDTable.hpp) from the specs.
# Requires python3. The generated headers are committed, so a plain `make`
# does NOT invoke this; run `make opgen` explicitly when a spec changes.
OPGEN_SPECS := Tools/CodeGen/simd_dp.txt
OPGEN_GEN   := Tools/CodeGen/opgen.py
OPGEN_OUTS  := Include/Generated/SIMDTable.hpp

opgen: $(OPGEN_OUTS)

$(OPGEN_OUTS): $(OPGEN_SPECS) $(OPGEN_GEN)
	python3 Tools/CodeGen/opgen.py $(OPGEN_SPECS) $@

# CI/navigation guard: fail if the committed generated header has drifted
# from the spec (i.e. someone edited the spec but forgot `make opgen`).
opgen-check:
	python3 Tools/CodeGen/opgen.py --check $(OPGEN_SPECS) $(OPGEN_OUTS)

# Native SIMD helper encodings: one spec drives FrostJIT's classifier and a
# generated guest test checked against QEMU hashes (nativegen-expect needs
# qemu-aarch64 and the cross toolchain; commit the refreshed expectations).
NATIVEGEN_ARGS := Tools/CodeGen/native_simd.txt Tools/CodeGen/native_simd_expected.txt \
	Include/Generated/NativeSimdTable.hpp Tests/Unit/jit_native_simd_gen.c

nativegen:
	python3 Tools/CodeGen/nativegen.py $(NATIVEGEN_ARGS)

nativegen-check:
	python3 Tools/CodeGen/nativegen.py --check $(NATIVEGEN_ARGS)

nativegen-expect: nativegen
	$(MAKE) cross SRC=Tests/Unit/jit_native_simd_gen.c OUT=Tests/Unit/jit_native_simd_gen.elf
	qemu-aarch64 Tests/Unit/jit_native_simd_gen.elf gen > Tools/CodeGen/native_simd_expected.txt
	python3 Tools/CodeGen/nativegen.py $(NATIVEGEN_ARGS)
	$(MAKE) cross SRC=Tests/Unit/jit_native_simd_gen.c OUT=Tests/Unit/jit_native_simd_gen.elf

# GraphicThunk symbol-signature table (same pattern as the SIMD_DP decode
# tables above): Tools/CodeGen/thunk_dp.txt is the single source of truth
# for which GL/GLES/EGL/SDL2/GLFW symbol is thunked and how it is
# marshalled. Generated into Include/Generated/ThunkTable.hpp, consumed by both
# registration (register_known_symbols_) and dispatch (policy switch).
THUNK_SPECS := Tools/CodeGen/thunk_dp.txt
THUNK_GEN   := Tools/CodeGen/thunkgen.py
THUNK_OUTS  := Include/Generated/ThunkTable.hpp

opgen-thunk: $(THUNK_OUTS)

$(THUNK_OUTS): $(THUNK_SPECS) $(THUNK_GEN)
	python3 Tools/CodeGen/thunkgen.py $(THUNK_SPECS) $@

opgen-thunk-check:
	python3 Tools/CodeGen/thunkgen.py --check $(THUNK_SPECS) $(THUNK_OUTS)

# DisplayProxy coverage audit: PROXY-policy spec rows vs hand-written
# proxy_dispatch_ handlers. Informational only (missing handlers are a
# demand-driven backlog, not drift) — prints the backlog, always exits 0.
proxy-check:
	python3 Tools/CodeGen/proxycheck.py

# Wayland protocol signature table (same pattern as the SIMD_DP decode
# tables): ThirdParty/WaylandXML/wayland.xml + xdg-shell.xml (vendored, like
# the Khronos registries) are the source of truth for per-message arg
# signatures consumed by the DisplayProxy marshal work.
WL_XML := ThirdParty/WaylandXML/wayland.xml ThirdParty/WaylandXML/xdg-shell.xml
WL_GEN := Tools/CodeGen/wlgen.py
WL_OUT := Include/Generated/WaylandTable.hpp

wlgen: $(WL_OUT)

$(WL_OUT): $(WL_XML) $(WL_GEN)
	python3 Tools/CodeGen/wlgen.py $(WL_XML) $@

wlgen-check:
	python3 Tools/CodeGen/wlgen.py --check $(WL_XML) $(WL_OUT)

# Android window subsurfaces scale pixel buffers with wp_viewporter. The
# client glue is wayland-scanner output from the vendored stable XML
# (wayland-protocols 1.36), committed so builds need neither tool nor package.
WL_VIEWPORTER_XML := ThirdParty/WaylandXML/viewporter.xml
WL_VIEWPORTER_OUT := Include/Generated/ViewporterClient.h Include/Generated/ViewporterProtocol.inc

wlviewporter:
	wayland-scanner client-header $(WL_VIEWPORTER_XML) Include/Generated/ViewporterClient.h
	wayland-scanner private-code $(WL_VIEWPORTER_XML) Include/Generated/ViewporterProtocol.inc

# The scanner stamps its version into each file; compare everything else.
wlviewporter-check:
	@wayland-scanner client-header $(WL_VIEWPORTER_XML) /dev/stdout | \
		diff -I 'Generated by wayland-scanner' - Include/Generated/ViewporterClient.h
	@wayland-scanner private-code $(WL_VIEWPORTER_XML) /dev/stdout | \
		diff -I 'Generated by wayland-scanner' - Include/Generated/ViewporterProtocol.inc

# Vulkan deep-marshal descriptor table (same pattern as the SIMD_DP decode
# tables): ThirdParty/VulkanHeaders/registry/vk.xml is the source of truth for
# struct layouts + per-command staging plans consumed by DisplayThunk's
# VK_CMD_DEEP policy. Generated into Include/Generated/VulkanMarshal.hpp.
VKMARSHAL_XML := ThirdParty/VulkanHeaders/registry/vk.xml
VKMARSHAL_GEN := Tools/CodeGen/vkmarshalgen.py Tools/CodeGen/vkxml.py
VKMARSHAL_OUT := Include/Generated/VulkanMarshal.hpp

vkmarshal: $(VKMARSHAL_OUT)

$(VKMARSHAL_OUT): $(VKMARSHAL_XML) $(VKMARSHAL_GEN)
	python3 Tools/CodeGen/vkmarshalgen.py $(VKMARSHAL_XML) $@

vkmarshal-check:
	python3 Tools/CodeGen/vkmarshalgen.py --check $(VKMARSHAL_XML) $(VKMARSHAL_OUT)

# Audit every VK row in the spec against the vendored Khronos vk.xml:
# arity + pointer-position agreement (catches shifted-pointer-mask bugs
# mechanically). Warnings list dynamically-sized params dispatched as
# plain 'p' — the deep-marshal roadmap.
vkxml-check:
	python3 Tools/CodeGen/vkxmlcheck.py $(THUNK_SPECS) ThirdParty/VulkanHeaders/registry/vk.xml

# Audit every GL/GLES row in the spec against the vendored Khronos gl.xml
# (ThirdParty/GLRegistry/gl.xml): arity + pointer-position agreement. Same
# bug class as vkxml-check catches for Vulkan.
glxml-check:
	python3 Tools/CodeGen/glxmlcheck.py $(THUNK_SPECS) ThirdParty/GLRegistry/gl.xml

# Verify the autogenerated core-GL coverage block inside thunk_dp.txt
# matches what glcoverage.py would generate from gl.xml (drift guard).
glcoverage-check:
	python3 Tools/CodeGen/glcoverage.py --check $(THUNK_SPECS) ThirdParty/GLRegistry/gl.xml

# Audit every EGL row against the vendored Khronos egl.xml.
egl-check:
	python3 Tools/CodeGen/eglcheck.py $(THUNK_SPECS) ThirdParty/GLRegistry/egl.xml

# Audit every GLFW/SDL row in the spec against the installed system headers
# (/usr/include/GLFW/glfw3.h, /usr/include/SDL2/*.h): arity +
# pointer-position + float agreement. Same bug class as vkxml-check catches
# for Vulkan (glfwGetVersion/GetWindowSize, SDL_CreateWindowAndRenderer).
# Informational only (discrepancies are a demand-driven backlog, not drift)
# — prints the backlog, always exits 0. Opaque host cookies (GLFWwindow*,
# SDL_Window*, ...) are verbatim 'i' by design; data/OUT pointers must
# be 'p'/'z'.
thunk-hdr-check:
	python3 Tools/CodeGen/thunkhdrcheck.py $(THUNK_SPECS) || true

# FP fixed-point conversion decode table (same pattern as SIMD_DP above):
# Tools/CodeGen/fp_fixconv.txt is the single source of truth for which
# SCVTF/UCVTF/FCVTZS/FCVTZU #fbits form is which (GPR vs FP register
# source/dest). Generated into Include/Generated/FixedPointTable.hpp, consumed by
# the interpreter (Dispatch.cpp), the IR translator (FloatSIMD.cpp)
# and the JIT block heuristic (BlockCompiler.cpp).
FPFIX_SPECS := Tools/CodeGen/fp_fixconv.txt
FPFIX_GEN   := Tools/CodeGen/fpgen.py
FPFIX_OUTS  := Include/Generated/FixedPointTable.hpp

opgen-fpfixed: $(FPFIX_OUTS)

$(FPFIX_OUTS): $(FPFIX_SPECS) $(FPFIX_GEN)
	python3 Tools/CodeGen/fpgen.py $(FPFIX_SPECS) $@

opgen-fpfixed-check:
	python3 Tools/CodeGen/fpgen.py --check $(FPFIX_SPECS) $(FPFIX_OUTS)

$(TARGET): $(OBJECTS) $(SOURCE_STAMP)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

# If flags change without changing a source timestamp, rebuild objects.
FORCE:

$(CONFIG_STAMP): FORCE
	@mkdir -p $(OBJDIR)
	@tmp="$@.tmp"; { \
	  printf '%s\n' 'CXX=$(CXX)' 'CXXFLAGS=$(CXXFLAGS)' 'LDFLAGS=$(LDFLAGS)' \
	    'USE_SDL2=$(USE_SDL2)' 'USE_SDL3=$(USE_SDL3)' 'USE_THUNK_GL=$(USE_THUNK_GL)' 'BUILD_PROFILE=$(BUILD_PROFILE)'; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

$(OBJECTS) $(LIB_OBJECTS): $(CONFIG_STAMP)

# Relink/rearchive when files are removed or renamed, even if the remaining
# objects are older than the output. Do not force recompilation for this.
$(SOURCE_STAMP): FORCE
	@mkdir -p $(OBJDIR)
	@printf '%s\n' $(SOURCES) $(LIB_SOURCES) > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

# Pattern rule: compile any .cpp under Source/ or Source/Main.cpp to an object under BUILD_DIR.
# Header dependencies are auto-tracked via -MMD -MP (see DEPS above).
$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Include auto-generated header dependencies (silently ignore if missing).
-include $(DEPS)

# Build a configuration-specific archive and refresh the conventional root
# copy for existing API consumers.
lib: $(LIB)
	cp $(LIB) libbifrost.a

$(LIB): $(LIB_OBJECTS) $(SOURCE_STAMP) Makefile
	@rm -f $@.tmp
	ar rcs $@.tmp $(LIB_OBJECTS)
	mv $@.tmp $@
	@echo "Built $@ (excludes Source/Main.cpp; link your own driver)"

# Debug + ASan/UBSan build uses isolated objects and can coexist with release.
debug:
	$(MAKE) BUILD_PROFILE=debug BUILD_DIR=build/debug TARGET=build/debug/bifrost-emu-dbg \
	  USE_SDL2=0 USE_THUNK_GL=0 \
	  CXXFLAGS='-O0 -g -std=c++17 -pthread -Wall -Wextra -fsanitize=address,undefined -MMD -MP -I$(INCDIR) -ISource' \
	  LDFLAGS='-pthread -fsanitize=address,undefined' all

# ── Test runner ────────────────────────────────────────────────────────
# `make check` runs the standalone test script (Scripts/Testing/run_tests.sh),
# which categorizes tests, colorizes output, and prints a summary table.
# Supports filters: `make check ARGS="--toybox"` or `make check ARGS="--filter md5"`.
# See `./Scripts/Testing/run_tests.sh --help` for all options.
.PHONY: check check-quick check-nojit check-fwd verify
check: $(TARGET)
	./Scripts/Testing/run_tests.sh --emu $(TARGET) $(ARGS)

check-quick: $(TARGET)
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --quick

check-nojit: $(TARGET)
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --no-jit --quick

check-fwd: $(TARGET)
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --fwd --quick

# Run all test programs.
#
# JIT is the default execution mode. The first loop runs every .elf
# under the JIT (with stdin redirected from /dev/null so interactive
# programs don't block). The second loop runs the JIT-specific
# regression suite (Tests/Unit/jit_*.elf) under the interpreter (--no-jit)
# to catch decoder/interpreter drift. Interactive programs that need
# real stdin (echo, repl, sh, fgets_test, cat-with-args) and the
# infinite `yes` program are excluded from the auto-loop -- run them
# by hand.
test: $(TARGET)
	@echo "--- Running test suite (JIT, default) ---"
	@for f in Tests/Asm/*.elf Tests/Unit/*.elf Tests/Apps/*.elf; do \
	case "$$f" in \
	*/echo.elf|*/repl.elf|*/sh.elf|*/fgets_test.elf|*/yes.elf|*/cat.elf|*/tr.elf) \
	echo "--- skipping interactive/infinite: $$f ---"; continue;; \
	esac; \
	echo "--- $$f ---"; \
	timeout 10 ./$(TARGET) $$f </dev/null \
	|| echo "FAILED (rc=$$?): $$f"; \
	done
	@echo "--- Running JIT regression suite under interpreter (--no-jit) ---"
	@for f in Tests/Unit/jit_*.elf; do \
	echo "--- $$f (interp) ---"; \
	timeout 10 ./$(TARGET) --no-jit $$f </dev/null \
	|| echo "FAILED (rc=$$?): $$f"; \
	done
	@echo "--- Done. ---"

# Differential replay plus separate threaded oracle runs (no shared-memory replay).
SHELL := /bin/bash
verify: $(TARGET) test-ir-ssa
	@bash Scripts/Testing/verify_jit.sh "$(TARGET)"


# Cross-compile a test program with the bundled musl toolchain.
# Usage: make cross SRC=Tests/Apps/hello.c OUT=Tests/Apps/hello.elf
cross:
	@if [ -z "$(SRC)" ] || [ -z "$(OUT)" ]; then \
	echo "Usage: make cross SRC=<file.c> OUT=<file.elf>"; exit 1; \
	fi
	@extra="$(CROSS_EXTRA)"; \
	case "$(SRC)" in \
		Tests/Unit/test_lse_inline.c) extra="$$extra -march=armv8.1-a+lse";; \
		Tests/Apps/test_sha256_full.c|Tests/Apps/test_sha256_crypto.c) extra="$$extra -march=armv8-a+crypto";; \
	esac; \
	$(CROSS_CC) -static -O2 $$extra -o $(OUT) $(SRC)
	@echo "Built $(OUT)"

# Install to /usr/local/bin (override with `make install DESTDIR=/prefix PREFIX=/opt`)
PREFIX ?= /usr/local
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bifrost-emu

clean:
	rm -rf build bifrost-emu bifrost-emu-dbg *.o *.d libbifrost.a

# ── One-click setup ───────────────────────────────────────────────────
# `make setup` runs the bundled bootstrap script: builds the emulator,
# fetches the musl toolchain (if missing), cross-compiles every test,
# sets up the rootfs, and runs the test suite. Idempotent.
setup: all
	./Scripts/Setup/setup.sh

# ── Test toolchains ────────────────────────────────────────────────────
# Most Tests/Unit and Tests/Apps tests are self-contained and are built STATICALLY
# with the musl toolchain (no rootfs needed to run). The tests listed
# below must be DYNAMICALLY linked so they exercise the emulator's ELF
# loader, interpreter, and dl* plumbing — glibc ones use the glibc cross
# toolchain, *_musl variants use the musl toolchain without -static.
# All dynamic tests need the rootfs (BIFROST_ROOT) to run.
GLIBC_DYN_SRCS := \
	Tests/Apps/test_dlopen.c \
	Tests/Apps/test_dlopen_mt.c \
	Tests/Apps/test_dladdr_glibc.c \
	Tests/Apps/test_dyn_hello.c \
	Tests/Apps/test_dyn_write.c \
	Tests/Apps/test_dyn_malloc.c \
	Tests/Apps/test_dyn_printf.c \
	Tests/Apps/test_dyn_pthread_min.c \
	Tests/Apps/test_dyn_threads.c \
	Tests/Apps/test_dyn_pthread_stress.c \
	Tests/Apps/test_dyn_pthread_8thread.c \
	Tests/Apps/test_dyn_main_locks.c \
	Tests/Apps/xwayland_wire.c

# Dynamically-linked variants whose .elf name differs from the source.
MUSL_DYN_PAIRS := Tests/Apps/hello_dyn_musl.elf:Tests/Apps/hello_dyn.c \
	Tests/Apps/test_dyn_full_musl.elf:Tests/Apps/test_dyn_full.c
GLIBC_DYN_PAIRS := Tests/Apps/hello_dyn_glibc.elf:Tests/Apps/hello_dyn.c

# `make setup-tests` fetches the toolchains (if missing) and cross-
# compiles the test .elf files — it does NOT run the test suite or build
# the rootfs. Useful for CI jobs that want to build tests once and run
# them later. A fresh checkout then yields the complete test set.
setup-tests:
	@if ! command -v "$(MUSL_CC)" >/dev/null 2>&1 && [ ! -x "$(MUSL_CC)" ]; then \
	  if [ "$(MUSL_CC)" != "Tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc" ]; then echo "error: MUSL_CC not found: $(MUSL_CC)" >&2; exit 1; fi; \
		echo "Fetching musl toolchain ..."; \
		./Tools/Fetch/fetch-musl-toolchain.sh; \
	fi
	@if ! command -v "$(GLIBC_CC)" >/dev/null 2>&1 && [ ! -x "$(GLIBC_CC)" ]; then \
	  if [ "$(GLIBC_CC)" != "Tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc" ]; then echo "error: GLIBC_CC not found: $(GLIBC_CC)" >&2; exit 1; fi; \
		echo "Fetching glibc toolchain ..."; \
		./Tools/Fetch/fetch-glibc-toolchain.sh; \
	fi
	@CC="$(MUSL_CC)"; \
	count=0; \
	for src in Tests/Unit/*.c Tests/Apps/*.c; do \
		[ -f "$$src" ] || continue; \
		case " $(GLIBC_DYN_SRCS) " in *" $$src "*) continue ;; esac; \
		case "$$src" in Tests/Unit/test_capi.c|Tests/Unit/nb_lib.c) continue ;; esac; \
		elf="$${src%.c}.elf"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && continue; \
		extra=""; \
		case "$$src" in \
			Tests/Unit/test_lse_inline.c) extra="-march=armv8.1-a+lse";; \
			Tests/Apps/test_sha256_full.c|Tests/Apps/test_sha256_crypto.c) extra="-march=armv8-a+crypto";; \
			Tests/Apps/test_*vulkan*.c) extra="-ITests/Apps/vulkan_headers/include";; \
		esac; \
		if $$CC -static -O2 $$extra -o "$$elf" "$$src" 2>/dev/null; then \
			count=$$((count + 1)); \
		fi; \
	done; \
	echo "Cross-compiled $$count musl-static test binaries."
	@python3 Scripts/Testing/build_asm_tests.py "$(MUSL_CC)"
	@CC="$(MUSL_CC)"; \
	if [ ! -f Tests/Unit/nb_testlib.so ] || [ "Tests/Unit/nb_testlib.so" -ot "Tests/Unit/nb_lib.c" ]; then \
		$$CC -O2 -shared -fPIC -nostdlib Tests/Unit/nb_lib.c -o Tests/Unit/nb_testlib.so && \
		echo "Built Tests/Unit/nb_testlib.so (native bridge test lib)."; \
	fi
	@CC="$(GLIBC_CC)"; \
	count=0; \
	for src in $(GLIBC_DYN_SRCS); do \
		elf="$${src%.c}.elf"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && [ "$$elf" -nt Makefile ] && continue; \
		if [ "$$src" = Tests/Apps/xwayland_wire.c ]; then \
			if [ -f rootfs/usr/lib/libX11.so.6 ] && [ -f /usr/include/X11/Xlib.h ]; then \
				$$CC -O2 -idirafter /usr/include "$$src" -Lrootfs/usr/lib -Wl,-rpath-link,rootfs/usr/lib -l:libX11.so.6 -o "$$elf" && count=$$((count + 1)); \
			fi; \
			continue; \
		fi; \
		extra=""; \
		case "$$src" in Tests/Apps/test_dlopen.c|Tests/Apps/test_dlopen_mt.c) extra="-pthread -ldl -lm";; esac; \
		if $$CC -O2 -o "$$elf" "$$src" $$extra 2>/dev/null; then \
			count=$$((count + 1)); \
		fi; \
	done; \
	echo "Cross-compiled $$count glibc-dynamic test binaries."
	@CC="$(MUSL_CC)"; \
	count=0; \
	for pair in $(MUSL_DYN_PAIRS); do \
		elf="$${pair%%:*}"; src="$${pair#*:}"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && continue; \
		if $$CC -O2 -o "$$elf" "$$src" 2>/dev/null; then \
			count=$$((count + 1)); \
		fi; \
	done; \
	echo "Cross-compiled $$count musl-dynamic test binaries."
	@CC="$(GLIBC_CC)"; \
	count=0; \
	for pair in $(GLIBC_DYN_PAIRS); do \
		elf="$${pair%%:*}"; src="$${pair#*:}"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && continue; \
		if $$CC -O2 -o "$$elf" "$$src" 2>/dev/null; then \
			count=$$((count + 1)); \
		fi; \
	done; \
	echo "Cross-compiled $$count glibc-dynamic variant binaries."

# Generated source checks share one entry point for CI and local reviews.
generated-check: opgen-check nativegen-check opgen-thunk-check wlgen-check wlviewporter-check vkmarshal-check vkxml-check glxml-check glcoverage-check egl-check opgen-fpfixed-check

# Local guest-correctness gate: generator drift, host APIs, JIT/interpreter
# regressions, sandbox coverage, and a focused dynamic-glibc run.
ci: setup-tests generated-check $(TARGET)
	$(MAKE) test-capi
	$(MAKE) test-nb
	$(MAKE) test-android-window-backend test-android-window-plugin-cli
	$(MAKE) verify
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --unit --quick --strict
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --unit --quick --strict --no-jit
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --integration --strict --filter '^test_sandbox$$'
	@./Scripts/Setup/setup-rootfs.sh
	@test -f rootfs/lib/ld-linux-aarch64.so.1 || { echo "error: glibc loader missing from rootfs" >&2; exit 1; }
	./Scripts/Testing/run_tests.sh --emu $(TARGET) --dynamic --strict --filter '^(hello_dyn_glibc|test_dyn_(write|hello|malloc|printf|pthread_min|threads|pthread_stress|pthread_8thread)|test_dladdr_glibc)$$'

# Full validation provisions the rootfs and requires every selected fixture
# except the optional external iperf3/coreutils bundle, which is reported as
# a visible skip when it is not installed. Setup failures are fatal.
check-all: setup-tests generated-check $(TARGET)
	$(MAKE) test-capi
	$(MAKE) test-nb
	$(MAKE) test-android-window-backend test-android-window-plugin-cli
	@./Scripts/Setup/setup-rootfs.sh
	@test -f rootfs/lib/ld-linux-aarch64.so.1 || { echo "error: glibc loader missing from rootfs" >&2; exit 1; }
	@test -f rootfs/lib/ld-musl-aarch64.so.1 || { echo "error: musl loader missing from rootfs" >&2; exit 1; }
	@./Scripts/Testing/run_tests.sh --emu $(TARGET) --strict --allow-missing '^rw_(iperf3_version|coreutils_)'

# Host-side C API test: test_capi.c links libbifrost.a and runs on the
# HOST (it cannot be cross-compiled as a guest ELF). Builds and runs it.
# Note: the guest hello.elf path is relative to the repo root.
CAPI_TESTLIB := Tests/Unit/capi_testlib.so
$(CAPI_TESTLIB): Tests/Unit/capi_call_lib.c
	@$(CROSS_CC) -O2 -shared -fPIC -nostdlib $< -o $@
	@echo "Built $@"

test-capi: lib $(TARGET) $(CAPI_TESTLIB)
	@echo "=== Building host C API test ==="
	@mkdir -p $(OBJDIR)
	@$(CXX) -O1 -g -MMD -MP -MF $(OBJDIR)/test_capi_host.d -ISource -x c -c Tests/Unit/test_capi.c -o $(OBJDIR)/test_capi_host.o
	@$(CXX) $(OBJDIR)/test_capi_host.o $(LIB) -o $(OBJDIR)/test_capi_host $(LDFLAGS)
	@echo "=== Running host C API test ==="
	@./$(OBJDIR)/test_capi_host

.PHONY: test-ir-ssa
test-ir-ssa:
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) Tests/Host/ir_ssa.cpp Source/FrostJIT/IR/SSA/SSA.cpp -o $(OBJDIR)/ir_ssa_test $(LDFLAGS)
	./$(OBJDIR)/ir_ssa_test

.PHONY: test-audio-frame-counters
test-audio-frame-counters:
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) Tests/Host/audio_frame_counters.cpp Source/System/Audio/Audio.cpp -o $(OBJDIR)/audio_frame_counters $(LDFLAGS)
	./$(OBJDIR)/audio_frame_counters

.PHONY: test-android-window-backend
test-android-window-backend: $(LIB)
	$(CXX) $(CXXFLAGS) Tests/Unit/android_window_backend_host.cpp $(LIB) -o $(OBJDIR)/android_window_backend_host $(LDFLAGS)
	./$(OBJDIR)/android_window_backend_host

.PHONY: test-android-sdl-backend
test-android-sdl-backend:
ifeq ($(SDL3_AVAILABLE),yes)
	@mkdir -p $(OBJDIR)/test-wayland
	$(CC) -std=c11 -Wall -Wextra -fPIC -shared Tests/Host/android_sdl3_fixture.c $(SDL3_CFLAGS) -o $(OBJDIR)/test-wayland/libSDL3.so.0
	$(CC) -std=c11 -Wall -Wextra -fPIC -shared Tests/Host/wayland_egl_lifetime_fixture.c -o $(OBJDIR)/test-wayland/libwayland-egl.so.1
	$(CXX) $(CXXFLAGS) Tests/Host/android_sdl_backend_lifetime.cpp Source/System/Android/Window/WindowBackend.cpp -o $(OBJDIR)/android_sdl_backend_lifetime -pthread -ldl $(shell pkg-config --libs wayland-client x11)
	LD_LIBRARY_PATH=$(abspath $(OBJDIR)/test-wayland) ./$(OBJDIR)/android_sdl_backend_lifetime
else
	@echo "error: test-android-sdl-backend requires USE_SDL3=1 and SDL3 development headers" >&2
	@exit 1
endif

ANDROID_WINDOW_PLUGIN := $(OBJDIR)/plugins/android-window-sdl.so
.PHONY: android-window-plugin test-android-window-plugin test-android-window-plugin-cli

.PHONY: android-runtime-adapters test-android-package
.PHONY: android-managed-launcher
android-managed-launcher:
	python3 Scripts/Android/android_managed.py build

android-runtime-adapters:
	python3 Tools/Android/build-android-adapters.py

test-android-package: $(TARGET) android-window-plugin android-runtime-adapters
	python3 Scripts/Android/test_android_package.py --emu $(abspath $(TARGET)) --plugin $(abspath $(ANDROID_WINDOW_PLUGIN))

android-window-plugin: $(ANDROID_WINDOW_PLUGIN)

$(ANDROID_WINDOW_PLUGIN): Plugins/AndroidWindowSDL/plugin.cpp Source/System/Android/Window/WindowBackend.cpp Source/System/Android/Window/WindowBackend.hpp Include/Bifrost/AndroidWindowPlugin.h Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fPIC -fvisibility=hidden -shared Plugins/AndroidWindowSDL/plugin.cpp Source/System/Android/Window/WindowBackend.cpp -o $@ -pthread -ldl $(shell pkg-config --libs wayland-client x11)

test-android-window-plugin: $(LIB)
	$(CC) -std=c11 -Wall -Wextra -IInclude -fPIC -shared Tests/Host/android_window_plugin_fixture.c -o $(OBJDIR)/android_window_plugin_fixture.so
	$(CC) -std=c11 -IInclude -fPIC -shared -DBIFROST_TEST_NO_ENTRY Tests/Host/android_window_plugin_fixture.c -o $(OBJDIR)/android_window_plugin_no_entry.so
	$(CXX) $(CXXFLAGS) Tests/Unit/android_window_plugin_host.cpp $(LIB) -o $(OBJDIR)/android_window_plugin_host $(LDFLAGS)
	./$(OBJDIR)/android_window_plugin_host ./$(OBJDIR)/android_window_plugin_fixture.so ./$(OBJDIR)/android_window_plugin_no_entry.so

# Requires setup-tests (or a cross-built Tests/Unit/hello.elf); never silently skip.
test-android-window-plugin-cli: $(TARGET) test-android-window-plugin
	python3 Scripts/Android/test_android_window_plugin_cli.py --emu $(TARGET) --plugin $(OBJDIR)/android_window_plugin_fixture.so

# Cross-compile the guest AArch64 shared object used by the native bridge
# host test (a -nostdlib -shared lib with JNI-shaped functions; must be
# rebuilt whenever Tests/Unit/nb_lib.c changes).
NB_TESTLIB := Tests/Unit/nb_testlib.so
$(NB_TESTLIB): Tests/Unit/nb_lib.c
	@$(CROSS_CC) -O2 -shared -fPIC -nostdlib Tests/Unit/nb_lib.c -o $@
	@echo "Built $@"

# Host-side native bridge test: test_nb.c links libbifrost.a (which
# contains the adapter when libffi is present) and runs on the HOST. It
# drives the NativeBridgeItf table exactly as ART's libnativebridge would:
# loadLibrary the guest .so, getTrampoline per JNI shorty, and call each
# trampoline through the borrow-CPU path.
test-nb: lib $(TARGET) $(NB_TESTLIB)
	@echo "=== Building host native bridge test ==="
	@mkdir -p $(OBJDIR)
	@$(CXX) -O1 -g -MMD -MP -MF $(OBJDIR)/test_nb_host.d -ISource -x c -c Tests/Unit/test_nb.c -o $(OBJDIR)/test_nb_host.o
	@$(CXX) $(OBJDIR)/test_nb_host.o $(LIB) -o $(OBJDIR)/test_nb_host $(LDFLAGS)
	@echo "=== Running host native bridge test ==="
	@./$(OBJDIR)/test_nb_host

# Hardware telemetry uses the vendored Vulkan ABI without a host SDK dependency.
$(OBJDIR)/Source/System/Graphics/Display/WindowStats.o: CXXFLAGS += -IThirdParty/VulkanHeaders/include
