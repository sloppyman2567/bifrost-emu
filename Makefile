# bifrost-emu Makefile (1.5.5-alpha)
#
# Auto-discovers all .cpp files under src/ and compiles them into the
# final bifrost-emu binary. Library builds (libbifrost.a) compile the
# same sources minus main.cpp.
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
# Directory layout (1.5.5-alpha):
#
#   src/core/         — Emulator, Memory, CPU, SignalTable, thread_mgr
#   src/yggdrasil/          — Yggdrasil VFS (Node + FdTable + procfs + devfs)
#   src/ir/           — IR builder/translator/optimizer/executor
#   src/jit/          — FrostJIT (split into x86_backend, x86_regalloc,
#                       jit_cache, jit_profiler, frostjit, jit_glue)
#   src/syscalls/     — Linux AArch64 syscall layer (split by concern)
#   src/frontend/     — decoder + ELF loader
#   src/frost_graphics/  — FrostGraphics + GraphicThunk (fb + GL thunking)
#   src/interp/       — switch-based instruction interpreter

CXX      ?= g++
INCDIR   := include
CXXFLAGS ?= -O3 -std=c++17 -pthread -Wall -Wextra -I$(INCDIR) -Isrc -MMD -MP
LDFLAGS  ?= -pthread

BUILD_PROFILE ?= release
USE_SDL2 ?= 1
USE_THUNK_GL ?= 1
BUILD_DIR ?= build/$(BUILD_PROFILE)-sdl$(USE_SDL2)-gl$(USE_THUNK_GL)
OBJDIR   := $(BUILD_DIR)
TARGET   ?= $(BUILD_DIR)/bifrost-emu
LIB      := $(OBJDIR)/libbifrost.a
CONFIG_STAMP := $(OBJDIR)/.build-config
MUSL_CC ?= tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
GLIBC_CC ?= tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc
CROSS_CC ?= $(MUSL_CC)

# Auto-discover all .cpp under src/, plus main.cpp at the root.
# api/bifrost_capi.cpp is included in LIB_SOURCES so libbifrost.a exposes
# the C API (bifrost.h).
SRC_DIRS := src/core src/yggdrasil src/ir src/jit src/syscalls src/frontend src/frost_graphics src/interp src/audio
SOURCES  := $(shell find $(SRC_DIRS) -name '*.cpp') main.cpp
OBJECTS  := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)   # auto-generated header dependency files

# Library objects (everything except main.cpp, PLUS the C API wrapper)
# and the native bridge adapter (if libffi is available, see below).
LIB_SOURCES := $(filter-out main.cpp,$(SOURCES)) api/bifrost_capi.cpp

# Native bridge adapter (api/native_bridge.cpp): exports the Android ART
# NativeBridgeItf table so libbifrost can serve as a native bridge
# (-XX:NativeBridge) for AArch64 apps on x86_64 hosts. Trampolines are
# libffi closures, so libffi is a hard dependency when enabled. Auto-
# disabled (with a warning) if libffi is unavailable.
NB_FFI_OK := $(shell echo 'int main(void){return 0;}' | $(CXX) -x c++ - -lffi -o /dev/null 2>/dev/null && echo yes)
ifeq ($(NB_FFI_OK),yes)
    LIB_SOURCES += api/native_bridge.cpp
    LDFLAGS += -lffi
else
    $(warning "libffi not found — native bridge adapter (api/native_bridge.cpp) disabled; install libffi-dev")
endif
LIB_OBJECTS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(LIB_SOURCES))
# api/*.o files generate .d files via the pattern rule below, but DEPS above
# only covered OBJECTS (src/* + main) — the api objects' deps were never
# included, so a header layout change left stale api objects in libbifrost.a
# (false test-capi segfault; always needed a clean rebuild). fold them in.
DEPS += $(LIB_OBJECTS:.o=.d)
# host test objects get their own .d files from the explicit recipes below.
DEPS += $(OBJDIR)/test_capi_host.d $(OBJDIR)/test_nb_host.d

HEADERS  := $(shell find include src -name '*.hpp' -o -name '*.h')

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

ifeq ($(USE_THUNK_GL),1)
    ifneq ($(SDL2_CFLAGS),)
        CXXFLAGS += -DBIFROST_THUNK_HAVE_GL -DBIFROST_THUNK_HAVE_EGL
        LDFLAGS  += -lGL -lEGL
    endif
endif

