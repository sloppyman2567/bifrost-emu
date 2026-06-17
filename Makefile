# bifrost-emu Makefile
# Simple, no autotools. Just `make` to build, `make test` to run tests.

CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -pthread -Wall -Wextra
LDFLAGS  ?= -pthread

TARGET   := bifrost-emu
LIB      := libbifrost.a
SOURCES  := main.cpp interpreter.cpp syscalls.cpp decoder.cpp graphics.cpp
HEADERS  := arm64_emu.hpp decoder.hpp graphics.hpp api/bifrost.h

.PHONY: all test clean install uninstall lib

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

# Build the static library (libbifrost.a) for API consumers
lib: $(LIB)

$(LIB): interpreter.cpp syscalls.cpp decoder.cpp graphics.cpp arm64_emu.hpp decoder.hpp graphics.hpp
	ar rcs $@ interpreter.o syscalls.o decoder.o graphics.o
	@echo "Note: run 'make lib' after building object files with 'make objects'"

# Debug build with sanitizers
debug: CXXFLAGS = -O0 -g -std=c++17 -pthread -Wall -Wextra -fsanitize=address,undefined
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

# Assemble test programs from .s sources
tests-assemble:
	@for f in test/*.s; do \
	    out="$${f%.s}.elf"; \
	    echo "Assembling $$f -> $$out"; \
	    python3 mini_arm64_asm.py $$f -o $$out; \
	done

# Install to /usr/local/bin
install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET)-dbg *.o $(LIB)
