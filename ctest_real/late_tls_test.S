// Regression for a static executable with no startup PT_TLS that creates a
// thread before dlopen'ing TLS-bearing shared objects. The modules cover
// traditional GD access, static-surplus TLSDESC, and dynamically allocated
// TLSDESC. Each module is checked on the main TCB and a pre-existing TCB.

.section .rodata
path_gd:          .asciz "/tmp/bifrost_late_tls/liblate_tls_gd.so"
symbol_gd:        .asciz "late_tls_gd_addr"
path_desc_static: .asciz "/tmp/bifrost_late_tls/liblate_tls_desc_static.so"
symbol_desc_static: .asciz "late_tls_desc_static_addr"
path_desc_dynamic: .asciz "/tmp/bifrost_late_tls/liblate_tls_desc_dynamic.so"
symbol_desc_dynamic: .asciz "late_tls_desc_dynamic_addr"
path_rejected:    .asciz "/tmp/bifrost_late_tls/liblate_tls_rejected.so"
symbol_rejected:  .asciz "late_tls_rejected_marker"

.section .text
.macro TEST_MODULE path_label, symbol_label
    adrp x0, \path_label
    add x0, x0, :lo12:\path_label
    mov x1, #2
    mov x8, #0x1002
    svc #0
    cbz x0, .Lfail
    mov x19, x0

    adrp x1, \symbol_label
    add x1, x1, :lo12:\symbol_label
    mov x0, x19
    mov x8, #0x1003
    svc #0
    cbz x0, .Lfail
    mov x20, x0

    // Main thread sees the initialized value, then gets a private value.
    blr x20
    ldr w1, [x0]
    movz w2, #0x9bdf
    movk w2, #0x1357, lsl #16
    cmp w1, w2
    b.ne .Lfail
    movz w3, #0xace0
    movk w3, #0x2468, lsl #16
    str w3, [x0]

    // The TCB was allocated before dlopen. Its TLS must be initialized on
    // demand and remain independent from the main thread's TLS instance.
    mrs x23, tpidr_el0
    mrs x24, tpidrro_el0
    msr tpidr_el0, x22
    msr tpidrro_el0, x22
    blr x20
    ldr w1, [x0]
    movz w2, #0x9bdf
    movk w2, #0x1357, lsl #16
    cmp w1, w2
    b.ne .Lrestore_fail
    movz w3, #0x1357
    movk w3, #0x2468, lsl #16
    str w3, [x0]
    msr tpidr_el0, x23
    msr tpidrro_el0, x24

    blr x20
    ldr w1, [x0]
    movz w2, #0xace0
    movk w2, #0x2468, lsl #16
    cmp w1, w2
    b.ne .Lfail
.Lrestore_done_\@:
.endm

.global _start
.type _start, %function
_start:
    // Keep a TCB alive before the first TLS module exists.
    mov x0, #0
    mov x8, #0x1001
    svc #0
    cmp x0, #0
    b.le .Lfail
    mov x22, x0

    // This DSO requests initial-exec TLS beyond the available static
    // surplus. dlopen must fail cleanly and must not publish its symbol.
    adrp x0, path_rejected
    add x0, x0, :lo12:path_rejected
    mov x1, #2
    mov x8, #0x1002
    svc #0
    cbnz x0, .Lfail
    mov x0, #0 // RTLD_DEFAULT
    adrp x1, symbol_rejected
    add x1, x1, :lo12:symbol_rejected
    mov x8, #0x1003
    svc #0
    cbnz x0, .Lfail

    TEST_MODULE path_gd, symbol_gd
    TEST_MODULE path_desc_static, symbol_desc_static
    TEST_MODULE path_desc_dynamic, symbol_desc_dynamic

    mov x0, #0
    mov x8, #93
    svc #0

.Lfail:
    mov x0, #1
    mov x8, #93
    svc #0

.Lrestore_fail:
    msr tpidr_el0, x23
    msr tpidrro_el0, x24
    b .Lfail