.PHONY: all opgen opgen-check opgen-thunk opgen-thunk-check proxy-check wlgen wlgen-check vkmarshal vkmarshal-check vkxml-check glxml-check glcoverage-check egl-check opgen-fpfixed opgen-fpfixed-check test clean install uninstall lib debug setup setup-tests check-all test-capi test-nb generated-check ci FORCE

all: $(TARGET)
ifeq ($(BUILD_PROFILE),release)
ifeq ($(TARGET),$(BUILD_DIR)/bifrost-emu)
	cp $(TARGET) bifrost-emu
endif
endif

# Regenerate the opcode-decode tables (opgen_simd.hpp) from the specs.
# Requires python3. The generated headers are committed, so a plain `make`
# does NOT invoke this; run `make opgen` explicitly when a spec changes.
OPGEN_SPECS := tools/opgen/simd_dp.txt
OPGEN_GEN   := tools/opgen/opgen.py
OPGEN_OUTS  := include/opgen_simd.hpp

opgen: $(OPGEN_OUTS)

$(OPGEN_OUTS): $(OPGEN_SPECS) $(OPGEN_GEN)
	python3 tools/opgen/opgen.py $(OPGEN_SPECS) $@

# CI/navigation guard: fail if the committed generated header has drifted
# from the spec (i.e. someone edited the spec but forgot `make opgen`).
opgen-check:
	python3 tools/opgen/opgen.py --check $(OPGEN_SPECS) $(OPGEN_OUTS)

# GraphicThunk symbol-signature table (same pattern as the SIMD_DP decode
# tables above): tools/opgen/thunk_dp.txt is the single source of truth
# for which GL/GLES/EGL/SDL2/GLFW symbol is thunked and how it is
# marshalled. Generated into include/opgen_thunk.hpp, consumed by both
# registration (register_known_symbols_) and dispatch (policy switch).
THUNK_SPECS := tools/opgen/thunk_dp.txt
THUNK_GEN   := tools/opgen/thunkgen.py
THUNK_OUTS  := include/opgen_thunk.hpp

opgen-thunk: $(THUNK_OUTS)

$(THUNK_OUTS): $(THUNK_SPECS) $(THUNK_GEN)
	python3 tools/opgen/thunkgen.py $(THUNK_SPECS) $@

opgen-thunk-check:
	python3 tools/opgen/thunkgen.py --check $(THUNK_SPECS) $(THUNK_OUTS)

# DisplayProxy coverage audit: PROXY-policy spec rows vs hand-written
# proxy_dispatch_ handlers. Informational only (missing handlers are a
# demand-driven backlog, not drift) — prints the backlog, always exits 0.
proxy-check:
	python3 tools/opgen/proxycheck.py

# Wayland protocol signature table (same pattern as the SIMD_DP decode
# tables): tools/wayland-xml/wayland.xml + xdg-shell.xml (vendored, like
# the Khronos registries) are the source of truth for per-message arg
# signatures consumed by the DisplayProxy marshal work.
WL_XML := tools/wayland-xml/wayland.xml tools/wayland-xml/xdg-shell.xml
WL_GEN := tools/opgen/wlgen.py
WL_OUT := include/opgen_wl.hpp

wlgen: $(WL_OUT)

$(WL_OUT): $(WL_XML) $(WL_GEN)
	python3 tools/opgen/wlgen.py $(WL_XML) $@

wlgen-check:
	python3 tools/opgen/wlgen.py --check $(WL_XML) $(WL_OUT)

# Vulkan deep-marshal descriptor table (same pattern as the SIMD_DP decode
# tables): tools/vulkan-headers/registry/vk.xml is the source of truth for
# struct layouts + per-command staging plans consumed by DisplayThunk's
# VK_CMD_DEEP policy. Generated into include/opgen_vkmarshal.hpp.
VKMARSHAL_XML := tools/vulkan-headers/registry/vk.xml
VKMARSHAL_GEN := tools/opgen/vkmarshalgen.py tools/opgen/vkxml.py
VKMARSHAL_OUT := include/opgen_vkmarshal.hpp

vkmarshal: $(VKMARSHAL_OUT)

$(VKMARSHAL_OUT): $(VKMARSHAL_XML) $(VKMARSHAL_GEN)
	python3 tools/opgen/vkmarshalgen.py $(VKMARSHAL_XML) $@

vkmarshal-check:
	python3 tools/opgen/vkmarshalgen.py --check $(VKMARSHAL_XML) $(VKMARSHAL_OUT)

