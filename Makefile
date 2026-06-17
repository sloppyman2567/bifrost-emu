# bifrost-emu Makefile
# Simple, no autotools. Just `make` to build, `make test` to run tests.

CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -pthread -Wall -Wextra
LDFLAGS  ?= -pthread

TARGET   := bifrost-emu
SOURCES  := main.cpp arm64_emu.cpp
HEADERS  := arm64_emu.hpp

.PHONY: all test clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

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
	rm -f $(TARGET) $(TARGET)-dbg *.o
