; hello.s - prints "Hello, ARM64!" and exits 0

_start:
    ; write(1, msg, 14)
    mov  x0, #1
    adr  x1, msg
    mov  x2, #14
    mov  x8, #64
    svc  #0

    ; exit(0)
    mov  x0, #0
    mov  x8, #93
    svc  #0

    nop
    nop
    nop

msg:
    .ascii "Hello, ARM64!\n"