# Audit every VK row in the spec against the vendored Khronos vk.xml:
# arity + pointer-position agreement (catches shifted-pointer-mask bugs
# mechanically). Warnings list dynamically-sized params dispatched as
# plain 'p' — the deep-marshal roadmap.
vkxml-check:
	python3 tools/opgen/vkxmlcheck.py $(THUNK_SPECS) tools/vulkan-headers/registry/vk.xml

# Audit every GL/GLES row in the spec against the vendored Khronos gl.xml
# (tools/gl-registry/gl.xml): arity + pointer-position agreement. Same
# bug class as vkxml-check catches for Vulkan.
glxml-check:
	python3 tools/opgen/glxmlcheck.py $(THUNK_SPECS) tools/gl-registry/gl.xml

# Verify the autogenerated core-GL coverage block inside thunk_dp.txt
# matches what glcoverage.py would generate from gl.xml (drift guard).
glcoverage-check:
	python3 tools/opgen/glcoverage.py --check $(THUNK_SPECS) tools/gl-registry/gl.xml

# Audit every EGL row against the vendored Khronos egl.xml.
egl-check:
	python3 tools/opgen/eglcheck.py $(THUNK_SPECS) tools/gl-registry/egl.xml

# Audit every GLFW/SDL row in the spec against the installed system headers
# (/usr/include/GLFW/glfw3.h, /usr/include/SDL2/*.h): arity +
# pointer-position + float agreement. Same bug class as vkxml-check catches
# for Vulkan (glfwGetVersion/GetWindowSize, SDL_CreateWindowAndRenderer).
# Informational only (discrepancies are a demand-driven backlog, not drift)
# — prints the backlog, always exits 0. Opaque host cookies (GLFWwindow*,
# SDL_Window*, ...) are verbatim 'i' by design; data/OUT pointers must
# be 'p'/'z'.
thunk-hdr-check:
	python3 tools/opgen/thunkhdrcheck.py $(THUNK_SPECS) || true

# FP fixed-point conversion decode table (same pattern as SIMD_DP above):
# tools/opgen/fp_fixconv.txt is the single source of truth for which
# SCVTF/UCVTF/FCVTZS/FCVTZU #fbits form is which (GPR vs FP register
# source/dest). Generated into include/opgen_fpfixed.hpp, consumed by
# the interpreter (interp_fp.cpp), the IR translator (ir_translate_fp.cpp)
# and the JIT block heuristic (jit_translate.cpp).
FPFIX_SPECS := tools/opgen/fp_fixconv.txt
FPFIX_GEN   := tools/opgen/fpgen.py
FPFIX_OUTS  := include/opgen_fpfixed.hpp

opgen-fpfixed: $(FPFIX_OUTS)

$(FPFIX_OUTS): $(FPFIX_SPECS) $(FPFIX_GEN)
	python3 tools/opgen/fpgen.py $(FPFIX_SPECS) $@

opgen-fpfixed-check:
	python3 tools/opgen/fpgen.py --check $(FPFIX_SPECS) $(FPFIX_OUTS)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

# If flags change without changing a source timestamp, rebuild objects.
FORCE:

