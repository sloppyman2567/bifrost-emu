// Minimal fixture entry: use Bionic's real process initialization and exit.
// __libc_init(raw_stack, unused_onexit, main, zeroed_structors).
.text
.global _start
.type _start, %function
_start:
    mov x0, sp
    sub sp, sp, #64
    stp xzr, xzr, [sp, #0]
    stp xzr, xzr, [sp, #16]
    stp xzr, xzr, [sp, #32]
    stp xzr, xzr, [sp, #48]
    mov x1, xzr
    adrp x2, main
    add x2, x2, :lo12:main
    mov x3, sp
    bl __libc_init
    brk #1
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
