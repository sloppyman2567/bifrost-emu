# bifrost-emu Makefile
# Simple, no autotools. Just `make` to build, `make test` to run tests.
#
# Build options (set on the make command line, e.g. `make USE_SDL2=1`):
#
#   USE_SDL2=1     Enable SDL2 window backend for /dev/fb0.
#                  Requires SDL2 dev headers (libsdl2-dev on Debian,
#                  SDL2-devel on Fedora). Default: headless (no SDL2).
#
#   SDL2_CFLAGS    Override SDL2 compiler flags. Default: $(sdl2-config --cflags)
#   SDL2_LIBS      Override SDL2 linker flags.    Default: $(sdl2-config --libs)
#
#   CXX            C++ compiler (default: g++)
#   CXXFLAGS       C++ compiler flags
#   LDFLAGS        Linker flags

CXX      ?= g++
SRCDIR   := src
INCDIR   := include
CXXFLAGS ?= -O3 -std=c++17 -pthread -Wall -Wextra -I$(INCDIR)
LDFLAGS  ?= -pthread

TARGET   := bifrost-emu
LIB      := libbifrost.a
SOURCES  := $(SRCDIR)/main.cpp $(SRCDIR)/interpreter.cpp $(SRCDIR)/syscalls.cpp \
	    $(SRCDIR)/decoder.cpp $(SRCDIR)/graphics.cpp $(SRCDIR)/signal.cpp \
	    $(SRCDIR)/frostjit.cpp $(SRCDIR)/jit_glue.cpp $(SRCDIR)/ir.cpp \
	    $(SRCDIR)/ir_optimize.cpp $(SRCDIR)/ops.cpp
HEADERS  := $(INCDIR)/arm64_emu.hpp $(INCDIR)/decoder.hpp $(INCDIR)/graphics.hpp \
	    $(INCDIR)/signal.hpp $(INCDIR)/frostjit.hpp $(INCDIR)/ir.hpp api/bifrost.h

# ── SDL2 backend (opt-in) ────────────────────────────────────────────────
ifeq ($(USE_SDL2),1)
    SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
    SDL2_LIBS   ?= $(shell sdl2-config --libs   2>/dev/null)
    CXXFLAGS += $(SDL2_CFLAGS) -DBIFROST_USE_SDL2
    LDFLAGS  += $(SDL2_LIBS)
endif

.PHONY: all test clean install uninstall lib

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

# Build the static library (libbifrost.a) for API consumers
lib: $(LIB)

$(LIB): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/interpreter.cpp -o interpreter.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/syscalls.cpp   -o syscalls.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/decoder.cpp    -o decoder.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/graphics.cpp   -o graphics.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/signal.cpp     -o signal.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/frostjit.cpp   -o frostjit.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/jit_glue.cpp   -o jit_glue.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/ir.cpp         -o ir.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/ir_optimize.cpp -o ir_optimize.o
	$(CXX) $(CXXFLAGS) -c $(SRCDIR)/ops.cpp        -o ops.o
	ar rcs $@ interpreter.o syscalls.o decoder.o graphics.o signal.o frostjit.o jit_glue.o ir.o ir_optimize.o ops.o
	@echo "Built $@ (note: this skips main.cpp; link your own driver)"

# Debug build with sanitizers
debug: CXXFLAGS = -O0 -g -std=c++17 -pthread -Wall -Wextra -fsanitize=address,undefined -I$(INCDIR)
debug: LDFLAGS = -pthread -fsanitize=address,undefined
debug: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $(TARGET)-dbg $(SOURCES) $(LDFLAGS)

# Run all test programs
test: $(TARGET)
	@echo "--- Running test suite ---"
	@for f in test/*.elf; do \
	    echo "--- $$f ---"; \
	    ./$(TARGET) $$f || echo "FAILED: $$f"; \
	done

# Install to /usr/local/bin
install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET)-dbg *.o $(LIB)