$(CONFIG_STAMP): FORCE
	@mkdir -p $(OBJDIR)
	@tmp="$@.tmp"; { \
	  printf '%s\n' 'CXX=$(CXX)' 'CXXFLAGS=$(CXXFLAGS)' 'LDFLAGS=$(LDFLAGS)' \
	    'USE_SDL2=$(USE_SDL2)' 'USE_THUNK_GL=$(USE_THUNK_GL)' 'BUILD_PROFILE=$(BUILD_PROFILE)'; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

$(OBJECTS) $(LIB_OBJECTS): $(CONFIG_STAMP)

# Pattern rule: compile any .cpp under src/ or main.cpp to an object under BUILD_DIR.
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

$(LIB): $(LIB_OBJECTS)
	ar rcs $@ $^
	@echo "Built $@ (excludes main.cpp; link your own driver)"

# Debug + ASan/UBSan build uses isolated objects and can coexist with release.
debug:
	$(MAKE) BUILD_PROFILE=debug BUILD_DIR=build/debug TARGET=build/debug/bifrost-emu-dbg \
	  USE_SDL2=0 USE_THUNK_GL=0 \
	  CXXFLAGS='-O0 -g -std=c++17 -pthread -Wall -Wextra -fsanitize=address,undefined -MMD -MP -I$(INCDIR) -Isrc' \
	  LDFLAGS='-pthread -fsanitize=address,undefined' all

# ── Test runner ────────────────────────────────────────────────────────
# `make check` runs the standalone test script (scripts/run_tests.sh),
# which categorizes tests, colorizes output, and prints a summary table.
# Supports filters: `make check ARGS="--toybox"` or `make check ARGS="--filter md5"`.
# See `./scripts/run_tests.sh --help` for all options.
.PHONY: check check-quick check-nojit check-fwd verify
check: $(TARGET)
	./scripts/run_tests.sh --emu $(TARGET) $(ARGS)

check-quick: $(TARGET)
	./scripts/run_tests.sh --emu $(TARGET) --quick

check-nojit: $(TARGET)
	./scripts/run_tests.sh --emu $(TARGET) --no-jit --quick

check-fwd: $(TARGET)
	./scripts/run_tests.sh --emu $(TARGET) --fwd --quick

# Run all test programs.
#
# JIT is the default execution mode. The first loop runs every .elf
# under the JIT (with stdin redirected from /dev/null so interactive
# programs don't block). The second loop runs the JIT-specific
# regression suite (ctest/jit_*.elf) under the interpreter (--no-jit)
# to catch decoder/interpreter drift. Interactive programs that need
# real stdin (echo, repl, sh, fgets_test, cat-with-args) and the
# infinite `yes` program are excluded from the auto-loop -- run them
# by hand.
test: $(TARGET)
	@echo "--- Running test suite (JIT, default) ---"
	@for f in test/*.elf ctest/*.elf ctest_real/*.elf; do \
	case "$$f" in \
	*/echo.elf|*/repl.elf|*/sh.elf|*/fgets_test.elf|*/yes.elf|*/cat.elf|*/tr.elf) \
	echo "--- skipping interactive/infinite: $$f ---"; continue;; \
	esac; \
	echo "--- $$f ---"; \
	timeout 10 ./$(TARGET) $$f </dev/null \
	|| echo "FAILED (rc=$$?): $$f"; \
	done
	@echo "--- Running JIT regression suite under interpreter (--no-jit) ---"
	@for f in ctest/jit_*.elf; do \
	echo "--- $$f (interp) ---"; \
	timeout 10 ./$(TARGET) --no-jit $$f </dev/null \
	|| echo "FAILED (rc=$$?): $$f"; \
	done
	@echo "--- Done. ---"

# Run JIT tests under BIFROST_JIT_VERIFY=1 and fail on missing fixtures,
# non-zero emulator exit, or a reported divergence. Capture output in a
# temporary file so filtering cannot mask the emulator's exit status.
SHELL := /bin/bash
verify: $(TARGET)
	@echo "--- JIT verify mode (divergence check) ---"
	@set -u; failed=0; count=0; \
	for f in ctest/jit_*.elf; do \
	  [ -f "$$f" ] || continue; count=$$((count + 1)); \
	  log=$$(mktemp); \
	  echo "--- $$f (verify) ---"; \
	  if BIFROST_JIT_VERIFY=1 timeout 30 "$(TARGET)" "$$f" </dev/null >"$$log" 2>&1; then rc=0; else rc=$$?; fi; \
	  if grep -Eq 'VERIFY.*DIVERGENCE.*pc|VERIFY.*x[0-9]+: jit' "$$log"; then \
	    grep -E 'VERIFY.*DIVERGENCE.*pc|VERIFY.*x[0-9]+: jit' "$$log" | head -3; \
	    echo "FAIL: divergence in $$f"; failed=1; \
	  fi; \
	  if [ $$rc -ne 0 ]; then echo "FAIL: $$f exited with rc=$$rc"; tail -20 "$$log"; failed=1; fi; \
	  rm -f "$$log"; \
	done; \
	if [ $$count -eq 0 ]; then echo "FAIL: no ctest/jit_*.elf fixtures found; run make setup-tests"; exit 1; fi; \
	if [ $$failed -ne 0 ]; then exit 1; fi; \
	echo "JIT verify passed for $$count fixtures."


# Cross-compile a test program with the bundled musl toolchain.
# Usage: make cross SRC=ctest_real/hello.c OUT=ctest_real/hello.elf
cross:
	@if [ -z "$(SRC)" ] || [ -z "$(OUT)" ]; then \
	echo "Usage: make cross SRC=<file.c> OUT=<file.elf>"; exit 1; \
	fi
	@extra="$(CROSS_EXTRA)"; \
	case "$(SRC)" in \
		ctest/test_lse_inline.c) extra="$$extra -march=armv8.1-a+lse";; \
		ctest_real/test_sha256_full.c) extra="$$extra -march=armv8-a+crypto";; \
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
	./scripts/setup.sh

