# bifrost-emu Makefile (v1.4.0-rc.0)
#
# Auto-discovers all .cpp files under src/ and compiles them into the
# final bifrost-emu binary. Library builds (libbifrost.a) compile the
# same sources minus main.cpp.
#
# Build options (set on the make command line, e.g. `make USE_SDL2=1`):
#
#   USE_SDL2=1     Enable SDL2 window backend for /dev/fb0.
#                  Requires SDL2 dev headers. Either install libsdl2-dev
#                  system-wide, or run ./tools/fetch-sdl2-headers.sh to
#                  download them via apt-get download (no sudo needed).
#                  Default: headless (no SDL2).
#
#   SDL2_CFLAGS    Override SDL2 compiler flags. Default: $(sdl2-config --cflags)
#   SDL2_LIBS      Override SDL2 linker flags.    Default: $(sdl2-config --libs)
#
#   CXX            C++ compiler (default: g++)
#   CXXFLAGS       C++ compiler flags
#   LDFLAGS        Linker flags
#
# Directory layout (v1.4.0-beta.3+):
#
#   src/core/         — Emulator, Memory, CPU, SignalTable, thread_mgr
#   src/vfs/          — VFS abstraction (VNode + FdTable + procfs + devfs)
#   src/ir/           — IR builder/translator/optimizer/executor
#   src/jit/          — FrostJIT (split into x86_backend, x86_regalloc,
#                       jit_cache, jit_profiler, frostjit, jit_glue)
#   src/syscalls/     — Linux AArch64 syscall layer (split by concern)
#   src/frontend/     — decoder + ELF loader
#   src/graphics/     — /dev/fb0 backend (headless or SDL2)
#   src/interp/       — switch-based instruction interpreter

CXX      ?= g++
INCDIR   := include
CXXFLAGS ?= -O3 -std=c++17 -pthread -Wall -Wextra -I$(INCDIR) -Isrc -MMD -MP
LDFLAGS  ?= -pthread

TARGET   := bifrost-emu
LIB      := libbifrost.a

# Auto-discover all .cpp under src/, plus main.cpp at the root.
SRC_DIRS := src/core src/vfs src/ir src/jit src/syscalls src/frontend src/graphics src/interp src/audio
SOURCES  := $(shell find $(SRC_DIRS) -name '*.cpp') main.cpp
OBJDIR   := build
OBJECTS  := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)   # auto-generated header dependency files

# Library objects (everything except main.cpp)
LIB_SOURCES := $(filter-out main.cpp,$(SOURCES))
LIB_OBJECTS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(LIB_SOURCES))

HEADERS  := $(shell find include src -name '*.hpp' -o -name '*.h')

# ── SDL2 backend (opt-in) ────────────────────────────────────────────────
ifeq ($(USE_SDL2),1)
    SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
    SDL2_LIBS   ?= $(shell sdl2-config --libs   2>/dev/null)
    CXXFLAGS += $(SDL2_CFLAGS) -DBIFROST_USE_SDL2
    LDFLAGS  += $(SDL2_LIBS)
endif

.PHONY: all test clean install uninstall lib debug

all: $(TARGET)

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
	      */echo.elf|*/repl.elf|*/sh.elf|*/fgets_test.elf|*/yes.elf|*/cat.elf) \
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

# Install to /usr/local/bin
install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TARGET)-dbg *.o $(LIB)
