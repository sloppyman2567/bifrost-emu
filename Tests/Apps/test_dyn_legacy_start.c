/* test_dyn_legacy_start.c — constructors of a legacy-start glibc program.
 *
 * Programs linked before glibc 2.34 (e.g. Geekbench) import
 * __libc_start_main@GLIBC_2.17 and pass a non-NULL `init` that runs the
 * executable's DT_INIT_ARRAY themselves; ld.so never runs the main
 * executable's initializers. This binary has its own _start doing the
 * same, so a dynamic linker that also runs the main init array runs
 * every constructor twice.
 *
 * Build: aarch64-linux-gnu-gcc -O2 -nostartfiles -o test_dyn_legacy_start.elf test_dyn_legacy_start.c
 *   (must be built with the glibc toolchain, dynamically linked)
 * Run:   BIFROST_ROOT=rootfs ./bifrost-emu test_dyn_legacy_start.elf
 * Pass:  exit 0.
 */
#include <stdio.h>

typedef void (*init_fn)(int, char **, char **);
extern init_fn __init_array_start[], __init_array_end[];

int __libc_start_main_legacy(int (*)(int, char **, char **), int, char **,
                             void (*)(int, char **, char **), void (*)(void),
                             void (*)(void), void *);
__asm__(".symver __libc_start_main_legacy, __libc_start_main@GLIBC_2.17");

static int ctor_runs;
__attribute__((constructor)) static void count_ctor(void) { ctor_runs++; }

/* What __libc_csu_init did in old crt files. */
__attribute__((used, visibility("hidden"))) void legacy_init(int argc, char **argv, char **envp) {
    for (init_fn *f = __init_array_start; f < __init_array_end; f++)
        (*f)(argc, argv, envp);
}

__attribute__((used, visibility("hidden"))) int real_main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    printf("test_dyn_legacy_start: constructor ran %d time(s)\n", ctor_runs);
    if (ctor_runs != 1) {
        printf("test_dyn_legacy_start: FAIL\n");
        return 1;
    }
    printf("test_dyn_legacy_start: ALL PASS\n");
    return 0;
}

/* aarch64 sysdeps start.S, with an explicit init argument (x3). */
__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "    mov x29, #0\n"
    "    mov x30, #0\n"
    "    mov x5, x0\n"                         /* rtld_fini */
    "    ldr x1, [sp]\n"                       /* argc */
    "    add x2, sp, #8\n"                     /* argv */
    "    mov x6, sp\n"                         /* stack_end */
    "    adrp x0, real_main\n"
    "    add x0, x0, :lo12:real_main\n"
    "    adrp x3, legacy_init\n"
    "    add x3, x3, :lo12:legacy_init\n"
    "    mov x4, #0\n"                         /* fini */
    "    bl __libc_start_main_legacy\n"
    "    brk #0\n"
    ".size _start, .-_start\n");