# ── Test toolchains ────────────────────────────────────────────────────
# Most ctest/ctest_real tests are self-contained and are built STATICALLY
# with the musl toolchain (no rootfs needed to run). The tests listed
# below must be DYNAMICALLY linked so they exercise the emulator's ELF
# loader, interpreter, and dl* plumbing — glibc ones use the glibc cross
# toolchain, *_musl variants use the musl toolchain without -static.
# All dynamic tests need the rootfs (BIFROST_ROOT) to run.
GLIBC_DYN_SRCS := \
	ctest_real/test_dladdr_glibc.c \
	ctest_real/test_dyn_hello.c \
	ctest_real/test_dyn_write.c \
	ctest_real/test_dyn_malloc.c \
	ctest_real/test_dyn_printf.c \
	ctest_real/test_dyn_pthread_min.c \
	ctest_real/test_dyn_threads.c \
	ctest_real/test_dyn_pthread_stress.c \
	ctest_real/test_dyn_pthread_8thread.c

# Dynamically-linked variants whose .elf name differs from the source.
MUSL_DYN_PAIRS := ctest_real/hello_dyn_musl.elf:ctest_real/hello_dyn.c \
	ctest_real/test_dyn_full_musl.elf:ctest_real/test_dyn_full.c
GLIBC_DYN_PAIRS := ctest_real/hello_dyn_glibc.elf:ctest_real/hello_dyn.c

