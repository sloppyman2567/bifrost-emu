# bifrost-emu Makefile (v1.5.1-alpha)
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
# Directory layout (v1.5.1-alpha):
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

TARGET   := bifrost-emu
LIB      := libbifrost.a

# Auto-discover all .cpp under src/, plus main.cpp at the root.
# api/bifrost_capi.cpp is included in LIB_SOURCES so libbifrost.a exposes
# the C API (bifrost.h).
SRC_DIRS := src/core src/yggdrasil src/ir src/jit src/syscalls src/frontend src/frost_graphics src/interp src/audio
SOURCES  := $(shell find $(SRC_DIRS) -name '*.cpp') main.cpp
OBJDIR   := build
OBJECTS  := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)   # auto-generated header dependency files

# Library objects (everything except main.cpp, PLUS the C API wrapper)
LIB_SOURCES := $(filter-out main.cpp,$(SOURCES)) api/bifrost_capi.cpp
LIB_OBJECTS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(LIB_SOURCES))

HEADERS  := $(shell find include src -name '*.hpp' -o -name '*.h')

# Default to SDL2/GL for standard builds; override with USE_SDL2=0 / USE_THUNK_GL=0
USE_SDL2 ?= 1
USE_THUNK_GL ?= 1

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

.PHONY: all opgen opgen-check test clean install uninstall lib debug setup setup-tests check-all

all: $(TARGET)

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

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

# Pattern rule: compile any .cpp under src/ or main.cpp to .o in build/
# Header dependencies are auto-tracked via -MMD -MP (see DEPS above).
$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Include auto-generated header dependencies (silently ignore if missing).
-include $(DEPS)

# Build the static library (libbifrost.a) for API consumers
lib: $(LIB)

$(LIB): $(LIB_OBJECTS)
	ar rcs $@ $^
	@echo "Built $@ (excludes main.cpp; link your own driver)"

# Debug build with sanitizers (no -MMD to keep build/ clean for release)
debug: CXXFLAGS = -O0 -g -std=c++17 -pthread -Wall -Wextra -fsanitize=address,undefined -I$(INCDIR) -Isrc
debug: LDFLAGS = -pthread -fsanitize=address,undefined
debug: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET)-dbg $(LDFLAGS)

# ── Test runner ────────────────────────────────────────────────────────
# `make check` runs the standalone test script (scripts/run_tests.sh),
# which categorizes tests, colorizes output, and prints a summary table.
# Supports filters: `make check ARGS="--toybox"` or `make check ARGS="--filter md5"`.
# See `./scripts/run_tests.sh --help` for all options.
.PHONY: check check-quick check-nojit check-fwd
check: $(TARGET)
	./scripts/run_tests.sh $(ARGS)

check-quick: $(TARGET)
	./scripts/run_tests.sh --quick

check-nojit: $(TARGET)
	./scripts/run_tests.sh --no-jit --quick

check-fwd: $(TARGET)
	./scripts/run_tests.sh --fwd --quick

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

# Run JIT tests under BIFROST_JIT_VERIFY=1 — catches JIT/interpreter
# divergences by running each block through both paths and comparing
# CPU state. Slow (10-50x), but catches codegen bugs that produce
# wrong results without crashing. Use this after any JIT codegen change.
#
# The recipe uses ${PIPESTATUS[0]} (a bash array) to capture the
# emulator's exit code before the grep pipe consumes it. /bin/sh on
# Debian is dash, which doesn't support PIPESTATUS, so we force SHELL
# to /bin/bash for this target (and any other that needs bashisms).
SHELL := /bin/bash
verify: $(TARGET)
	@echo "--- JIT verify mode (divergence check) ---"
	@for f in ctest/jit_*.elf; do \
	echo "--- $$f (verify) ---"; \
	BIFROST_JIT_VERIFY=1 timeout 30 ./$(TARGET) $$f </dev/null 2>&1 | \
	grep -E 'VERIFY.*DIVERGENCE.*pc|VERIFY.*x[0-9]+: jit' | head -3; \
	echo "  (rc=$${PIPESTATUS[0]})"; \
	done
	@echo "Done. Any DIVERGENCE lines above indicate JIT codegen bugs."


# Cross-compile a test program with the bundled musl toolchain.
# Usage: make cross SRC=ctest_real/hello.c OUT=ctest_real/hello.elf
CROSS_CC := tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
cross:
	@if [ -z "$(SRC)" ] || [ -z "$(OUT)" ]; then \
	echo "Usage: make cross SRC=<file.c> OUT=<file.elf>"; exit 1; \
	fi
	@$(CROSS_CC) -static -O2 -o $(OUT) $(SRC)
	@echo "Built $(OUT)"

# Install to /usr/local/bin (override with `make install DESTDIR=/prefix PREFIX=/opt`)
PREFIX ?= /usr/local
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TARGET)-dbg *.o $(LIB)

# ── One-click setup ───────────────────────────────────────────────────
# `make setup` runs the bundled bootstrap script: builds the emulator,
# fetches the musl toolchain (if missing), cross-compiles every test,
# sets up the rootfs, and runs the test suite. Idempotent.
setup: $(TARGET)
	./scripts/setup.sh

# `make setup-tests` only fetches the toolchain and cross-compiles the
# test .elf files — it does NOT run the test suite or build the rootfs.
# Useful for CI jobs that want to build tests once and run them later.
setup-tests:
	@if [ ! -x tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc ]; then \
		echo "Fetching musl toolchain ..."; \
		./tools/fetch-musl-toolchain.sh; \
	fi
	@CC=tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc; \
	count=0; \
	for src in ctest/*.c ctest_real/*.c; do \
		[ -f "$$src" ] || continue; \
		elf="$${src%.c}.elf"; \
		[ -f "$$elf" ] && [ "$$elf" -nt "$$src" ] && continue; \
		if $$CC -static -O2 -o "$$elf" "$$src" 2>/dev/null; then \
			count=$$((count + 1)); \
		fi; \
	done; \
	echo "Cross-compiled $$count test binaries."

# `make check-all` is the "everything" target: build, fetch toolchain,
# cross-compile tests, set up rootfs, and run the full test suite.
# This is what CI should run for a complete validation pass.
check-all: setup-tests $(TARGET)
	@./scripts/setup-rootfs.sh 2>/dev/null || true
	@./scripts/run_tests.sh
