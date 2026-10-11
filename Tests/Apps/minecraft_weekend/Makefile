# minecraft_weekend — cross-compile the GLFW game for the emulator.
#
# GLFW and GL are NOT linked natively; guest_glfw.c resolves glfw_* at
# runtime against the emulator's thunked host libglfw.so.3, and GLAD
# resolves gl_* via glfwGetProcAddress (also thunked). cglm and stb are
# header-only; glad.c and noise1234.c are compiled in.
CROSS_CC := ../../tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc

GAME_SRCS := $(shell find src -name '*.c')
SRCS := $(GAME_SRCS) \
	lib_glad/src/glad.c \
	lib_noise/noise1234.c \
	guest_glfw.c

CFLAGS := -O2 -Isrc -Iglfw_shim -Ilib_glad/include -Ilib_cglm/include -Ilib_noise -Ilib_stb

all: minecraft_weekend.elf

minecraft_weekend.elf: $(SRCS)
	$(CROSS_CC) -static $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f minecraft_weekend.elf

.PHONY: all clean