# `make setup-tests` fetches the toolchains (if missing) and cross-
# compiles the test .elf files — it does NOT run the test suite or build
# the rootfs. Useful for CI jobs that want to build tests once and run
# them later. A fresh checkout then yields the complete test set.
setup-tests:
	@if ! command -v "$(MUSL_CC)" >/dev/null 2>&1 && [ ! -x "$(MUSL_CC)" ]; then \
	  if [ "$(MUSL_CC)" != "tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc" ]; then echo "error: MUSL_CC not found: $(MUSL_CC)" >&2; exit 1; fi; \
		echo "Fetching musl toolchain ..."; \
		./tools/fetch-musl-toolchain.sh; \
	fi
	@if ! command -v "$(GLIBC_CC)" >/dev/null 2>&1 && [ ! -x "$(GLIBC_CC)" ]; then \
	  if [ "$(GLIBC_CC)" != "tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc" ]; then echo "error: GLIBC_CC not found: $(GLIBC_CC)" >&2; exit 1; fi; \
		echo "Fetching glibc toolchain ..."; \
		./tools/fetch-glibc-toolchain.sh; \
	fi
	@CC="$(MUSL_CC)"; \
	count=0; \
	for src in ctest/*.c ctest_real/*.c; do \
		[ -f "$$src" ] || continue; \
		case " $(GLIBC_DYN_SRCS) " in *" $$src "*) continue ;; esac; \
		case "$$src" in ctest/test_capi.c|ctest/nb_lib.c) continue ;; esac; \
		elf="$${src%.c}.elf"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && continue; \
		extra=""; \
		case "$$src" in \
			ctest/test_lse_inline.c) extra="-march=armv8.1-a+lse";; \
			ctest_real/test_sha256_full.c) extra="-march=armv8-a+crypto";; \
			ctest_real/test_*vulkan*.c) extra="-Ictest_real/vulkan_headers/include";; \
		esac; \
		if $$CC -static -O2 $$extra -o "$$elf" "$$src" 2>/dev/null; then \
			count=$$((count + 1)); \
		fi; \
	done; \
	echo "Cross-compiled $$count musl-static test binaries."
	@python3 scripts/build_asm_tests.py "$(MUSL_CC)"
	@CC="$(MUSL_CC)"; \
	if [ ! -f ctest/nb_testlib.so ] || [ "ctest/nb_testlib.so" -ot "ctest/nb_lib.c" ]; then \
		$$CC -O2 -shared -fPIC -nostdlib ctest/nb_lib.c -o ctest/nb_testlib.so && \
		echo "Built ctest/nb_testlib.so (native bridge test lib)."; \
	fi
	@CC="$(GLIBC_CC)"; \
	count=0; \
	for src in $(GLIBC_DYN_SRCS); do \
		elf="$${src%.c}.elf"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && continue; \
		if $$CC -O2 -o "$$elf" "$$src" 2>/dev/null; then \
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
generated-check: opgen-check opgen-thunk-check wlgen-check vkmarshal-check vkxml-check glxml-check glcoverage-check egl-check opgen-fpfixed-check

# Local guest-correctness gate: generator drift, host APIs, JIT/interpreter
# regressions, sandbox coverage, and a focused dynamic-glibc run.
ci: setup-tests generated-check $(TARGET)
	$(MAKE) test-capi
	$(MAKE) test-nb
	$(MAKE) verify
	./scripts/run_tests.sh --emu $(TARGET) --unit --quick --strict
	./scripts/run_tests.sh --emu $(TARGET) --unit --quick --strict --no-jit
	./scripts/run_tests.sh --emu $(TARGET) --integration --strict --filter '^test_sandbox$$'
	@./scripts/setup-rootfs.sh
	@test -f rootfs/lib/ld-linux-aarch64.so.1 || { echo "error: glibc loader missing from rootfs" >&2; exit 1; }
	./scripts/run_tests.sh --emu $(TARGET) --dynamic --strict --filter '^(hello_dyn_glibc|test_dyn_(write|hello|malloc|printf|pthread_min|threads|pthread_stress|pthread_8thread)|test_dladdr_glibc)$$'

# Full validation provisions the rootfs and requires every selected fixture
# except the optional external iperf3/coreutils bundle, which is reported as
# a visible skip when it is not installed. Setup failures are fatal.
check-all: setup-tests generated-check $(TARGET)
	$(MAKE) test-capi
	$(MAKE) test-nb
	@./scripts/setup-rootfs.sh
	@test -f rootfs/lib/ld-linux-aarch64.so.1 || { echo "error: glibc loader missing from rootfs" >&2; exit 1; }
	@test -f rootfs/lib/ld-musl-aarch64.so.1 || { echo "error: musl loader missing from rootfs" >&2; exit 1; }
	@./scripts/run_tests.sh --emu $(TARGET) --strict --allow-missing '^rw_(iperf3_version|coreutils_)'

# Host-side C API test: test_capi.c links libbifrost.a and runs on the
# HOST (it cannot be cross-compiled as a guest ELF). Builds and runs it.
# Note: the guest hello.elf path is relative to the repo root.
CAPI_TESTLIB := ctest/capi_testlib.so
$(CAPI_TESTLIB): ctest/capi_call_lib.c
	@$(CROSS_CC) -O2 -shared -fPIC -nostdlib $< -o $@
	@echo "Built $@"

test-capi: lib $(TARGET) $(CAPI_TESTLIB)
	@echo "=== Building host C API test ==="
	@mkdir -p $(OBJDIR)
	@$(CXX) -O1 -g -MMD -MP -MF $(OBJDIR)/test_capi_host.d -Iapi -x c -c ctest/test_capi.c -o $(OBJDIR)/test_capi_host.o
	@$(CXX) $(OBJDIR)/test_capi_host.o $(LIB) -o $(OBJDIR)/test_capi_host $(LDFLAGS)
	@echo "=== Running host C API test ==="
	@./$(OBJDIR)/test_capi_host

# Cross-compile the guest AArch64 shared object used by the native bridge
# host test (a -nostdlib -shared lib with JNI-shaped functions; must be
# rebuilt whenever ctest/nb_lib.c changes).
NB_TESTLIB := ctest/nb_testlib.so
$(NB_TESTLIB): ctest/nb_lib.c
	@$(CROSS_CC) -O2 -shared -fPIC -nostdlib ctest/nb_lib.c -o $@
	@echo "Built $@"

# Host-side native bridge test: test_nb.c links libbifrost.a (which
# contains the adapter when libffi is present) and runs on the HOST. It
# drives the NativeBridgeItf table exactly as ART's libnativebridge would:
# loadLibrary the guest .so, getTrampoline per JNI shorty, and call each
# trampoline through the borrow-CPU path.
test-nb: lib $(TARGET) $(NB_TESTLIB)
	@echo "=== Building host native bridge test ==="
	@mkdir -p $(OBJDIR)
	@$(CXX) -O1 -g -MMD -MP -MF $(OBJDIR)/test_nb_host.d -Iapi -x c -c ctest/test_nb.c -o $(OBJDIR)/test_nb_host.o
	@$(CXX) $(OBJDIR)/test_nb_host.o $(LIB) -o $(OBJDIR)/test_nb_host $(LDFLAGS)
	@echo "=== Running host native bridge test ==="
	@./$(OBJDIR)/test_nb_host
