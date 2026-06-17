; fib.s - computes the 30th Fibonacci number and prints it.
; Tests: MUL, MADD, MSUB, conditional branches, loops, multi-digit print.

_start:
    ; Compute fib(30) iteratively.
    ; a = 0, b = 1
    mov  x19, #0                ; a
    mov  x20, #1                ; b
    mov  x21, #30               ; counter

fib_loop:
    ; tmp = a + b
    add  x22, x19, x20
    ; a = b
    mov  x19, x20
    ; b = tmp
    mov  x20, x22
    ; counter--
    sub  x21, x21, #1
    ; if counter > 0, loop
    cmp  x21, #0
    b.gt fib_loop

    ; x19 = fib(30) = 832040
    ; Now print it as decimal.
    ; We divide by 10 repeatedly, collecting digits.
    adr  x23, buf_end           ; x23 points just past the buffer end
    mov  x24, x19               ; working copy
    mov  x25, #10               ; divisor

    ; Handle 0 specially
    cmp  x24, #0
    b.ne not_zero
    sub  x23, x23, #1
    mov  w26, #48               ; '0'
    strb w26, [x23]
    b print_digits

not_zero:
digit_loop:
    cmp  x24, #0
    b.eq print_digits
    ; x25 = x24 % 10, x24 = x24 / 10
    ; We have UDIV/MSUB in our CPU.
    udiv x27, x24, x25          ; quotient
    msub x28, x27, x25, x24     ; remainder = x24 - (quotient * 10)
    mov  x24, x27               ; x24 = quotient
    add  w28, w28, #48          ; to ASCII
    sub  x23, x23, #1
    strb w28, [x23]
    b digit_loop

print_digits:
    ; print from x23 to buf_end
    mov  x0, #1
    mov  x1, x23
    adr  x2, buf_end
    sub  x2, x2, x23
    mov  x8, #64
    svc  #0

    ; print newline
    mov  x0, #1
    adr  x1, newline
    mov  x2, #1
    mov  x8, #64
    svc  #0

    ; exit(0)
    mov  x0, #0
    mov  x8, #93
    svc  #0

newline:
    .byte 10
buf:
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
    .byte 0
buf_end:
