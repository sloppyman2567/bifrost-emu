// Freestanding static AArch64 image with no PT_INTERP: write(1, ...) then exit_group(7). The kernel
// runs such an image itself with AT_BASE 0, as it does a static Bionic executable or linker64.
.text
.global _start
.type _start, %function
_start:
    mov x0, #1
    adr x1, message
    mov x2, #message_size
    mov x8, #64
    svc #0
    mov x0, #7
    mov x8, #94
    svc #0
message:
    .ascii "android_static_image: ran without PT_INTERP\n"
.set message_size, . - message
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
